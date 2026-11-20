# Desktop stat layout and hotkey checks

Windows-only fixtures link the actual stat panel, layout engines, localization,
and shared protocol source. They create an offscreen native WinForms window and
use synthetic states plus an in-memory request handler. No game connection,
installation service, save files, user settings, or private artifact inputs are
used.

Run with .NET SDK 10 on a Windows desktop session:

```powershell
dotnet run --project tests/Trainer.Desktop.Layout.Tests -c Release
```

An optional first argument chooses the JSON report path:

```powershell
dotnet run --project tests/Trainer.Desktop.Layout.Tests -c Release -- artifacts/resize-hotkeys/layout-regression.json
```

The default report is `artifacts/stat-layout/result.json`. Chinese and English
panel screenshots are saved beside the report, including three resize widths.

The checks cover the 23 displayed stats, omission of Seal, binding callbacks,
long hotkey captions, enabling with the edited target, disabling from actual
state, connection/capability/pending guards, and activation while filtered or
on a hidden tab. They also cover language and dirty-value retention, unchanged
English text across repeated states, header alignment, fixed-column rails,
offscreen row layout, scrolling and keyboard focus, and restoration of
scaled row heights after filtering.

These are source-level UI and protocol-routing checks. They do not verify live
game effects, actual global keyboard polling, monitor DPI transitions, or
visible-window compositor frame rate. Timing comparisons use the separate
native resize probe.
