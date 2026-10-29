# 开发和构建

应用使用 C++20、Qt Widgets、CMake 和 Ninja。Python 3.10+ 仅用于开发、测试和打包，最终安装后的应用不依赖 Python。

独立程序使用锁定的 Qt 6.12.0。共享核心支持 QGIS 使用的 Qt 6.4+ API，不引入媒体、PDF、OCR 或 QGIS 依赖。模型和 llama.cpp 的版本与 SHA-256 分别记录在配置锁文件中。

## 外置构建

所有生成内容放在源码树以外，包括构建树、SDK、模型、安装暂存、测试 profile 和报告。每个后端与构建配置使用独立目录，不能跨后端复用二进制缓存。

```powershell
$qctsmBuildRoot = 'C:/Build/qctsm'
$qctsmQtRoot = 'C:/SDK/Qt'
$qctsmVariant = 'windows-x64-cpu'

./qctsm-dev.ps1 bootstrap --variant $qctsmVariant --build-root $qctsmBuildRoot --qt-root $qctsmQtRoot
./qctsm-dev.ps1 doctor --variant $qctsmVariant --build-root $qctsmBuildRoot --qt-root $qctsmQtRoot
./qctsm-dev.ps1 configure --variant $qctsmVariant --build-root $qctsmBuildRoot --qt-root $qctsmQtRoot
./qctsm-dev.ps1 build --variant $qctsmVariant --build-root $qctsmBuildRoot --qt-root $qctsmQtRoot
./qctsm-dev.ps1 models --variant $qctsmVariant --build-root $qctsmBuildRoot --qt-root $qctsmQtRoot
./qctsm-dev.ps1 test --variant $qctsmVariant --build-root $qctsmBuildRoot --qt-root $qctsmQtRoot
```

已有满足锁定版本的 SDK 时可以跳过 bootstrap。Windows 入口初始化 VS2022 x64 开发环境。`QCOPILOTS_TOOL_SEMANTIC_MATCHING_PYTHON` 可以显式指定 Python，避免 Microsoft Store 占位解释器。Linux/macOS 使用 `bash ./qctsm-dev.sh` 和相同参数。

只构建 SDK 时给 configure 添加 `--core-only`。这不需要 llama.cpp 模型或 GPU SDK。独立完整产品的测试仍需真实模型。

## 安装与打包

依次运行 `install`、`package`、`verify-package`，Windows/Linux 再运行 `installers`。参数与 configure 使用的变体、构建根和 Qt 根一致。`check` 执行完整开发检查链，日常增量开发可直接重复 build 和对应测试。

| 平台 | 构建变体 | 用户包 |
| --- | --- | --- |
| Windows x64 | CPU、Vulkan、ROCm/HIP、CUDA 12.4、CUDA 13.4 | NSIS、ZIP |
| Linux x64 | CPU、Vulkan、ROCm/HIP、CUDA 12.8、CUDA 13.4 | DEB、TGZ |
| macOS arm64 | Metal | DMG |

具体 SDK 版本和 runner 以 `config/build-variants.json` 为准。托管 CI 的 GPU 变体主要证明编译和部署路径。真实 GPU 推理需要报告实际 backend 与 device，不能把 CPU 回退写成 GPU 已验收。

## QGIS 核心快照

使用 `tools/qctsm-export-core.py --help` 查看源码导出接口。源码包只含共享 SDK、必要 CMake、许可和校验清单，不含 GUI、推理引擎、模型或开发机路径。

QGIS 集成使用固定提交导出的快照，由 QGIS 自己的 Qt 和编译器构建。不要直接加载独立应用编译出的 Qt C++ DLL，也不要在 QGIS 集成目录中独立修改检索算法。

## CI 结果

CI 在拉取请求、main 更新和手动触发时运行。所有要求的变体、真实模型测试与包验证需要成功，汇总检查绑定源码提交、run 与 attempt。日志应区分构建失败、功能失败、模型缺失和硬件不可用。提交了 workflow 或启动了 run 都不代表 CI 已通过。
