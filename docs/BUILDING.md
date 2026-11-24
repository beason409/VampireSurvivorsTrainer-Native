# 构建与公开检查

轻量化后端的迁移顺序、体积口径和验收标准见 [轻量化架构](COMPACT-ARCHITECTURE.md)。

作者署名：Beason9527。本项目源代码公开可查看，具体使用与再分发权利以仓库许可证为准。第三方组件保留各自许可证。

## 普通使用者

取得作者提供的 Windows x64 ZIP，完整解压后运行其中的 `VampireSurvivorsTrainer.exe`。默认完整版自带桌面运行时，解压即可运行；可选轻量版需要 .NET 10 Windows Desktop Runtime x64。两种包都包含同一套加载器和插件，不需要安装 .NET SDK、Visual Studio、Cheat Engine、Inkscape 或 Docker。

打包默认生成文件名带 `full` 的完整版；`./scripts/Package.ps1 -FrameworkDependent` 生成带 `lite` 的轻量版。轻量版仅复用系统桌面运行时，配套文件与功能相同。清单中的 `deploymentMode` 和 `runtimeRequirement` 记录部署方式。ZIP 使用标准 SmallestSize 压缩，并保留框架简体中文资源和默认英文；应用自己的中英文界面资源完整保留。

游戏必须由使用者自行合法安装。发布包不提供游戏本体或游戏生成的互操作程序集。第一次带插件启动时，加载器在本机生成这些接口，耗时会比后续启动长；游戏更新后仍须通过受支持版本检查。

## 应用图标

原创图标源文件为 `src/Trainer.Desktop/Assets/App.svg`，使用形状和路径绘制，可直接用 Inkscape 编辑。重新生成多尺寸 Windows ICO 时使用 `scripts/Generate-AppIcon.ps1`；脚本查找已安装的 Inkscape，也支持显式指定其路径。生成后的 ICO 已包含在源码中，正常编译和使用程序不需要运行图标生成器。

## 公开 Windows 源码检查

需要 Windows x64、PowerShell 7 和 `global.json` 指定的 .NET SDK。此检查不需要安装游戏或运行 Steam：

```powershell
./scripts/Verify-Source.ps1 -IncludeDesktop -IncludeProbeIntegration
```

脚本顺序构建桌面程序与测试项目，执行 Core 假目录/假进程测试、直接链接生产 `TrainerRuntime` 的策略测试、Probe 解析/计时及自建假命名管道集成测试。`-IncludeDesktop` 还运行直接链接奖励面板源码的搜索、选择与请求测试，最后以 `--smoke-test` 创建并关闭隔离窗口。它不会连接真实游戏、安装插件、修改设置或读取真实存档。日志和摘要写入 `artifacts/source-check/`。

桌面检查还覆盖中英文切换、设置持久化、动态提示和切换期间输入/选中/图标缓存保留。`src/Trainer.Desktop/Localization/*.en.json` 是嵌入主程序集的英文目录，构建时使用 `WithCulture="false"`，无需外置翻译文件。隔离预览支持 `--render-preview-found <path.png> --language en`；奖励、属性和说明页分别使用 `--render-preview-chest`、`--render-preview-stats`、`--render-preview-about`。

`-IncludeDesktop` 还检查内置文件目录及分块限长响应读取，并运行
实际 `PipeServer`/`PipePeerGuard`/`BackupVerifier` 的本地原生管道测试。
这些测试不连接实际游戏；受限令牌沙箱可能阻止子测试进程通过当前用户
ACL，需在正常 Windows 用户会话中运行，不能把跳过当作通过。

配套文件指纹由 `scripts/Generate-TrustedPayloadCatalog.ps1` 在完整
payload 准备后生成，并以 `TrustedPayloadCatalogPath` 构建属性嵌入 Core。
构建与打包脚本先编译插件、准备固定目录，再编译带该目录指纹的 Core
和窗口，避免自循环哈希。普通源码检查允许没有资源时编译与烟雾显示，
但安装必定拒绝；请运行脚本输出的完整 app 目录，不要搭配旧 payload。

