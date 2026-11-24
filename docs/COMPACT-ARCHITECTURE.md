# 轻量化架构与迁移边界

目标是减少完整分发、首次准备和运行时的成本，同时保留现有功能与恢复能力。完整版本仍是 .NET 10 WinForms 桌面端、BepInEx IL2CPP 加载链和 .NET 6 游戏插件。独立的 [原生试点](../native/README.md)已具备单 EXE、内嵌原生后端、版本核验和IPC。用户确认 native-pilot.2 的护甲、生命不减、经验倍率和伤害倍率生效、运行稳定，并确认 native-pilot.4 移动倍率正常。native-pilot.5支持开启中直接改值，并补齐14项普通属性及Omni。

**2.0.0 原生版**包含 9 项常用功能、23 项本局属性、金币设值和两种指定奖励用途。单 EXE 内嵌经过 LZMS 压缩的固定后端。连接前核验游戏文件、Steam Build 和后端指纹。最小化时暂停状态区刷新和指定奖励补图，游戏通信按原有期限继续运行。

实机验证了主菜单图标加载、搜索和滚动。此前用户领取指定升级的护盔后，普通护甲由4升至5。自然宝箱领取、免消费自然扣款和去牙生成效果仍待专测。实际验证范围见 [VALIDATION.md](../VALIDATION.md)。

## 现有体积

以下是 1.0.1-preview.1 Windows x64 优化测试包的审计值。MB 按 1,000,000 字节计算；展开大小包含 ZIP 中全部文件，包括发行清单。

| 包 | ZIP 字节数 | ZIP 大小 | 展开字节数 |
| --- | ---: | ---: | ---: |
| full | 82,255,592 | 82.26 MB | 192,121,739 |
| lite | 34,952,404 | 34.95 MB | 77,608,965 |

lite 复用系统已安装的 .NET 10 Windows Desktop Runtime，游戏插件的 .NET 6 运行时仍随包提供。其 `payload/dotnet` 有 187 个文件，展开占 69,049,095 字节；这些文件在 ZIP 内的压缩数据占 31,625,020 字节，约为整个 lite ZIP 的 **90.48%**。BepInEx `core` 压缩数据另占约 2.57 MB。桌面程序与根目录文件在 lite 中的压缩数据合计约 0.31 MB。这里的分组压缩大小不含 ZIP 目录头。

所以，换一个更小的窗口只能缩减很小的一部分。把相同文件嵌入 EXE 后自解压，依然需要携带和安装原有字节；启动时下载运行时或加载器，则把成本移到了首次联网。现有 full 包免装桌面运行时，但加载器首次生成接口时仍可能取得基础库，不能据此宣称完全离线。

若目标是从已安装的受支持游戏出发，首次使用也完全离线、只分发一个小型 EXE，就需要同时替换桌面 UI、安装/备份/恢复等托管服务，以及游戏内的 BepInEx/.NET 6 后端。备份和配置仍需正常保存到磁盘；单 EXE 指分发形式，不代表运行时不产生数据。原生后端可以减少依赖，但最终大小必须以实际构建结果计量。

