# 集成契约 v1

本文件定义独立工具与 QGIS 使用的共享接口。公开接口与数据库版本需要同步维护。

## 共享 C++ SDK

头文件 `src/core/retrieval.h`，命名空间 `qctsm`。CMake targets `qctsm-retrieval` 和 `qctsm-http-provider`，aliases `qctsm::retrieval` / `qctsm::http_provider`。

`EmbeddingProfile` 保存 modelId、revision、dimensions，提供 `fingerprint()`、`toJson()`、`fromJson()`。指纹包括 mean/L2、query/document 提示版本和文档规范化版本，不包含 endpoint。相同模型 revision 的本地及远端 provider 可以共享向量。

`EmbeddingProvider::embed(const QStringList &, const std::atomic_bool *, QString *) -> QList<QVector<float>>` 接收原始文本，返回按输入顺序排列的真实向量。调用发生在后台线程。错误写 error 并返回空列表。

`RetrievalEngine(const QString &databasePath, std::shared_ptr<EmbeddingProvider>, const EmbeddingProfile &)`。

- `QJsonObject syncCatalog(const QJsonObject &snapshot, const std::atomic_bool *cancel = nullptr)`
- `QJsonObject search(const QJsonObject &request, const std::atomic_bool *cancel = nullptr)`
- `QJsonObject getRecords(const QStringList &ids)`
- `QJsonObject status()`

同步 SDK API 由应用、QGIS native runtime 的后台 job 包装成异步。结果统一含 `status`，错误含 `error_code` 和 `message`。不抛异常跨越 Qt 边界。成功状态为 `succeeded`。

`HttpEmbeddingProvider` 头文件 `src/provider_http/http_embedding_provider.h`，构造 `(const QUrl &apiBase, const QString &modelId, const QByteArray &authorization = {}, int timeoutMs = 60000)`。apiBase 以 /v1/ 结束。provider 使用调用线程自己的 network event loop，禁止跨线程共享 QNetworkAccessManager。

## 数据格式

目录 `{schema_version:1, source_id:string, revision:string, records:[]}`。
记录 `{record_id:string, kind:definition|example, capability_id:string, payload:object, name?:string, description?:string, tags?:string[], available?:bool, metadata?:object}`。
definition payload 可为 MCP descriptor 或 Processing/Binary 详情。example payload 保留原始调用 JSON。缺少 record_id 时允许通过标准化内容 hash 生成。capability_id 可从 binary_id/algorithm_id/name 提取，否则使用 record_id。
查询 `{query:string, top_n?:8, filters?:{source_id?:string, kind?:string, available?:bool}}`。
结果 `{status:succeeded, generation:integer, embedding_profile:object, retrieval_mode:hybrid|lexical, candidates:[{capability_id,record_id,name,description,score,semantic_score,lexical_score,payload,matched_records:[]}], ...}`。

数据库使用同一个持久 schema。QGIS 与独立工具默认使用 GenericDataLocation 下的 `QCopilots/ToolSemanticMatching/index.sqlite`，也允许配置同一个自定义本地路径。使用 WAL、busy_timeout 和跨进程 writer lock。embedding 计算不持有 SQLite 写事务或目录写锁。每次查询观察最新 generation，避免跨进程缓存陈旧。profile 不一致不得混用向量。confirmed_risk/client_request_id 不参与语义文档，原始 payload 不修改。

目录可带 `expected_revision` 实现乐观并发控制。字符串表示调用方预期的当前版本，null 表示该来源必须尚不存在。冲突后重新读取或导出当前目录，不自动重放过期快照。未提供该字段的显式目录替换按到达顺序生效。`force_rebuild:true` 在保留现有可用向量的同时重新计算，迟到结果仍须通过来源版本、文档 hash 与模型指纹校验。

## QGIS 设置 v1

QgsSettings Plugins section key `QCopilots/servicesV1` 保存 JSON 字符串：
`{schema_version:1,web_url,planner:{api_base_mode:follow_web|explicit,api_base_url,model_id,authcfg},embedding:{endpoint_mode:follow_planner|explicit,api_base_url,model_id,revision,authcfg},retrieval:{database_path,top_n:8},tool_mode:gateway|full,timeout_seconds:60}`。
旧 `QCopilots/serverUrl` 仍为网页地址投影。新安装默认远端 URL `https://1692fe05.r27.cpolar.top`，旧设置原值优先。默认模型 Qwen3.8-27B-UD-Q8_K_XL 和 embeddinggemma-2-Q8_0。

进程内 QObject objectName `QCopilotsSemanticRuntimeV1`，不注册 WebChannel。Q_INVOKABLE `QString submit(QString requestJson)`, `QString snapshot(QString jobId)`, `QString cancel(QString jobId)`。请求含 operation `sync_catalog|search|get_records|index_status|chat` 和 payload。submit 只入队，返回 `{status:queued,job_id}`。snapshot 返回 `{status:queued|running|succeeded|failed|cancelled,result?:object,error_code?:string,message?:string}`。chat 的 payload 为 OpenAI 请求体，运行时使用 planner 端点/认证。每任务捕获配置快照。

Python bridge 增加线程安全短路由，不能在主线程等待检索/模型/状态长轮询。网关 handler 只调用 bridge。完整 capability registry 和 workflow 状态位于 QGIS 进程。

QGIS 内部提交可带 `configuration_snapshot`，用于同一工作流继续使用创建时保存的配置。该字段由 QGIS 后端生成，不从外部 MCP 参数接收，也不放入模型请求正文。修改设置只影响新任务。

## 网关八工具

qcopilots_search_capabilities, qcopilots_describe_capabilities, qcopilots_execute_capability, qcopilots_prepare_workflow, qcopilots_start_workflow, qcopilots_get_job, qcopilots_respond_workflow, qcopilots_cancel_job。
typed handle `{kind:search|call|workflow,id,qgis_instance_id}`，所有需要模型后续调用的字段同时写入 content text 和 structuredContent。
