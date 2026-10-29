# HTTP API v1

默认服务地址为 `http://127.0.0.1:8383`。服务提供工具检索与文本向量接口，独立工具不会执行匹配到的算法、Shell 命令或文件操作。

请求与响应均使用 UTF-8 JSON。配置 API token 后，除 `/health` 外的接口需要 `Authorization: Bearer <token>`。未配置 token 时不要求认证，服务仍限定本机回环监听。接口供 QGIS、GUI、CLI 等本地 HTTP 客户端调用，带 `Origin` 请求头的网页跨来源请求会被拒绝。客户端应同时检查 HTTP 状态和 JSON 中的任务状态，HTTP 202 只表示任务已接收。

## 接口列表

| 方法与路径 | 请求 | 返回 |
|---|---|---|
| `GET /health` | 无 | 服务可访问状态，不代表模型已加载或索引已就绪 |
| `GET /v1/models` | 无 | 模型列表 |
| `POST /v1/embeddings` | 模型 ID 与字符串或字符串数组 | 同步返回 OpenAI 兼容 float 向量 |
| `PUT /v1/catalogs/{source_id}` | 目录快照 | HTTP 202 与索引任务 ID |
| `POST /v1/search` | 自然语言需求、Top N、过滤条件 | HTTP 202 与查询任务 ID |
| `POST /v1/records/get` | `{"ids":["记录或能力 ID"]}` | 同步返回完整记录与原始 JSON |
| `GET /v1/jobs/{job_id}` | 无 | 当前任务快照 |
| `POST /v1/jobs/{job_id}/cancel` | 空对象 | 取消请求后的任务快照 |

## 文本向量

```json
{
  "model": "embeddinggemma-2-Q8_0",
  "input": [
    "task: search result | query: 将栅格重投影并重采样",
    "title: GDAL Warp | text: Raster reprojection and resampling."
  ],
  "encoding_format": "float"
}
```

接口不自动添加检索前缀。直接调用该接口时，由调用方决定输入文本。通过 `/v1/search` 查询或同步工具目录时，共享检索核心负责生成一致的 query/document 前缀。

每次请求接收 1 到 128 个非空字符串，每个字符串最多 131,072 个字符。该 HTTP 大小限制不代替模型的 token 上限，模型上下文可能先达到限制。

成功响应的 `object` 为 `list`，`data` 中每项包含 `object: "embedding"`、对应输入的 `index` 和 float 数组 `embedding`。当前锁定的 EmbeddingGemma 2 profile 使用 768 维、mean pooling 与 L2 归一化。客户端仍应校验模型身份、输入输出数量、索引唯一性、向量维度和有限非零数值。只支持文本输入及 float 编码，不支持 token ID 输入、base64 向量或多模态 content 对象。

请求和成功响应结构分别见 [embeddings-request.schema.json](../schemas/embeddings-request.schema.json) 与 [embeddings-response.schema.json](../schemas/embeddings-response.schema.json)。超出模型上下文限制时返回错误，不用零向量表示成功。

## 导入目录与索引状态

`PUT /v1/catalogs/my-gis-tools` 的推荐请求体为 [JSON 目录文档](json-catalog.md) 中的完整快照。URL 中的 source ID 为 1 到 256 个字符，并作为本次导入的来源。请求体应使用相同 `source_id`。服务还接受原始记录对象或数组的便捷导入，自动分配 revision。便捷导入中，带 `inputSchema`、`outputSchema` 或参数 schema 的原始对象按定义处理。其他对象含 `arguments` 或 `parameters` 时按示例处理，其余按定义处理。要精确指定种类，请显式提供 `kind` 与 `payload`。跨客户端长期同步应使用带明确 revision 的完整快照。

完整快照支持可选的 `expected_revision` 和 `force_rebuild`。`expected_revision` 为字符串时必须匹配当前来源 revision，为 `null` 时要求来源尚不存在，省略时没有此并发前置条件。冲突会在任务结果中返回 `source_revision_conflict`，客户端应重新读取或导出当前目录后处理，不自动覆盖较新的快照。`force_rebuild: true` 为当前模型 profile 重新计算可用记录，同时保留仍与当前文档兼容的旧向量。它不绕过并发检查。两字段不计入目录内容指纹，可对同一 revision 的相同内容发起重建。

任务接收响应如下：

```json
{"status":"queued","job_id":"由服务生成的标识"}
```

客户端用该 ID 查询任务。终态的 `result` 是共享核心返回的结果，索引结果包括 `source_id`、`revision`、`record_count`、`embedded_records`、`pending_records`、`generation` 和 `embedding_profile`。

