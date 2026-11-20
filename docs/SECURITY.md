# 下载核验与本机保护

## 核对正式版 2.0.0

从官方 Release 下载 `VampireSurvivorsTrainer.Native.exe` 和 `SHA256SUMS.txt`。在 PowerShell 查询文件摘要，结果应与校验文件中的 SHA-256 一致。

```powershell
Get-FileHash -Algorithm SHA256 .\VampireSurvivorsTrainer.Native.exe
```

也可以用 GitHub CLI 核对完整 EXE 的来源、发布标签和签发工作流。

```powershell
gh attestation verify .\VampireSurvivorsTrainer.Native.exe --repo beason409/VampireSurvivorsTrainer-Native --signer-workflow beason409/VampireSurvivorsTrainer-Native/.github/workflows/native-release.yml --source-ref refs/tags/v2.0.0 --deny-self-hosted-runners
```

GitHub 构建证明用于验证下载来源。Windows 发布者签名使用独立的证书机制，本项目未使用该证书。

## 本机保护

- 连接前核验游戏 EXE、GameAssembly、元数据及 Steam Build，加载项限定为 EXE 内嵌的原生后端。程序校验压缩数据和解压后的后端，直接从锁定的同一文件读取并核验缓存。缓存异常时停止加载。
- 命名管道限定当前 Windows 用户、会话和游戏进程。持久化操作需要通过相应的备份与权限检查。断连或心跳过期会撤销持续效果。
- 公开发布仅上传核验过的 EXE 和校验文件。源码在发布前按白名单导出并检查，个人路径、游戏接口及用户数据保存在本机。

公开报告问题时，请提供版本、错误文字和复现步骤，先去掉账户标识、路径和存档信息。
