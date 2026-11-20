# Maintenance and integrity-check concurrency

```powershell
dotnet run --project tests/Trainer.Desktop.Maintenance.Tests -c Release
```

This Windows fixture references the actual desktop project and creates an
offscreen MainForm in diagnostic mode. A controlled poll-completion task and a
read-locked fake DLL verify that maintenance invalidates old poll results,
waits for read handles before writing, blocks overlapping actions, and restores
the UI. It never invokes a real installation action, connects to the game, or
reads/writes user settings or saves. Fake files remain under this test project.