目录与向量的提交不是一个覆盖整个远端推理过程的大事务。目录变更先发布，删除或禁用记录立即影响后续查询。向量按实际完成情况写入。如果任务因嵌入失败而终止，`metadata_applied: true` 表示目录已更新，`pending_records` 表示仍待计算的记录。客户端可在服务恢复后核对当前目录，再提交相同内容的快照，已有兼容向量会复用。若原请求包含 `expected_revision`，必须按核对后的当前 revision 更新该前置条件，不能原样重试已成功创建来源的 `null` 条件或旧 revision。另一快照在推理期间取代该来源时，迟到向量不会发布，任务返回 `source_changed`。

`GET /v1/index/status` 是辅助管理接口，可查看当前本地数据库的来源、记录、兼容向量与 generation。它不替代具体索引任务的终态检查。

## 搜索与结果

```json
{
  "query": "把高程栅格用 Lanczos 重采样，保存为压缩 GeoTIFF",
  "top_n": 8,
  "filters": {"source_id": "my-gis-tools", "available": true}
}
```

`query` 接收非空字符串，最多 8,000 个字符。`top_n` 为 1 到 100 的整数，默认 8。`filters.kind` 可选 `definition` 或 `example`，省略则检索两类。`available` 默认 `true`，显式设为 `false` 时只查询不可用记录，适合目录检查。

查询是异步任务。`GET /v1/jobs/{job_id}` 返回 `status: "succeeded"` 后，从 `result` 读取搜索结果，而不是从接收任务的响应中读取 candidates。

搜索结果包含：

- `generation` 和 `embedding_profile`，标识本次读取的目录版本与向量空间。
- `retrieval_mode`，为 `hybrid` 或 `lexical`。降级时提供 `degradation_reason`。
- `candidates`，按相关性排序并按来源与能力分组。
- 每个候选的原始 `payload` 和最多八条 `matched_records`，便于调用方检查定义及示例。
- `score`、`semantic_score` 与 `lexical_score`。`score_kind` 为 `rrf_not_probability`，得分不表示操作成功概率。

见 [search-request.schema.json](../schemas/search-request.schema.json) 与 [search-response.schema.json](../schemas/search-response.schema.json)。成功且候选为空是有效结果。读取完整记录使用 `/v1/records/get`，每次可提供 1 到 100 个记录或能力 ID，同一能力可能返回多条关联记录。

## 任务、取消与错误

任务状态为 `queued`、`running`、`cancelling`、`succeeded`、`failed` 或 `cancelled`。后三种为终态。取消请求只表达意图，`cancelling` 仍需继续查询，不能把接收取消请求当作已停止推理或写入。

核心失败通常包含 `status: "failed"`、`error_code` 与 `message`，并可能保留已完成工作的数量。常见错误包括目录格式不合法、重复记录 ID、来源 revision 冲突、数据库正被其他进程写入、模型或嵌入结果无效。调用方不要因为 HTTP 请求成功就把嵌套 `result.status: "failed"` 当成成功。

任务快照持久保存到服务 profile 目录的 `jobs.json`，与共享 SQLite 目录分开。使用同一 profile 重启服务后，已恢复的旧任务 ID 仍可查询。原先的 `succeeded`、`failed` 和 `cancelled` 终态及结果保持不变。停机时尚未终结的任务在恢复时统一变为 `status: "failed"`，其 `result` 包含 `status: "failed"`、`error_code: "interrupted"` 和说明，外层快照也包含该错误码。`interrupted` 是错误码，不是新的任务状态。服务不会重新排队或自动继续这些任务。正常关闭时已完成取消的任务恢复后仍为 `cancelled`。

任务历史有容量限制，最多恢复 256 条，新增任务可能淘汰已终结记录。日志文件不可读、损坏或超过 32 MiB 时不能保证恢复任务历史，因此任务 ID 不代替持久目录身份。发生中断后，应通过 `/v1/index/status` 和完整记录核对实际已发布的目录及向量，再发起新的请求，并重新确定 `expected_revision`。任务结构见 [job.schema.json](../schemas/job.schema.json)。

## 模型与服务管理

辅助接口服务于本地 GUI 和 CLI：

| 方法与路径 | 用途 |
|---|---|
| `GET /v1/model/info` | 当前模型信息 |
| `POST /v1/model/download`，请求 `{}` | 提交锁定模型下载任务 |
| `POST /v1/model/import`，请求 `{"path":"本地 GGUF 路径"}` | 校验并导入已有模型，返回任务 |
| `POST /v1/shutdown` | 请求当前服务正常退出 |

模型下载、导入和索引任务均应查询终态并处理错误。模型权重不随目录 JSON 上传。QGIS 使用远端 llama-server 时仍将目录与向量存入本地数据库，使用本地 QCTSM 服务时仅更换 embedding provider 地址。两种方式共享模型 profile 时可复用同一本地索引。
