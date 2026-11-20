# Desktop search checks

Windows-only console checks link the actual desktop panel source and create an
offscreen form with native textbox/list handles. The default run uses synthetic
data only and does not read game files, saves, or installation settings.

```powershell
dotnet run --project tests/Trainer.Desktop.Tests -c Release
```

The checks cover Chinese/English/ID search and kind combinations, catalog order,
selection retention and clearing, native mouse/keyboard selection and the target
IDs they submit, same-count query changes, catalog replacement, reconnect, and
disposal. Requests are captured in memory; no game connection is opened.

Icon checks additionally use generated transparent PNGs and controlled async
responses. They verify visible-row request limits, scrolling, image caching,
old-plugin and missing-image fallbacks, uninterrupted search while a request is
pending, and rejection of old responses after catalog refresh, disconnect, or
disposal. No game artwork is included in these tests.

Hotkey checks cover the 30 toggle actions, default and unbound bindings, exact
modifier matching, one trigger per press, focus rearming, legacy-settings
migration, persistence, and stored-conflict normalization. Keyboard input is
supplied in memory; the tests do not send keys to the desktop or game.

`--benchmark` additionally measures the synchronous `TextChanged` path for typing
`p`, `po`, `pow`, `powe`, `power`, and clearing the textbox, three times each, with
344 and 4096 synthetic entries. Repaint time is recorded separately. Timing is
reported rather than used as a machine-dependent pass/fail assertion.

```powershell
dotnet run --project tests/Trainer.Desktop.Tests -c Release -- --benchmark --output artifacts/search-performance/fixed-synthetic.json
```

Optional repeated `--catalog <path>` arguments load archived response envelopes
(`response.itemCatalog`) instead of the 344-entry synthetic set. The 4096-entry
benchmark remains synthetic. `--output <path>` writes the checks and measurements
to JSON; without it no result artifact is written.

For this fix, an optional `-p:Baseline=true` build links the pre-fix panel archived
at `artifacts/search-performance/baseline/ChestRewardPanel.cs`. That archived file
is required only for this baseline comparison; normal checks use production source.