现有 WinForms 依赖内置 COM 封送，微软 SDK 目前仍禁用其裁剪支持；Native AOT 又要求裁剪且限制动态加载和运行时代码生成。迁移需要替换相应实现，不能直接给当前项目增加发布开关。依据：[WinForms 裁剪限制](https://learn.microsoft.com/en-us/dotnet/core/deploying/trimming/incompatibilities#windows-forms)、[Native AOT 限制](https://learn.microsoft.com/en-us/dotnet/core/deploying/native-aot/#limitations-of-native-aot-deployment)。

## 现有功能与可复用依据

当前有 9 项常用开关、23 项本局属性，以及永久金币和两种指定奖励用途。各项开关共享连接、状态检查和生命周期规则。

| 能力 | 现有实现 | 迁移时可复用与需重做的部分 |
| --- | --- | --- |
| 9 项常用功能 | [GameAdapter.cs](../src/Trainer.Plugin/GameAdapter.cs) 中的七项常用功能、[Unity 时间倍率](../native/evidence/game-speed-route.md) 和 [宝箱动画](../native/evidence/chest-animation-route.md) | 加速和跳过动画应用于本地单人场景。金币消费保护保留普通钱包和备份授权范围，免消费自然扣款仍待专测。 |
| 14 项普通数值属性与万能组合 | [StatHooks.cs](../src/Trainer.Plugin/StatHooks.cs)、[StatCatalog.cs](../src/Trainer.Shared/StatCatalog.cs) | 已在原生端迁移，保留 ID、单位、百分比换算及单项优先；方法组、Final公式、采样与维护依据见 [普通属性接口](../native/evidence/ordinary-stats-route.md)。 |
| 其余 8 项属性 | `StatHooks.cs`：拾取范围、魅力、四种消耗次数、保留概率、去牙概率 | 拾取范围、次数和魅力使用独立临时对象，魅力为Int字段绑定；三种菜单次数消耗不回填及两次附加复活的2→1→0已实测。GC根、字段写屏障及刷新依据见 [资源与钱包接口](../native/evidence/resource-wallet-route.md)。 |
| 宝箱与升级指定奖励 | [ChestHooks.cs](../src/Trainer.Plugin/ChestHooks.cs)、[UpgradeRewardHooks.cs](../src/Trainer.Plugin/UpgradeRewardHooks.cs)、[RewardCatalog.cs](../src/Trainer.Plugin/RewardCatalog.cs) | 已迁移封闭泛型集合、嵌套作用域、共享目标和独立用途。普通升级、重选、实际取得及血笞进化已开发实测；宝箱实际领取待测。ABI和未覆盖矩阵见 [指定奖励接口](../native/evidence/reward-route.md)。 |
| 游戏物品图标 | [RewardIcons.cs](../src/Trainer.Plugin/RewardIcons.cs) | 原生端核验图集读取与 GPU 读回入口，传输有尺寸上限的 RGBA，客户端按需缓存并保留缺图回退。分发不含游戏美术。 |
| 永久金币 | `GameAdapter.cs`：普通单人主菜单的金币变更和保存 | 原生版通过按钮保存普通和冒险模式金币。主线程核对单人菜单与当前钱包，保存失败时尝试恢复原余额。 |
| 快捷键 | 托管桌面端的开关绑定和本机设置 | 原生候选支持7项常用和23项属性开关的自定义绑定、冲突提示及前台游戏范围；金币设值和指定奖励不绑定快捷键。 |
| 识别、安装、备份与恢复 | [FingerprintService.cs](../src/Trainer.Core/FingerprintService.cs)、[TrustedPayloadCatalog.cs](../src/Trainer.Core/TrustedPayloadCatalog.cs)、[InstallationService.cs](../src/Trainer.Core/InstallationService.cs)、[BackupService.cs](../src/Trainer.Core/BackupService.cs) | 原生候选已有独立备份/列表/恢复，兼容旧manifest和proof，恢复前保护快照、失败回滚、存档白名单与云元数据保留均有离线回归。完整旧插件安装日志、安装与卸载维护仍由正式版处理。 |
| 会话、主线程与断连保护 | [TrainerRuntime.cs](../src/Trainer.Plugin/TrainerRuntime.cs)、[Plugin.cs](../src/Trainer.Plugin/Plugin.cs)、[PipePeerGuard.cs](../src/Trainer.Plugin/PipePeerGuard.cs)、[Protocol.cs](../src/Trainer.Shared/Protocol.cs) | 原生端保留v1 IPC、主线程队列、会话与进程/用户/Session检查，以及1秒新鲜度、1.5秒请求期限和3秒心跳。奖励验证可取消，晚返回不得启用；已开始的金币保存返回真实事务结果。 |

当前兼容目标为 Steam Build 25984009 / 游戏 1.16.111 / Unity 6000.0.62f1。[适配配置](../config/supported-game.json)固定 GameAssembly 和 IL2CPP 元数据的 SHA-256。更换后端仍须通过版本和文件检查，不能仅凭窗口显示了功能名称就允许修改。

[NativeTelemetry.cs](../src/Trainer.Plugin/NativeTelemetry.cs)仍是托管版本的只读诊断能力。独立 C++ 后端位于 [native/backend](../native/backend)，通过当前游戏导出的 IL2CPP API 和已交叉核验的方法布局解析入口，以固定 MinHook 安装主线程、对象生命周期、属性和战斗事件入口。护甲依据见 [护甲接口](../native/evidence/armor-route.md)，生命、经验和武器伤害的入口及范围见 [战斗接口](../native/evidence/combat-route.md)，移动和武器间隔见 [移动与冷却接口](../native/evidence/movement-cooldown-route.md)。原生加载不使用托管包装或固定进程地址。

现已核对当前游戏的 IL2CPP 导出、MethodInfo 前缀及本轮属性、资源、钱包和奖励入口签名；游戏对象引用使用相应GC根和字段写屏障。奖励集合只对已核验的完整封闭类型和精确方法token允许泛型参数，不放宽普通方法校验，也不猜测字典枚举器或值结构布局。原生实现中的绝对进程地址不能作为跨启动依据。

目录首次读取采用高精度计时与固定步骤数双重限制，把等级数据、语言槽和英文翻译候选分片处理，完整后核对数据来源及语言再替换缓存。客户端在加载时重试，搜索在本机缓存中完成，不更改游戏语言。冷启动20次请求全部成功，9次返回加载中；最大IPC往返252.2915毫秒，这不是游戏帧耗时测量。分片构建继续遵守原状态新鲜度和请求期限。

现有 Runtime 隔离测试链接真实策略代码，但用假适配器替代 Unity/GameAdapter；它们可回归状态规则，不能证明原生 hook 生效。Probe 可继续用于协议和游戏流程观测，其采样也不能代替亚秒级延迟测量。已有与未覆盖的游戏场景以 [VALIDATION.md](../VALIDATION.md) 为准。

## 可逐步交付的切口

1. **先优化现有运行路径。** 本轮处理游戏诊断热路径：数值计数保留为数值，避免在高频受击/属性读取中反复解析和格式化字符串；减少未变化诊断的整份复制。分别测量游戏内和桌面端，保留心跳、请求期限和所有功能门槛。这个阶段不去掉 BepInEx，不宣称已得到原生轻量版。
2. **验收原生后端最小闭环。** 已实现固定版本核验、单人/角色状态、v1 IPC和独立原生窗口。native-pilot.2的四项功能已有上述用户反馈；native-pilot.3加入移动倍率、快速冷却和一击必杀，native-pilot.4修复控制会话误失效并完成七项的开发实机回归。后续逐项记录真实效果、关闭恢复及生命周期；实际事件计数不含主动采样。原 WinForms 的托管可信清单继续独立维护。
3. **按复杂度迁移功能。** native-pilot.5补齐普通数值与万能组合；native-pilot.6补齐临时次数、拾取范围、魅力和概率属性，以及金币控制。native-pilot.7迁移宝箱与升级指定奖励、增量目录和双语搜索；普通及进化升级已有本轮开发实测，宝箱领取仍待完成。继续核对真实取得、正常升级、关闭后的行为和单人范围。
4. **完成原生桌面与维护服务。** 原生端已有开关快捷键、本机设置、指定奖励界面、图标缓存和兼容旧数据的备份/恢复。完整旧插件安装维护由正式版处理。恢复前需退出游戏和 Steam，并建立保护快照。最终单 EXE 的体积、性能及完整功能以逐阶段构建和实机验收确定。

## 每阶段的完成验收

各阶段使用相同口径记录结果。中间版本可保留现有依赖，但须列明；最终的单 EXE 完全离线版本必须通过全部等价验收。

| 项目 | 必须记录或通过的结果 |
| --- | --- |
| 完整离线大小 | 记录 EXE/ZIP 总字节数、全部分发文件的展开字节数、首次安装到游戏/数据/临时目录的新增字节数，单列外部必需组件。宣称完全离线的候选须在断网、未预装 .NET/BepInEx 且无加载器缓存时完成首次准备，不能漏算下载、解包或另装组件。 |
| 运行开销 | 同一游戏版本、角色/关卡、敌人数、功能组合和窗口尺寸下，对比基线与候选的桌面进程及游戏进程 CPU、工作集/私有内存。分别记录菜单、战斗、搜索和连续缩放；同时观察游戏帧时间，不能只看窗口空闲内存。 |
| 交互延迟 | 从输入/快捷键到确认状态和游戏实际效果的延迟，记录重复测试分布；区分 UI 显示确认和游戏真正生效。不能用 400–500 ms 状态轮询读数充当精确开关延迟。 |
| 功能效果与恢复 | 各功能的真实效果、组合优先级、关闭恢复、覆盖期间正常升级、次数消耗、重新开局及不同支持武器。指定奖励核对实际取得物品和原有结算，不能仅检查菜单或数值显示。 |
| 保护与维护 | 错版、多人/未就绪、断连/心跳过期、迟到指令均拒绝或关闭。金币免消耗要求有效备份。备份/恢复含保护快照，安装失败可回滚，卸载保留后改文件，旧数据能维护，篡改配套文件仍被拒绝。 |

完整交付体积、同场景开销、真实游戏效果和保护/恢复均需要逐阶段验收。首轮护甲试点 EXE为839,680字节（820 KiB），内嵌的原生DLL展开为379,392字节；源码和许可证均可核对。native-pilot.3扩展到护甲和六项常用功能，体积与效果反馈随各轮记录，后续继续按上述顺序迁移。
