# Trainer.Probe

本机插件协议的命令行观测与显式命令回放工具。默认仅发送 `hello`、`getState`、`heartbeat`，不会发送 `setFeature`、`setGold` 或结束时的 `disableAll`。协议连接本身会创建新的插件控制会话，因此请勿与桌面控制器同时运行。

```powershell
dotnet '.\tools\Trainer.Probe\bin\Release\net10.0\Trainer.Probe.dll' --pid 12345 --duration 10 --output '.\artifacts\probe.jsonl'
```

所有带值参数均接受 `--name=value` 和 `--name value`。不传 `--pid` 时，要求本机恰好有一个 `VampireSurvivors` 进程。

## 一次性 JSONL 回放

`--commandsfile`（也可写 `--commands-file`）按文件顺序执行，每行一个完整的协议请求对象。空白行忽略，最后一个完整对象可不带换行。有限回放文件中的坏 JSON 会在连接游戏前报告错误。

```jsonl
{"kind":"getState"}
{"kind":"getGold"}
```

```powershell
dotnet '.\tools\Trainer.Probe\bin\Release\net10.0\Trainer.Probe.dll' --pid 12345 --commandsfile '.\artifacts\commands.jsonl' --duration 5
```

若文件包含 `setFeature`、`setGold` 等命令，它们属于显式修改请求；工具退出前会尝试发送 `disableAll`，但不会回滚已经保存的金币。`--abrupt` 可用于专门验证插件断连保护，跳过这次清理请求。

## 监控期间追加命令

使用 `--inbox path.jsonl` 监控命令文件。每条命令以换行提交；未写完、尚未换行的末尾内容会留到下一轮，不解析、不丢弃。空白行和 `Set-Content -Value ''` 产生的 CRLF 会忽略。已提交的无效 JSON 只向标准错误输出告警并跳过，后续有效命令继续执行。清空或改写已消费前缀会被视为新的收件箱。

```powershell
Add-Content -LiteralPath '.\artifacts\commands-inbox.jsonl' -Value '{"kind":"getState"}'
```

`{"kind":"exit"}` 只结束本探针，不会发往游戏。原有 `--steps file.json` 继续接受含 `atMs` 的定时步骤数组。

## 按未暂停的实玩时间执行短窗计划

`--gameplay-steps file.json` 使用相同的步骤数组，`atMs` 表示累计的未暂停实际毫秒。只有插件明确返回 `playerReady: true` 且 `diagnostics["game.paused"]` 可解析为 `False` 时才能执行步骤。缺字段、坏值、玩家未就绪都显示等待；暂停、升级弹窗、游戏结束或重启由插件合成的 `game.paused` 阻止推进。零秒步骤也等待有效状态。

```json
[
  { "atMs": 0, "kind": "disableAll" },
  { "atMs": 3000, "kind": "setFeature", "feature": "experience", "enabled": true, "value": 2 },
  { "atMs": 8000, "kind": "setFeature", "feature": "experience", "enabled": false },
  { "atMs": 12000, "kind": "exit" }
]
```

运行本项目的90秒分段计划（`--pid` 可省略，要求恰好一个游戏进程）：

```powershell
dotnet '.\tools\Trainer.Probe\bin\Release\net10.0\Trainer.Probe.dll' --gameplay-steps '.\tools\Trainer.Probe\plans\solo-short-windows.json' --duration 300 --output '.\artifacts\solo-short-windows.jsonl'
```

`--duration` 仍限制观测循环的**壁钟秒数**，等待菜单不会延长上限。到上限不再执行步骤，未完成计划返回非零退出码；到 `exit` 步骤可提前结束。连接及握手在观测窗口之前，结束时另留最多5秒尝试 `disableAll` 并核对回应。玩法计划即使一直等待、未执行修改，也会尝试这次清理；不能与 `--abrupt` 合用。与桌面窗口互斥使用；需要金币权限时另传本次有效备份的 `--token`。

原生试点的 `plans/native-combat.json` 依次安排生命保护关闭/开启/关闭、2倍经验和2倍伤害，最后关闭全部并退出。先通过原生界面连接后端，再关闭窗口，使用该计划观测普通单人关卡；实验前备份当前进度。计划只切换功能，受击、拾取经验和武器命中仍需要在关卡中自然发生；检查 `native.<feature>.actualCalls` 与 HP/XP 前后值，不能把步骤执行成功当成效果通过。

