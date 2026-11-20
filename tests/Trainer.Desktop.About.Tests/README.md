# About dialog privacy regression

On Windows with the .NET 10 SDK, run from the repository root:

```powershell
dotnet run --project tests/Trainer.Desktop.About.Tests -c Release -- artifacts/about-privacy/regression.json
```

The project links the actual dialog, localization, release verification, and
catalog source. It creates synthetic folders under its own `.artifacts` folder,
shows the dialog offscreen, and exercises Chinese/English switching, opening a
folder through a recording/throwing delegate, and a failed file verification.
It checks control text, tooltips, accessibility text, and error text for private
path fragments. It never starts Explorer, contacts a game, reads user settings,
or accesses a real game/save installation. The tiny embedded catalog belongs
only to this test fixture; it is never packaged with the application.
