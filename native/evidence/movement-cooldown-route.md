# Native movement, fast cooldown and instant kill

Target: Steam Build `25984009`, game `1.16.111`, Unity `6000.0.62f1`, Windows x64. The fixed file hashes, IL2CPP MethodInfo prefix evidence, entry-byte comparison and lifecycle rules remain those in [armor-route.md](armor-route.md). Existing enemy-hit metadata is documented in [combat-route.md](combat-route.md).

## Exact getter and timer entries

Fresh read-only decompilation of the cached generated interoperability image `VampireSurvivors.Runtime.dll` identifies:

| Namespace and class | Method | Signature | Metadata token |
| --- | --- | --- | ---: |
| `VampireSurvivors.Objects.Characters.CharacterController` | PMoveSpeed | public virtual instance `float ()` | 100689702 |
| `VampireSurvivors.Objects.Weapons.Weapon` | PInterval | public virtual instance `float ()` | 100688833 |
| `VampireSurvivors.Objects.Weapons.Weapon` | ResetFiringTimer | public virtual instance `void ()` | 100688856 |

The Windows x64 getter ABI is `float (void* instance, const MethodInfo*)`; the reset method is invoked using `il2cpp_runtime_invoke` with the verified instance and zero explicit arguments. The MethodInfo name, token, flags, parameter count, return type, native pointer and virtual pointer must match the fixed metadata and entry checks before a hook can be created. The current player's movement virtual entry must resolve to the hooked entry. Each eligible weapon's interval and timer-reset virtual entries must resolve to the verified base entries; alternate overrides or thunks remain unsupported.

Movement multiplies the current local player's original finite PMoveSpeed result by the requested value, from 0.25 through 10. Zero and negative finite results retain the managed route's multiplication semantics. Overflow retains the original value. No speed, physics, template or save field is written. A getter callback records a real native return; the backend does not repeatedly invoke PMoveSpeed to simulate hits.

## Basic weapon interval scope

The millisecond rule matches the existing managed implementation: a positive finite PInterval return is capped at 100, corresponding to 0.1 second. Nonpositive and nonfinite values retain their original return. Eligibility requires the current local owner, a verified Weapon instance, an allowed type and both exact virtual routes.

The nine intended basic weapon types are resolved by their literal static enum fields in `VampireSurvivors.Data.WeaponType`, whose underlying type must be `System.Int32`:

| Enum name | Fixed-build value |
| --- | ---: |
| MAGIC_MISSILE | 1 |
| WHIP | 3 |
| AXE | 5 |
| KNIFE | 7 |
| HOLYWATER | 9 |
| FIREBALL | 12 |
| HOLYBOOK | 14 |
| CROSS | 16 |
| LIGHTNING | 22 |

The managed string list used `HOLY_WATER` and `BIBLE`; neither is the fixed game's corresponding enum name. This route corrects those two labels to HOLYWATER and HOLYBOOK. Evolved weapons and other special types are excluded. Runtime code obtains the values from verified enum metadata rather than embedding this table as an object layout or timer offset.

The bounded active/hidden equipment scan retains the existing exact local-owner checks. Cooldown capability requires a currently owned eligible basic weapon and rejects a mismatching eligible weapon route. The timer reset itself uses a fresh active-equipment list, exactly matching the managed reset scope; no weapon object pointer survives between frames.

## Enable and disable transitions

On Unity's established Update thread, effective cooldown state is checked immediately before and after the command queue is processed. An immediate disable followed by a queued enable between frames therefore refreshes both schedules. For each current active owned eligible weapon, ResetFiringTimer runs through the game's verified method. An enable recalculates the existing firing timer using the capped getter; a disable or session expiry recalculates it using the game's current natural getter, including any ordinary upgrades. No template snapshot or hard-coded timer field is restored.

Actor identity, current death state and teardown generation are checked across list access and each reset. If the actor becomes unavailable, unsafe, dead or destroyed, timer access stops and the transition identity is cleared. A timer refresh failure disables cooldown and forces the next equipment scan to revalidate support. A completed scan with only a route mismatch can have refreshed other valid weapons; one bounded pass with the feature already off restores those valid schedules. An invocation or state failure is never retried. Timer recalculations can themselves cause genuine native PInterval callbacks; these are effectful recalculations, rather than synthetic diagnostic samples. Closing the client clears the effective switch and the next valid Unity frame refreshes the natural timer.

## Instant kill priority and native processing

Instant kill reuses the already verified EnemyController.GetDamaged hook, with the same positive finite ordinary-enemy hit scope as native damage: a current local active/hidden equipment source, positive finite enemy HP, not dead and not a bullet. Unknown and environment sources remain unchanged. This deliberately preserves the native backend's owned-source boundary for both shared features.

The damage multiplier runs first. If instant kill is enabled, the final argument is `max(intermediateDamage, min(float.MaxValue, double(enemyHp) * 100))`. Calculation and saturation keep the argument finite, including very high HP. The original GetDamaged runs once with unchanged weapon type, visuals, knockback and other arguments, retaining the game's native resistance, invulnerability, death and reward processing. Both features can observe the same genuine HP before/after pair; these shared observations are not independent damage events and must not be added together.

## Validation boundary

The previous native pilot's four effects have user-confirmed live feedback. These three additions require their own off/on/off validation: movement across direction changes; existing and newly acquired basic weapons with cooldown enable/disable and reconnection; and owned-source ordinary-enemy hits with damage and instant kill independently and together. A route or counter alone does not establish observed movement speed, firing cadence or an enemy death. No game process is started or modified by the offline metadata and build checks described here.