高频状态更新使用 `UiLocalization.SetText(control, source)`：保存原文，翻译后仅在最终显示内容变化时更新控件。不要在轮询中先赋中文再异步翻译；这会导致英文闪烁。桌面防闪测试检查相同状态重复刷新时的 `TextChanged` 与下拉框 `WM_PAINT` 计数，并验证变化读数与双语切换仍然正确。

已还原依赖的本机工作区可加 `-SkipRestore`；未使用 `-IncludeProbeIntegration` 时，只运行 Probe 的解析及计时测试。此脚本包含 Windows 文件路径场景，非 Windows 主机上会明确拒绝；跨平台测试使用下述 Docker 命令，不将跳过写成通过。

`.github/workflows/source-checks.yml` 配置为在 GitHub 托管 Windows 主机执行相同检查，并在 Linux 主机运行下述 Docker 验证。实际结果以对应提交的公开工作流记录为准。工作流仅有源码读取权限；没有自托管执行器、发布权限或 Release 上传步骤，PR 也不会取得维护者本机游戏环境。

## Docker 的范围

Docker 只供开发者隔离执行可移植的策略与解析测试。它不承载 Windows 游戏窗口，不负责桌面启动或游戏内效果验证，使用者无需运行容器。

```powershell
docker build --progress=plain -t vs-trainer-portable-checks .
docker run --rm --network none vs-trainer-portable-checks
```

Dockerfile 使用固定摘要的微软官方 SDK 镜像。`.dockerignore` 采用默认排除、逐项允许的上下文：只传入共享协议、实际运行时策略、Probe 解析代码和对应测试，不包含存档、安装记录、依赖缓存、账号设置、游戏二进制、游戏接口或完整插件。构建阶段需联网取得基础镜像及 .NET 引用包；测试运行阶段可以禁用网络。

容器只以 `net10.0` 构建 Runtime 策略与 Probe `--offline-only` 测试，避免为不在本容器验证范围的插件 `net6.0` 还原引用包。它不运行 Core 的 Windows 文件路径/Steam 场景、跨进程 Windows 管道测试、WinForms 或实际战斗。Linux 容器成功不代表这些 Windows 与游戏内项目通过。

## 原生版

在 Windows x64 上从正式提交运行 `./scripts/Verify-Native.ps1 -SizeOptimization`。验证包含单 EXE 离线启动、后端缓存核验和损坏数据回归检查。`./scripts/Stage-NativeRelease.ps1 -Commit <提交 SHA>` 核对 Git 历史、源码白名单、构建内容和隐私范围，生成 EXE 与 `SHA256SUMS.txt`。

官方 `v2.1.1` 标签触发原生发布工作流。GitHub 托管运行器从该提交构建并为 EXE 出具证明，核对上传内容后发布。普通分支和 PR 继续执行源码检查。

## 1.0.0 完整游戏插件与分发包

完整插件使用 .NET SDK 编译，目标为 BepInEx 固定构建要求的 `net6.0`；桌面窗口目标为 `net10.0-windows`。维护者需要自行合法安装受支持游戏，并由固定 BepInEx 首次启动生成本机 `BepInEx/interop`，再执行：

```powershell
./scripts/Build.ps1 -InteropDirectory '本机游戏目录/BepInEx/interop'
./scripts/Package.ps1 -InteropDirectory '本机游戏目录/BepInEx/interop'
```

加载器与依赖准备方式见仓库其余构建说明。缺少上述游戏接口时，正式构建/打包脚本会停止。直接单独编译未启用 `GAME_INTEROP` 的插件可能生成引导用途的空功能程序集，不得当作完整可用 Release 发布。

1.0.0 完整分发由维护者本机构建。公开 CI 使用隔离数据进行检查，真实游戏与生成的接口留在本机。

官方参考：[GitHub .NET 构建](https://docs.github.com/en/actions/tutorials/build-and-test-code/net)、[GitHub Actions 完整提交固定](https://docs.github.com/en/actions/security-for-github-actions/security-guides/security-hardening-for-github-actions#using-third-party-actions)、[微软 .NET 容器](https://learn.microsoft.com/en-us/dotnet/core/docker/build-container)。
