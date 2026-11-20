# Native game speed

Target: Steam Build `25984009`, game `1.16.111`, Unity `6000.0.62f1`, Windows x64. The supported-game fingerprint and native entry checks are those in [armor-route.md](armor-route.md).

Read-only Mono.Cecil inspection of the generated `UnityEngine.CoreModule.dll` wrapper identified these static methods on `UnityEngine.Time`. The tokens below are the game tokens passed to `GetIl2CppMethodByToken`.

| Method | Signature | Metadata token |
| --- | --- | ---: |
| get_timeScale | `float ()` | 100668873 |
| set_timeScale | `void (float)` | 100668874 |

The adapter resolves both full signatures and verifies their distinct executable entries before publishing support. Getter and setter calls use the verified IL2CPP invoker on the established game thread. The selected value is an absolute scale of 1–10, with 4 as the initial value. The feature extends the existing single-player main-thread queue, observation freshness and heartbeat gates.

The adapter leaves zero time scale to the native pause flow. It tracks the latest positive native scale and restores it when a live value still matches the trainer's last write. A positive scale changed by the game becomes the new baseline. If the game is paused at zero when the switch is turned off, the next positive scale determines whether a restore is needed. This also covers a pause flow that resumes using the previous override. Scene exit, disconnect and loss of a valid local player release the override on the game thread. Time scale writes are checked by reading back the value. The physics timestep and save data are outside this route.
