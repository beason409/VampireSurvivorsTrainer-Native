# 后续开发与 GitHub 同步

在从 GitHub 克隆的仓库内修改源码。Git 负责版本和同步，Docker 负责运行容器检查，两者不需要账号绑定。

## 一次迭代

先确认工作区没有待处理改动，再更新主分支并创建功能分支：

```powershell
git switch main
git pull --ff-only
git switch -c fix/short-description
```

完成修改和相关本地检查后，检查差异，只提交本次需要的文件：

```powershell
git status --short
git diff
git add -- path/to/changed-file
git commit -m "Describe the change"
git push -u origin HEAD
```

在 GitHub 创建 Pull Request。CI 也会从干净 checkout 执行导出，检查仓库能否再次生成公开源码快照。Windows 和 Linux 检查通过后合并到 `main`，本地再运行 `git switch main` 和 `git pull --ff-only`。保存文件不会自动推送，合并也不会自动发布正式版。

## 本地检查

Windows 源码与桌面检查使用 PowerShell 7 和仓库指定的 .NET SDK：

```powershell
./scripts/Verify-Source.ps1 -IncludeDesktop -IncludeProbeIntegration
./tests/Release.Tests.ps1
```

Docker Desktop 只需在运行下面的检查时开启。从仓库根目录执行：

```powershell
docker build --progress=plain -t vs-trainer-portable-checks .
docker run --rm --network none vs-trainer-portable-checks
```

这个容器运行 Runtime 和 Probe 的可移植测试；Windows 窗口、原生管道和完整插件仍在 Windows 上检查。不需要 Compose、监听端口或常驻服务，也不用把整个本机工作区挂进容器。源码改动后需重新构建镜像。

## 本机配置与发布

游戏生成的接口、NuGet 缓存、备份、安装记录和环境配置留在本机。完整构建可通过 `-InteropDirectory` 或 `VS_TRAINER_INTEROP` 指定接口目录，原有数据位置使用 `VS_TRAINER_DATA`，缓存可用 `VS_TRAINER_NUGET_PACKAGES` 指定；不要把实际路径或配置值写入提交。

发布前从当前源码生成公开快照：

```powershell
./scripts/Export-PublicSource.ps1
```

脚本接受开发工作区的 `docs/public` 布局，也能直接处理公开仓库的根 README、VALIDATION 和 `docs` 布局。它只导出允许的源码与文档，结果及清单位于忽略的 `artifacts/public`。正式 Windows 包的构建与检查见 [BUILDING.md](BUILDING.md)；发版时再单独确定版本、打包和上传 Release。
