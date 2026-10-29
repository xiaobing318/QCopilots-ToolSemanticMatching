# JSON 工具目录

QCopilots Tool Semantic Matching 接收自然语言需求，返回相关算法或工具的 JSON。需求可以是中文、英文或嵌入模型支持的其他语言。目录既可以保存工具定义，也可以保存具体调用示例。程序负责检索，不执行这些 JSON 中描述的操作。

## 一个来源对应一个完整快照

```json
{
  "schema_version": 1,
  "source_id": "my-gis-tools",
  "revision": "2026-10-29.1",
  "records": [
    {
      "record_id": "gdalwarp-definition",
      "kind": "definition",
      "capability_id": "bin.gdalwarp",
      "name": "GDAL Warp",
      "description": "Raster reprojection, resampling and clipping. 栅格重投影、重采样与裁剪。",
      "tags": ["raster", "projection", "resampling"],
      "available": true,
      "payload": {
        "binary_id": "bin.gdalwarp",
        "inputSchema": {
          "type": "object",
          "properties": {
            "arguments": {"type": "array", "items": {"type": "string"}}
          }
        }
      }
    },
    {
      "record_id": "gdalwarp-resampling-example",
      "kind": "example",
      "capability_id": "bin.gdalwarp",
      "description": "Resample a raster with Lanczos and write a tiled compressed GeoTIFF.",
      "payload": {
        "binary_id": "bin.gdalwarp",
        "confirmed_risk": true,
        "client_request_id": "example-request-01",
        "arguments": [
          "-of", "GTiff", "-r", "lanczos",
          "-ts", "30000", "30000",
          "-co", "TILED=YES", "-co", "COMPRESS=DEFLATE",
          "C:/example/input.tif", "C:/example/output.tif"
        ]
      }
    }
  ]
}
```

`source_id` 标识一组由同一调用方维护的记录。同步会替换该来源的完整快照，未包含在新快照中的旧记录会被移除，其他来源不受影响。空 `records` 用于清空该来源。内容有变化时应更新 `revision`。相同 revision 携带不同内容会返回 `source_revision_conflict`。

多个客户端维护同一来源时，可以在快照顶层设置以下控制字段：

| 字段 | 作用 |
|---|---|
| `expected_revision` 为字符串 | 要求数据库中的当前来源 revision 与该字符串完全一致，来源不存在也视为冲突 |
| `expected_revision` 为 `null` | 要求该来源尚不存在，适用于首次创建 |
| 省略 `expected_revision` | 不检查调用方读取的旧版本，显式替换按成功提交的顺序生效 |
| `force_rebuild` 为 `true` | 为当前模型 profile 重新计算所有可用记录的向量，默认 `false` |

例如，已读取来源 revision `2026-10-29.1` 后，提交新内容时设置 `revision: "2026-10-29.2"` 和 `expected_revision: "2026-10-29.1"`。前置条件不满足时返回 `source_revision_conflict`，不会应用该快照。应重新读取或导出当前目录，再决定如何更新，不自动重放过期快照。首次创建的 `expected_revision: null` 在创建成功后不能原样用于更新。

这两个控制字段不计入目录内容指纹，因此可以对同一 revision 的相同内容请求重建。`force_rebuild` 不跳过 revision 前置条件，也不会先删除已有的兼容向量。重算期间，文档未变化且模型 profile 一致的旧向量仍可用于查询。文档变化后的旧向量不再参与该记录的语义检索，重算失败或取消时应检查索引状态。迟到的计算结果只有在来源 revision、快照内容、文档 hash 和模型 profile 仍匹配时才发布，较新的目录已取代该任务时返回 `source_changed`。

每个快照最多 50,000 条记录，每条记录序列化后的大小最多 1 MiB。规模更大的目录应明确拆分来源。完整约束见 [catalog.schema.json](../schemas/catalog.schema.json)。

## 定义、示例与原始 JSON

