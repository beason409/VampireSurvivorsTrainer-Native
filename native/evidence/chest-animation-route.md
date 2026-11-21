# Native chest animation skip

Target: Steam Build `25984009`, game `1.16.111`, Unity `6000.0.62f1`, Windows x64. The backend applies the [fixed game and entry checks](armor-route.md).

The generated interop metadata and read-only disassembly of the installed GameAssembly agree on these instance methods in `VampireSurvivors.UI.OpenTreasurePage`.

| Method | Signature | Native metadata token |
| --- | --- | ---: |
| Update | `void ()`, virtual | 100670116 |
| TweenCoins | `void ()` | 100670152 |
| PerformSkip | `void ()` | 100670160 |

The native Update advances the unscaled playback clock. It accepts the animation skip input while playback is active and `_animCanBeSkippedPastThisPoint` is true. `TweenCoins` sets that readiness flag, and single-player `Skip` calls `PerformSkip`. That method guards against repeat skips, advances native playback, and handles the existing sound and tween transitions. The separate claim path calls `Treasure.ClaimPrizes`.

The hook checks immediately after the original `TweenCoins` sets the skip flag and on both sides of the original Update. A state already ready when Update begins can be skipped before the frame advances the animation. Each check runs on the established game thread with the current local player, the ordinary treasure and playback objects, chest tier 1 through 3, and fresh control authorization. The game owns and resets `_isSkipped`. The backend counts only a confirmed skip. A native no-op leaves the toggle available for the next valid check. A failing invocation disables the feature. Reward generation and claim run through their native routes.
