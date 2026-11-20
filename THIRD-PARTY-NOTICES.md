# Third-party components / 第三方组件

Vampire Survivors Trainer 自有代码的版权声明不改变第三方组件的许可。第三方的使用、复制、修改、替换、调试及再分发权利依各自原许可保留；不能将本项目的保留版权条款施加于第三方组件。

完整上游许可证、版权声明和必要通知保存在 [`licenses/`](licenses/)。固定来源、NuGet 元数据及配套文件 SHA-256 见 [`licenses/components.json`](licenses/components.json)。这是来源与许可材料清单，不是代码签名、漏洞扫描或零误报保证。

## 游戏插件配套组件

| 组件 | 配套版本 / 文件范围 | 许可文本 | 对应源码 |
| --- | --- | --- | --- |
| BepInEx | 6.0.0-be.793+dbc82f0；BepInEx.* DLL、XML和配置 | [LGPL 2.1](licenses/BepInEx/6.0.0-be.793/LICENSE) | [固定提交](https://github.com/BepInEx/BepInEx/tree/dbc82f05b8ea34833de5b6a723eb60dd8398e0b1) · [源码下载](https://github.com/BepInEx/BepInEx/archive/dbc82f05b8ea34833de5b6a723eb60dd8398e0b1.zip) |
| Unity Doorstop | 4.6.0；winhttp.dll、版本标记 | [LGPL 2.1](licenses/UnityDoorstop/4.6.0/LICENSE) | [固定提交](https://github.com/NeighTools/UnityDoorstop/tree/1c3ece1f1e6c20f4aa74d5402baabf8a0098d282) |
| Dobby（BepInEx构建） | 1.0.5；dobby.dll | [Apache 2.0](licenses/Dobby/1.0.5/LICENSE) | [固定提交](https://github.com/BepInEx/Dobby/tree/888d971214900374edbca6206fad6ded8a2c1311) |
| Il2CppInterop | 1.5.3；Common、Generator、HarmonySupport、Runtime | [LGPL 3.0 only，含GPL 3全文](licenses/Il2CppInterop.Runtime/1.5.3/LICENSE) | [固定提交](https://github.com/BepInEx/Il2CppInterop/tree/dbda1cb353b0f4253345dc45136d170b9e50a5a0) · [源码下载](https://github.com/BepInEx/Il2CppInterop/archive/dbda1cb353b0f4253345dc45136d170b9e50a5a0.zip) |
| HarmonyX | 2.10.2；0Harmony.dll | [MIT](licenses/HarmonyX/2.10.2/LICENSE) | [上游版本](https://github.com/BepInEx/HarmonyX/tree/v2.10.2) |
| AsmResolver | 6.0.0-beta.5；AsmResolver、DotNet、PE、PE.File | [MIT](licenses/AsmResolver/6.0.0-beta.5/LICENSE.md) | [固定提交](https://github.com/Washi1337/AsmResolver/tree/124e161a9bc0ebb83b4d97276ae3da3c9cfe2fc5) |
| AssetRipper.CIL | 1.2.2 | [MIT](licenses/AssetRipper.CIL/1.2.2/LICENSE) | [固定提交](https://github.com/AssetRipper/AssetRipper.CIL/tree/06216e69825ccd6536dc17e9ca81c4d539ce9321) |
| AssetRipper.Primitives | 3.2.0 | [MIT](licenses/AssetRipper.Primitives/3.2.0/License.md) | [固定提交](https://github.com/AssetRipper/AssetRipper.Primitives/tree/d759b3db98bf2a1cb8c2181faf5d405610aac585) |
| Cpp2IL / LibCpp2IL / StableNameDotNet / WasmDisassembler | 2022.1.0-development.1452系列；StableNameDotNet产品版本0.1.0-development.1452 | [MIT](licenses/Cpp2IL/2022.1.0-development.1452/LICENSE) | [固定提交](https://github.com/SamboyCoding/Cpp2IL/tree/558ddd98642010897d54316b51fbaa7889fda093) |
| Disarm | 2022.1.0-master.99 | [MIT](licenses/Disarm/2022.1.0-master.99/LICENSE) | [固定提交](https://github.com/SamboyCoding/Disarm/tree/8574010015c8c56bb7faad7e407c50b0abe9cc72) |
| AssetRipper.Gee.External.Capstone | 2.3.2；Gee.External.Capstone.dll | [MIT](licenses/AssetRipper.Gee.External.Capstone/2.3.2/LICENSE) | [固定提交](https://github.com/ds5678/Capstone.NET/tree/b90e380c14e857865c94a355954236e678bd557c) |
| Iced | 1.21.0 | [MIT](licenses/Iced/1.21.0/LICENSE.txt) | [固定提交](https://github.com/icedland/iced/tree/601c40570d6c986c8e1a6a2e483dd2fdf5ade552) |
| Mono.Cecil | 0.11.4；Cecil、Mdb、Pdb、Rocks | [MIT](licenses/Mono.Cecil/0.11.4/LICENSE.txt) | [上游版本](https://github.com/jbevain/cecil/tree/0.11.4) |
| MonoMod.RuntimeDetour / MonoMod.Utils | 22.7.31.1 | [MIT](licenses/MonoMod.RuntimeDetour/22.7.31.1/LICENSE) | [上游版本](https://github.com/MonoMod/MonoMod/tree/v22.07.31.01) |
| MonoMod.Backports / MonoMod.ILHelpers | 1.1.2 / 1.1.0 | [MIT](licenses/MonoMod.Backports/1.1.2/LICENSE) | [固定提交](https://github.com/MonoMod/MonoMod/tree/a1b82852b2574742776af08818487b90b0bfab93) |
| SemanticVersioning | 2.0.2 | [MIT](licenses/SemanticVersioning/2.0.2/LICENSE) | [固定提交](https://github.com/adamreeve/semver.net/tree/c52c9d4c5d34a6a3bb6ff45f7276d8b6e951a42d) |

BepInEx原始官方构建：[build 793 ZIP](https://builds.bepinex.dev/projects/bepinex_be/793/BepInEx-Unity.IL2CPP-win-x64-6.0.0-be.793%2Bdbc82f0.zip)，已下载构建SHA-256：`5B63D7D0E92A2448CCD13B738FF95FF1CC1026C6D718555D4DF8E6C824953AC4`。本项目未修改上述上游DLL。游戏原始文件、游戏生成的互操作程序集、用户存档和本机安装记录不随本工具再分发。

## Microsoft .NET

- BepInEx内嵌Mini CoreCLR为 **6.0.7**，来自 [BepInEx/dotnet-runtime固定源码](https://github.com/BepInEx/dotnet-runtime/tree/e10df436edd020835a630a40796ba48222acb1d5)。保留其 [MIT许可](licenses/BepInEx-mini-coreclr/6.0.7/LICENSE.TXT) 与 [第三方通知](licenses/BepInEx-mini-coreclr/6.0.7/THIRD-PARTY-NOTICES.TXT)；原始.NET 6.0.7 runtime pack的 [许可](licenses/Microsoft.NETCore.App.Runtime.win-x64/6.0.7/LICENSE.TXT)、[通知](licenses/Microsoft.NETCore.App.Runtime.win-x64/6.0.7/THIRD-PARTY-NOTICES.TXT)和元数据一并保留。
- Mini CoreCLR目录还包含Microsoft.Bcl.AsyncInterfaces、DependencyInjection、Logging、Options、Primitives等组件，实际产品版本为 **6.0.0**。各包MIT文本与`.nuspec`在`licenses/<PackageId>/6.0.0/`，完整列表见组件清单。
- 自包含桌面分发基线为 **Microsoft.NETCore.App.Runtime.win-x64 10.0.13** 和 **Microsoft.WindowsDesktop.App.Runtime.win-x64 10.0.13**。保留 [Core许可](licenses/Microsoft.NETCore.App.Runtime.win-x64/10.0.13/LICENSE.TXT)、[Core第三方通知](licenses/Microsoft.NETCore.App.Runtime.win-x64/10.0.13/THIRD-PARTY-NOTICES.TXT)、[Desktop许可](licenses/Microsoft.WindowsDesktop.App.Runtime.win-x64/10.0.13/LICENSE)、[WPF通知](licenses/Microsoft.WindowsDesktop.App.Runtime.win-x64/10.0.13/wpf-THIRD-PARTY-NOTICES.TXT)与 [WinForms通知](licenses/Microsoft.WindowsDesktop.App.Runtime.win-x64/10.0.13/winforms-THIRD-PARTY-NOTICES.TXT)。对应包元数据固定到 [dotnet/dotnet提交](https://github.com/dotnet/dotnet/tree/4099f83977f4d29604a9aeb971fb0a7bc1d5f699)。

旧加载器依赖是当前游戏的兼容性基线，不能视为最新运行时或无漏洞保证。.NET 6已结束支持；升级嵌入运行时与加载器需要重新验证游戏兼容性。[Microsoft .NET生命周期](https://dotnet.microsoft.com/en-us/platform/support/policy/dotnet-core)

## 库的替换和调试

配套库以独立文件保留，未压缩混淆或合并进本工具插件。上游源码获取链接和原许可权利继续有效，包括适用LGPL条款下为调试修改后的库进行的逆向工程。备份、游戏版本检查及安装记录用于避免破坏游戏和存档，不是对上游许可权利的额外限制。自行替换库后可能不再符合本工具已验证的依赖组合，需要按上游接口重新验证运行。

## 原生护甲试点

原生试点使用固定源码版本的 MinHook 1.3.4（BSD-2-Clause）及 yyjson 0.13.0（MIT），来源提交和逐文件校验值见 [native/vendor/manifest.json](native/vendor/manifest.json)。静态链接的 Zig 编译器运行库、LLVM libc++/libc++abi/libunwind 和 MinGW 相关许可原文保存在 [native/vendor/licenses](native/vendor/licenses)，版本与校验值见 [runtime-licenses.json](native/vendor/runtime-licenses.json)。这些许可与本项目自有代码的保留版权分别适用。

原生 EXE 内嵌项目、依赖及运行库许可，界面的“说明与许可”或 `--licenses` 可直接读取。便携编译器仅用于构建，玩家分发不携带编译器。原生后端不使用上表的 BepInEx/Il2CppInterop 运行时，方法布局依据通过固定上游源码核对。

## 分析工具

Cheat Engine、ILSpy曾用于开发分析，未包含在面向玩家的分发包中。它们拥有各自许可；仓库辅助脚本不是对这些工具许可的替代。Docker只用于开发或离线验证，不是运行本修改器的依赖。