`plans/native-extended.json` 用于 native-pilot.3 的移动倍率、基础武器快速冷却与一击必杀。它保持生命保护，依次比较2倍/0.5倍/正常移动，冷却开启/关闭，以及2倍伤害与一击必杀的组合；最后关闭全部并退出。冷却使用已适配的基础武器，检查真实读取和自然攻击节奏；一击必杀关闭后应继续保留尚未关闭的伤害倍率。

`plans/native-stats.json` 用于 native-pilot.5 的开启中改值与15项属性。它先用省略enabled的value-only请求更新仍开启的移动倍率，再核对属性与倍率组合、生命上限下调/恢复、Omni单项优先及全部属性关闭。最后关闭所有功能。自然getter与合成采样分开计数；协议步骤成功仍需结合实际返回、HP变化和游戏画面确认。

`plans/native-resources.json` 用于 native-pilot.6 新增8项属性的普通单人关卡检查，约40秒未暂停时间。计划保持生命保护，比较拾取范围300/80/关闭、魅力5/2/关闭、保留和去牙概率0%/100%，以及重选、跳过、排除、复活次数的开启/改值/关闭；随后重新开启8项并用`disableStats`恢复，再向已关闭属性发送value-only请求，确认没有重新开启，最后`disableAll`。它不设置永久金币，也不自动点击菜单消费次数。

拾取范围、次数和魅力的`boundValue`表示当前临时绑定或剩余预算，不作为天然getter读取。魅力为Int临时字段绑定，实机已记录绑定值5→2，关闭后恢复正常100；该轮天然getter读取和改写计数均为0，不能把绑定采样当作getter调用。次数消费、正常升级隔离、死亡生命周期和菜单显示需结合真实操作验证；去牙需要观察随后生成的敌人。

```powershell
dotnet '.\tools\Trainer.Probe\bin\Release\net10.0\Trainer.Probe.dll' --gameplay-steps '.\tools\Trainer.Probe\plans\native-resources.json' --duration 600 --output '.\artifacts\native-resources.jsonl'
```

最终轮开发记录完成该计划39/39步、40.175秒未暂停时间、573.131秒观测壁钟，1471条响应全部成功。另行操作验证保留概率0时重选、跳过、排除各消耗1次，目标10降至绑定值9且持续不回填，关闭后恢复正常资源；该窗口378条响应全部成功。正常复活基线为0时，目标2已支持两次实际复活，剩余2→1→0，后续暂停观测仍为0且未回填。去牙生成效果及免消费自然扣款尚未专测。完整范围见 [验证记录](../../VALIDATION.md)。

`plans/native-rewards.json` 是指定奖励的短窗控制计划：先开启魔杖的两种用途，仅关闭宝箱，再将仍开启的升级目标换为斧头，最后只开启宝箱并全部关闭。它不生成宝箱、不发放物品、不点击升级菜单。使用未满级且当前内容可用的目标；已有满级装备时应另存计划并更换目标。需要验证领取时，用命令收件箱保持对应用途开启，等待游戏自然生成升级或宝箱，并在游戏内选择/领取。检查 `chestReward.processedCount`（宝箱实际取得）、`upgradeProcessedCount`（提供选项）及 `upgradeGrantedCount`（升级实际取得），不能用计划完成或菜单出现代替获得证明。选择其他项、重选、跳过、排除、关闭和断连需分别验证；已生成的原生奖励不会因随后关闭而追溯撤回。

每次状态转换、步骤确认与计划结束会写入 `probeEvent: "gameplayPlan"` 记录，含 `wallMs`、`gameplayMs`、`status`、`completedSteps` 和 `nextAtMs`。修改命令被拒绝会立即报错、停止后续步骤并清理，失败步骤不会计为完成。

计时使用 `Stopwatch`，不使用会受 Hurry 影响的关卡时钟。只累计相邻两个明确可运行状态之间的间隔，排除进入/离开暂停或未知状态的边界间隔；插件诊断250毫秒采样、Probe约400毫秒轮询，属于采样精度，不适合亚秒级性能比较。`--steps` 继续按壁钟推进，两种步骤模式不能同时传入。

每条正常响应另有 `requestMs`，表示本机 IPC 往返及响应解码耗时，方便定位读取目录或排队超时；它不是游戏帧耗时或 hook 的 CPU 开销。清理阶段补收的旧响应没有完整的起始样本，记为 `null`。

只运行解析与计时的离线测试，不建立任何管道：

```powershell
dotnet '.\tests\Trainer.Probe.Tests\bin\Release\net10.0\Trainer.Probe.Tests.dll' --offline-only
```
