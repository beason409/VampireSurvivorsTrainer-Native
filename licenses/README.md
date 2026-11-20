# Upstream license material

These files preserve the licenses, copyright statements, and third-party notices of the fixed Windows x64 dependency baseline. They do not license Vampire Survivors Trainer's own code, change any upstream rights, certify legal compliance, or establish that a binary is safe.

`components.json` associates every file of the original BepInEx build 793 distribution with an attributed component and SHA-256. It covers 228 original distribution files, including all 37 files / 32 DLLs in `BepInEx/core`, the native loader, and the embedded runtime. It also identifies the .NET 10.0.13 runtime packs used by the self-contained desktop build. A later build must refresh this information if its dependencies change.

Source metadata is preserved in:

- `nuget-components.json` and the package-specific `.nuspec` files: actual package IDs, versions, original license declarations, source references, and downloaded package hashes. Some original declarations are license filenames or absent; the resolved license/text association is in `components.json`.
- `fixed-source-components.json`: official fixed source revisions and original license / notice texts for BepInEx, Cpp2IL, Unity Doorstop, Dobby, and BepInEx's Mini CoreCLR.
- `desktop-runtime-components.json`: Microsoft .NET 10.0.13 pack metadata and the WPF / WinForms notices from their fixed source tree.

The `.nuspec` documents and license texts were read from the official NuGet feeds configured by the upstream projects, their fixed GitHub source revisions, or the local official runtime packages. No downloaded program was executed to collect these documents. Full third-party library source can be obtained using the fixed repository / source archive links in the inventory and [THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md).

The LGPL libraries remain separate replaceable files. Any terms applied to the trainer's own code must preserve applicable third-party modification, replacement, redistribution, and reverse-engineering rights for debugging modified libraries. Compatibility and installation checks do not alter those licenses.

## Refreshing the inventory

With the original dependency files present in `dependencies/bepinex`, the two refresh scripts download public metadata and license texts into this directory only. They do not install or run downloaded software. Review the URLs and any new component before updating a release.

```powershell
./licenses/refresh-upstream.ps1 -IncludeRuntime
./licenses/refresh-fixed-sources.ps1
./licenses/build-inventory.ps1
```

`build-inventory.ps1` fails on an unattributed BepInEx distribution file or a missing required license text. It is a source/attribution check, not a vulnerability scanner. The desktop runtime metadata was collected from the exact locally cached .NET 10.0.13 packages; updating those packages also requires refreshing the corresponding notices.
