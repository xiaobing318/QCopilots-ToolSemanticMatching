# 部署与共享数据库

## 两种 QGIS 部署方式

| 场景 | 聊天与规划 | 向量生成 | 检索数据库 |
| --- | --- | --- | --- |
| 统一远端服务 | 远端 llama-server | 同一远端 `/v1/embeddings` | 本机 SQLite |
| 远端聊天、本地嵌入 | 远端 llama-server | 本机 qctsm-service 内嵌 llama.cpp | 同一本机 SQLite |

QGIS 使用 HTTP embedding provider，既可以连接另一台设备，也可以连接本机独立工具。统一远端部署不要求启动本地 qctsm-service。工具目录与向量不会因为模型在远端运行而自动迁移到远端数据库。

本轮 QGIS 集成的统一远端服务地址更新为 `https://35fcc12d.r27.cpolar.top`，API base 为 `https://35fcc12d.r27.cpolar.top/v1/`。该地址是使用者提供的部署实例，可以在配置中替换。旧地址 `https://1692fe05.r27.cpolar.top` 仅保留在早期验证证据中。

## 本地模型

在 GUI 中选择 `embedded` provider。导入或下载锁定的 `embeddinggemma-2-Q8_0.gguf`，程序校验文件大小与 SHA-256 后使用它。首版只处理文本及 JSON 的文本语义，不需要 mmproj。

模型通过独立 worker 加载。CPU 是默认后端，也可在对应发行包中选择可用 GPU。设备是否可用以运行时诊断为准，显式指定的设备不可用时应处理报告的错误。

服务默认使用 `127.0.0.1:8383`。配置 `port: 0` 时系统选择空闲端口，服务启动输出中包含实际端口。调用方需要使用这个实际地址。

## 远端 embedding provider

独立程序本身也可连接远端 embedding 服务。以下是 profile 目录内 `config.json` 的示意，实际 URL 由使用者填写：

```json
{
  "schema_version": 1,
  "provider": "http",
  "database_path": "C:/GIS/shared/tool-semantic.sqlite3",
  "api_base": "https://models.example.com/v1/",
  "model_id": "embeddinggemma-2-Q8_0",
  "model_revision": "bfcd298762cc34d0357ece5ebdd31791a3a374d8",
  "authorization": "",
  "host": "127.0.0.1",
  "port": 8383,
  "token": "replace-with-a-local-service-token",
  "timeout_ms": 120000
}
```

`authorization` 是向远端 provider 发送的认证头值，`token` 是调用本机 qctsm-service 的 bearer token，两者用途不同。不要将带秘密的 profile、日志或真实项目目录提交到仓库。

API base 包含 `/v1/` 及反向代理前缀，例如 `https://host/model-service/v1/`。网页能够打开不代表 embedding API 已可用，应使用连接测试或一次真实向量请求确认。

## QGIS 与独立程序打开同一个数据库

1. 选定一个普通本地磁盘路径，例如 `C:/GIS/shared/tool-semantic.sqlite3`。
2. 在 QGIS“工具语义检索”配置与独立程序的数据库设置中填写完全相同的路径。
3. 将两边的模型 ID、模型 revision 和维度配置为同一向量空间。URL 可以不同。
4. 导入或更新一侧的目录后，在另一侧重新发起查询。查询读取最新已发布 generation。

两个应用可以同时查询。目录或向量发布的短写入阶段通过跨进程锁互斥，等待后仍无法取得锁时返回 `index_busy`。embedding 计算不占用 SQLite 写事务或目录写锁，两个同步任务的网络计算可以重叠。向量发布前会核对来源版本和内容指纹，迟到结果不能覆盖新目录。已发布的记录在计算期间仍可读取。

每个 source 的同步请求表示完整快照。更新某个 source 不应顺便删除其他 source 的记录。同一 source 的上游需要约定唯一目录所有者，避免不同程序轮流提交不同的“完整”目录。

数据库包含模型 profile 和提示模板版本。同名模型替换了权重后必须更新 revision。不同 profile 的向量不能比较，相同服务 URL 也不能证明向量兼容。切换 provider 后复用向量的前提是 profile 一致，而不是只看文件名。

使用 SQLite WAL 时，运行中的数据库可能同时有 `-wal` 与 `-shm` 文件。备份应在关闭相关写入者后进行，或使用 SQLite 的备份机制。不要仅复制正在写入的主文件并把它视为完整备份。共享索引应放在本地磁盘，不放到不保证 SQLite 锁语义的网络共享中。

## 生命周期

`qctsm-service --profile <directory>` 显式启动一个服务实例，同一 profile 只允许一个服务拥有者。GUI 连接到已运行的服务时，关闭 GUI 不表示停止该服务。

请求返回 job ID 后可以查询或取消。关闭某个 HTTP 查询连接不等于取消后台索引。模型/服务停止后保留 SQLite 数据，重新启动后仍可使用已持久化目录。
