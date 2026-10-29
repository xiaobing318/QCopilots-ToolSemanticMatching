# 测试与检索质量

测试分为契约与逻辑测试、真实模型测试、检索质量评测和最终包验证。每一类分别报告结果，不把一个层次的成功替代另一个层次。

## 冻结的 GIS 检索夹具

`tests/quality/catalog.json` 含 58 个能力及一个 gdalwarp 调用示例，共 59 条记录。标识符已对照 QGIS 提交 `8f513743d20f78e88688e2174c59f6dc6a922b2c` 的源码核对，每条定义保留源文件路径。说明是面向检索编写的英文摘要，不是完整实时执行 schema。

`queries.json` 含 74 条中文、英文及混合语言需求。期望能力、查询与文件 SHA-256 在运行前冻结，见 `fixture-manifest.json`。主要混淆场景包括：

- buffer、clip、intersection、union、difference 与 dissolve。
- 真正重投影与只声明 CRS。
- 检查几何与修复几何。
- 质心与保证位于面内的代表点。
- 矢量线构面与栅格区域转面。
- 矩形范围裁剪与多边形掩膜裁剪。
- 坡度、坡向、等高线和地形阴影。
- 操作工具定义与带 argv 的 gdalwarp 调用示例。

该小目录用于可重复回归。实际 QGIS 验收还要使用运行时完整目录，不应把小目录分数表述为全部已安装 provider 的表现。质量跑器的 `--catalog` 参数接受 QGIS 导出的完整目录，预期 ID 可从候选的 `algorithm_id`、`binary_id` 或工具 `name` 解析，不依赖服务命名空间。

## 运行质量评测

先启动一个使用临时 profile 和临时数据库的 qctsm-service，配置真实嵌入模型。测试报告必须保存在源码外。

```powershell
$env:QCTSM_API_TOKEN = '<临时测试服务的 token>'
python -B tests/quality/run_quality.py `
  --endpoint http://127.0.0.1:8383 `
  --cli C:/Build/qctsm/bin/qctsm-cli.exe `
  --report C:/Build/qctsm/reports/gis-quality.json
```

跑器通过正式 HTTP API 导入一个唯一 benchmark source，等待索引后逐条调用产品 CLI 查询，最后用空快照清理这个 source。首条查询额外通过 HTTP 验证候选顺序和原始 JSON 一致。临时 CLI profile 在源码外创建并清理，token 不放在进程命令行中。每个查询都带来源过滤，不修改已有业务来源，也不执行检索出来的算法。`--keep-catalog` 仅用于保留失败现场。

省略 `--cli` 可以单独诊断 HTTP 路径。CTest 的 `qctsm-quality-real-model` 使用 `run_service_quality.py` 启动独立服务、临时数据库和真实内嵌模型，并传入编译完成的 CLI。测试结束通过明确的关闭接口结束自建服务，超时后的强制终止会作为验收失败记录。`QT_QPA_PLATFORM` 等运行环境由上层构建任务继承。

验收规则：

- 每条查询的 Recall@8 是前八个候选找到的期望能力数量除以期望能力总数，最终对全部冻结查询取平均。
- MRR@8 记录第一项相关能力的倒数排名。
- 所有查询都必须完成真实 embedding 检索，`lexical` 降级或请求错误导致验收失败。
- 平均 Recall@8 至少 0.90。未运行或错误查询按零分保留，不能从分母移除。
- 报告包含每条候选、遗漏项、失败样本、索引耗时、首个查询、重复查询、延迟中位数和 p95。
- 上下文体积按紧凑 JSON 的 UTF-8 字节数报告，包含全目录 payload、每次候选 payload 和候选响应。字节数不冒充模型 token 数，最终 QGIS 端到端验收还需记录实际请求与模型 tokenizer 统计。

首条查询与重复查询使用同一个客户端路径。CLI 延迟包含进程启动及任务轮询，HTTP 一致性检查耗时单列。索引阶段可能已加载模型，因此这里的首条查询不能表述为模型冷启动耗时。

不通过删除失败样本、放宽 Top N 或降低门槛制造通过结果。修订语料或标签需要说明独立的事实错误、提高 dataset revision 并保留可比较的旧报告。

## 快速检查与真实模型的区别

```powershell
python -B -m unittest discover -s tests/quality -p test_quality_contract.py -v
```

此命令只验证夹具哈希、标签有效性、用户示例的参数顺序和评分代码。它不加载模型，也不产生检索准确率结论。`build_fixture.py` 是显式维护工具，不能在 CI 测试开始时悄悄重写冻结夹具。

真实模型测试另外验证模型文件完整性、768 维、有限非零向量、L2 范数、固定参考向量、取消和文本超限。最终包验证在无源码和开发 SDK 的布局中加载真实模型。

## 集成与共享数据库

需要覆盖同一本地 SQLite 被 QGIS 和独立程序打开的情况，包括一侧更新后另一侧查询、并发读、写入者冲突、模型 profile 不一致、索引中断和重启后的读取。切换远端与本机 embedding provider 时，只有相同 profile 才复用向量。

QGIS 工作流测试独立验证依赖、实际输出、取消和副作用状态。检索成功不代表工具参数正确，工具调用成功也不代表业务输出已经验证完成。
