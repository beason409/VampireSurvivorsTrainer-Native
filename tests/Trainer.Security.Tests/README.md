# Companion-file integrity and bounded-response checks

These source-level fixtures use fake payload files below this test project's
`.artifacts` directory and in-memory streams. They never connect to a game,
install a real plugin, read user settings, or access real saves.

```powershell
dotnet run --project tests/Trainer.Security.Tests -c Release -- artifacts/security-hardening/catalog-regression.json
```

They cover modified, missing, extra and hidden files; immutable expectations;
invalid/traversal/device/duplicate inventory paths; compatibility-file byte
snapshots; installed-plugin checks while retaining generated caches; and
bounded, chunked UTF-8 response reads, cancellation and truncated input.

A source build without an embedded inventory is allowed to compile for tests
and UI smoke checks. Installation must fail closed until the build/packaging
scripts have generated and embedded the inventory. An intact official program
is the trust anchor; these tests do not establish Windows publisher identity,
antivirus reputation, or protection after the whole program is replaced.
