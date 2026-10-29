# QCopilots Tool Semantic Matching

把自然语言需求与 JSON 描述的算法、工具及调用示例匹配起来。查询可以是中文、英文或混合语言，结果包含候选工具、排序分数及完整原始 JSON。

项目使用 C++20 和 Qt Widgets，提供桌面界面、命令行、HTTP 服务和可嵌入的检索核心。内嵌 llama.cpp 可在本地生成文本向量，也可以连接兼容 `/v1/embeddings` 的远端服务。应用不执行检索命中的工具，具体参数绑定与执行由 QCopilots 等调用方负责。

## 输入与结果

两类 JSON 都可检索：

- 工具定义，例如名称、用途、参数说明、schema 和输出类型。
- 调用示例，例如 `binary_id` 与有序 `arguments`。示例可以关联定义，也可以单独导入。

查看 [JSON 目录说明](docs/json-catalog.md)、[gdalwarp 定义与示例](examples/gdalwarp-catalog.json) 和 [原始调用示例](examples/gdalwarp-call.json)。原始 payload 完整保留，`confirmed_risk` 与 `client_request_id` 不提供执行授权，也不参与语义排序。

```json
{
  "query": "将 DEM 使用 Lanczos 重采样并输出压缩的 GeoTIFF",
  "top_n": 8,
  "filters": {"available": true}
}
```

## 本地与远端共用索引

向量数据库始终保存在本地。QGIS 与本程序使用同一检索核心和数据库格式，可以配置为打开同一个 SQLite 文件。目录、原始 JSON 和向量会在两个应用之间可见，更新使用跨进程单写入者协调，查询读取已发布的数据。

嵌入服务地址不决定向量空间。模型版本、维度及提示模板一致时，本地和远端 provider 可复用向量。配置不同的模型版本时，旧向量不会被当成新模型向量。详见 [部署与共享数据库](docs/deployment.md)。

## 快速使用

安装与当前平台相符的构建包后启动 `qctsm-gui`。配置模型或远端 embedding API，选择本地数据库路径，启动服务，再导入 JSON 目录并查询。文本检索只需要主 GGUF，不需要多模态 projector。

命令行也可以连接同一个服务。以下命令通过同一 profile 读取地址和 token：

```powershell
qctsm-service --profile C:/GIS/qctsm-profile
```

在另一个终端执行：

```powershell
qctsm-cli --profile C:/GIS/qctsm-profile health
qctsm-cli --profile C:/GIS/qctsm-profile import --file ./examples/gdalwarp-catalog.json --wait
qctsm-cli --profile C:/GIS/qctsm-profile search --query "重采样高程影像并压缩输出" --top-n 8 --wait
```

`--wait` 超时只停止等待，不自动取消后台任务。使用 `cancel --job-id <id>` 明确请求取消。CLI 及 HTTP 返回真实状态和错误，embedding 故障时的关键词降级会标记为 `lexical`。

## 开发与验证

- [开发和构建](docs/development.md)：外置构建目录、CPU/GPU 变体与安装包。
- [HTTP API](docs/api.md)：目录、搜索、原始记录、模型和任务接口。
- [系统结构](docs/architecture.md)：共享核心、推理 worker、数据库与候选排序。
- [测试与检索质量](docs/testing.md)：冻结的 74 条查询、Recall@8、MRR 和真实模型验收。

GitHub Actions 配置覆盖 11 个平台与后端变体。是否通过以具体提交对应的 Actions 结果为准。GPU 变体构建成功和 CPU 回退测试不代表该 GPU 硬件已完成推理验收。

## 设计来源与许可

项目的模型锁、构建隔离和部署流程参考 [QCopilots-Search](https://github.com/xiaobing318/QCopilots-Search)。工具按需暴露思路参考 [AtQuery](https://github.com/adelacky-de/AtQuery)，分阶段算法选择思路参考 [GeoAgent](https://github.com/iamtekson/GeoAgent)。这些参考不构成对它们检索准确率或当前程序测试结果的声明。

项目代码采用 [MIT License](LICENSE)。第三方依赖及复用材料见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
