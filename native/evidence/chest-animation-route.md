# Native chest animation skip

Target: Steam Build `25984009`, game `1.16.111`, Unity `6000.0.62f1`, Windows x64. The backend applies the [fixed game and entry checks](armor-route.md).

The generated interop metadata and read-only disassembly of the installed GameAssembly agree on these instance methods in `VampireSurvivors.UI.OpenTreasurePage`.

| Method | Signature | Native metadata token |
| --- | --- | ---: |
| Update | `void ()`, virtual | 100670116 |
| PerformSkip | `void ()` | 100670160 |

The native Update advances the unscaled playback clock. It accepts the animation skip input while playback is active and `_animCanBeSkippedPastThisPoint` is true. `TweenCoins` sets that readiness flag, and single-player `Skip` calls `PerformSkip`. That method guards against repeat skips, advances native playback, and handles the existing sound and tween transitions. The separate claim path calls `Treasure.ClaimPrizes`.

The hook runs the original Update first. On the established game thread, it checks the current local player, the ordinary treasure and playback objects, chest level 1 through 3, and the native playback flags. With the toggle enabled and a fresh control session, it calls `PerformSkip` at the same playback boundary. The game resets and owns `_isSkipped`; the backend counts the call only after reading that flag back. Reward generation and claiming continue through the original paths. An error disables the feature.
