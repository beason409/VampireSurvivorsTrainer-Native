# Probe 离线测试

此控制台测试直接编译生产 `CommandFiles.cs`，并启动实际 Release 版 `Trainer.Probe.dll`，连接测试进程自己创建的假命名管道。不会查找、连接、启动或操作真实游戏。

```powershell
$env:DOTNET_CLI_HOME = '.\.dotnet'
$env:NUGET_PACKAGES = '.\dependencies\nuget'
dotnet build '.\tools\Trainer.Probe\Trainer.Probe.csproj' -c Release --no-restore -m:1 -p:BuildInParallel=false
dotnet restore '.\tests\Trainer.Probe.Tests\Trainer.Probe.Tests.csproj' --configfile '.\tests\Trainer.Probe.Tests\NuGet.Config' -p:BuildInParallel=false --disable-parallel
dotnet run --project '.\tests\Trainer.Probe.Tests\Trainer.Probe.Tests.csproj' -c Release --no-restore -p:BuildInParallel=false
```

覆盖空白 CRLF、分段追加、未提交的末尾行、已提交坏 JSON 后继续执行、文件清空与替换、有限文件末行无换行、两种 CLI 参数写法、只读默认请求、显式命令顺序及清理、`--abrupt`、会话错误与响应编号校验。

验证结果：**17/17 通过**。覆盖玩法计划解析、游戏未暂停计时和暂停/升级等待；六个跨进程集成用例须在允许本机命名管道互通的执行环境中运行，其余直接解析用例不依赖跨进程通信。测试输入保留在本测试目录 `artifacts` 下，便于核对。
