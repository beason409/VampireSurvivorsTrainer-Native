# Native ordinary-stat routes

Target: Steam Build `25984009`, game `1.16.111`, Unity `6000.0.62f1`, Windows x64. File identity, exported APIs, the fixed MethodInfo prefix, executable entry checks and teardown rules remain those in [armor-route.md](armor-route.md). Movement, weapon intervals and combat composition also retain [movement-cooldown-route.md](movement-cooldown-route.md) and [combat-route.md](combat-route.md).

## Verified method declarations

Read-only Mono.Cecil inspection of the cached generated `VampireSurvivors.Runtime.dll` confirmed these wrapper declarations and the native metadata tokens supplied by each wrapper's static initializer to `GetIl2CppMethodByToken`. Tokens are metadata identities, not process addresses. All methods below are instance methods on `VampireSurvivors.Objects.Characters.CharacterController` unless another type is named.

| Stat / purpose | Method | Return and explicit parameters | Native metadata token |
| --- | --- | --- | ---: |
| Maximum health | MaxHp | virtual `float ()` | 100689700 |
| Maximum health display | DisplayMaxHp | virtual `float ()` | 100689701 |
| Armor, existing shared hook | PArmor | virtual `float ()` | 100689690 |
| Recovery | PRegen | virtual `float ()` | 100689699 |
| Might | PPower | virtual `float ()` | 100689709 |
| Might final path | PPowerFinal | nonvirtual `float ()` | 100689707 |
| Might alternate path | PPowerWithoutSilentMight | nonvirtual `float ()` | 100689708 |
| Projectile speed | PSpeed | virtual `float ()` | 100689695 |
| Duration | PDuration | virtual `float ()` | 100689696 |
| Area | PArea | virtual `float ()` | 100689698 |
| Area with weapon factor | PAreaFinal | virtual `float (float preClampMultiplier)` | 100689697 |
| Cooldown | PCooldown | virtual `float ()` | 100689704 |
| Cooldown with floor | PCooldownFinal | virtual `float (float cap)` | 100689703 |
| Additional projectiles | PAmount | virtual `float ()` | 100689705 |
| Movement, existing shared hook | PMoveSpeed | virtual `float ()` | 100689702 |
| Luck | PLuck | virtual `float ()` | 100689693 |
| Growth | PGrowth | virtual `float ()` | 100689692 |
| Greed | PGreed | virtual `float ()` | 100689694 |
| Curse | PCurse | virtual `float ()` | 100689691 |
| Current-health setter | set_CurrentHp | nonvirtual `void (float)` | 100689448 |
| Current stage, on GameManager | get_Stage | nonvirtual `Stage ()` | 100694953 |
| Curse refresh, on Stage | RecalculateCurseAndCharm | nonvirtual `void ()` | 100672632 |

The Windows x64 getter shapes are `float (void* instance, const MethodInfo*)` and `float (void* instance, float argument, const MethodInfo*)`. The setter and stage refresh use `il2cpp_runtime_invoke`; no object-field offset or process address is embedded.

Each attribute group requires all of its listed return paths, exact signatures/tokens and executable entries. Every virtual path must resolve to the verified base entry for the current local character. Missing or alternate routes disable that stat. The four groups used by unified bonuses must all be available. Ambiguous native entry sharing between different stats or different getter signatures is refused; identical signatures shared within the same stat can reuse one hook. Optional stat failures do not make unrelated player observations or gameplay features fail globally.

## Values and composition

The fourteen ordinary definitions reuse the managed `StatCatalog` ranges and units: maximum health, armor, recovery and amount use absolute values; might, projectile speed, duration, area, movement speed, luck, growth, greed and curse use `1 + displayPercent / 100`; cooldown reduction uses `1 - displayPercent / 100`. Amount remains an integer-valued input even though its native getter returns a float. Unified bonuses apply the same percentage to might, projectile speed, duration and area, with an enabled individual stat taking priority.

`PAreaFinal` retains the supplied weapon factor: the selected raw area target is multiplied by `preClampMultiplier`. `PCooldownFinal` retains the game's supplied floor: its selected result is `max(cap, 1 - reduction / 100)`. These match the managed `AreaStatFinalPostfix` and `CooldownStatFinalPostfix`; the argument is not another display percentage. The original native method is called once before its result is filtered.

The movement getter applies the absolute movement-speed stat first and the common movement multiplier second. Growth affects the native experience calculation before the common AddXp multiplier. Might affects the game's native weapon calculation before the common enemy-hit damage multiplier and one-hit-kill processing. Fast cooldown still caps eligible basic-weapon interval returns after the character cooldown calculation. None of these return overrides writes the normal modifier backing objects, permanent upgrades, wallet or save.

## Sampling and main-thread maintenance

Normal stat values and complete per-character virtual-route groups are sampled at most every 250 milliseconds, cached only for that observed player and invalidated when the player becomes unavailable. The regular player-readiness observation and lifecycle checks continue every Unity Update. Diagnostic sampling bypasses all stat and feature overrides and does not increment natural getter counts. An unavailable or nonfinite optional sample disables that stat and its dependent unified capability rather than manufacturing a normal value.

Effective maintenance is distinct from original-value sampling: internal refresh reads see current overrides but do not increment natural getter counts or replace genuine input/output receipts. The shared movement and interval hooks preserve feature composition during an effective refresh without creating synthetic gameplay evidence. Parameterized final returns are diagnosed separately from the plain character-value sample.

Maximum-health and curse transitions are checked immediately before and after queued main-thread commands. No repeated write is made each frame. An enabled maximum-health target never heals the player; lowering or disabling it calls the verified original CurrentHp setter only if current HP exceeds the new effective maximum. The original MaxHp is read again for a transition, so normal upgrades are retained. A live session timeout, explicit disable or disconnect also changes the effective state and schedules this downward-only restoration on the next safe current-player frame.

Curse enable, target change and disable use the current manager's fresh Stage and its original `RecalculateCurseAndCharm`. This updates the game's existing stage calculations; overriding a panel getter alone would be insufficient. A failed refresh disables curse and leaves a natural refresh pending for a subsequent safe frame. Transition tracking is never used as an object reference: instance access requires a currently observed local actor, verified class, not-dead state and unchanged teardown generation. No worker or watchdog thread invokes Unity. Temporary unsafe observations defer maintenance until a fresh matching actor is available; destruction clears the transition identity entirely.

## Validation boundary

Metadata/ABI checks, compilation and isolated policy tests establish the declared routes and control behavior. Live gameplay still needs to establish natural reads, actual recovery and health limits, supported weapon behavior, growth/greed/curse consumers, unified priority, normal upgrades and off/on/off transitions. Getter samples and accepted commands alone do not establish those game effects. Preparing this evidence does not start or modify the game.