| 字段 | 作用 |
|---|---|
| `record_id` | 来源内唯一的记录标识，建议由调用方稳定维护 |
| `kind` | `definition` 为能力定义，`example` 为具体调用样本。封装记录省略时默认为 `definition` |
| `capability_id` | 把同一算法或工具的定义和示例放入同一候选组 |
| `payload` | 原始 JSON 对象，内部结构不限于 MCP 或 QGIS |
| `name`、`description`、`tags` | 用于说明能力与使用场景，可补充原始 JSON 中缺少的语义 |
| `available` | 是否可用，默认 `true`。默认搜索仅包含可用记录 |
| `metadata` | 调用方自定义的路由、来源和版本信息，检索器不把它当作执行指令 |

可以只导入调用示例，无需预先创建定义。在 `records` 中放入不含 `payload` 字段的原始对象时，核心会将整个对象封装为 `example`。如果原始对象本身有名为 `payload` 的字段，使用显式封装，避免与目录封装混淆。

未给出 `record_id` 时使用规范化记录内容的哈希。未给出 `capability_id` 时，依次尝试读取原始 JSON 中的 `binary_id`、`algorithm_id`、`name`，最后使用记录 ID。此规则方便首次导入，但长期维护仍建议显式提供稳定标识。

定义与示例在同一来源中使用相同 `capability_id` 即可关联。示例的检索文档会补充同一快照中定义的名称和说明。建议每个来源中每项能力只维护一条权威定义，其他用法放到示例中。不同来源可以使用相同能力 ID，搜索结果仍以 `(source_id, capability_id)` 区分。

## 原始内容与检索文档分开

数据库完整保留 `payload` 的 JSON 值，包括数组顺序、具体参数和示例路径。这里的保留指 JSON 数据内容，不承诺原文件的空白、对象字段顺序或数字字面量排版。

用于生成向量的文档是原始内容的语义投影：

- 包含名称、用途、标签、能力 ID、参数名与参数值。
- 示例关联定义时，补充定义的名称和描述。
- 忽略 `client_request_id`、`job_id`、`confirmed_risk`、确认令牌以及明确的认证字段，避免这些运行状态主导排序。
- 将独立字符串形式的绝对本地路径替换为保留扩展名的路径标记。该处理仅作用于检索文档，不改写原始 JSON。
- 对检索文档分片生成向量，完整 schema 与 payload 仍可通过记录查询接口读取。

调用示例里的 `confirmed_risk: true` 只是一条历史样本值。匹配成功不代表用户已经批准本次运行，也不保证示例路径、算法依赖和参数适合当前工程。QCopilots 执行时仍需基于当前任务重新绑定参数并遵守原工具的确认要求。

## 候选与评分

自然语言检索组合语义相似度、词法匹配与精确标识符匹配。返回结果按能力分组，同一工具的多个示例不会各自占一个 Top N 名额。若此次过滤后的记录包含能力定义，候选的 `payload` 优先呈现定义，`matched_records` 保存相关定义或示例及原始 payload。

`score` 是 RRF 融合排序值，`semantic_score` 是完整用途与参数视图的向量余弦得分，`name_semantic_score` 是能力名称视图的向量余弦得分，`lexical_score` 是词法得分。名称视图只作补充，完整用途与参数视图、名称视图、词法通道的 RRF 权重分别为 1、0.5、1。它们不是“正确概率”，不应用作自动执行许可。无可用向量或嵌入服务失败时，结果可能为 `retrieval_mode: "lexical"`，并包含降级原因。

## 本地数据库共享

QGIS 与独立工具可以指向同一本地 SQLite 文件。数据库保存来源、记录、向量、模型配置指纹和 generation，不绑定远端服务地址。两者使用同一模型版本、维度、提示模板和文档生成规则时可以复用向量。

WAL 允许索引更新期间继续读取。目录和向量发布的短写入阶段通过跨进程锁协调，等待后仍无法取得锁时返回 `index_busy`。embedding 计算期间不持有写锁，多个同步任务的计算可以重叠，迟到结果仍需通过来源版本和内容指纹检查。遇到写入竞争时应等待后重试，不能删除锁文件绕过协调。模型配置不同的向量使用独立指纹，不能仅因模型名称相同就视为可兼容。移动数据库前应关闭使用它的程序，避免漏掉仍在 WAL 中的数据。

工具定义中的 schema 与用法应来自真实工具版本。QGIS Processing、Binary 和 MCP 的运行元数据是接入 QGIS 时的权威来源。目录本身不是执行器，也不代替原工具的实时校验。
