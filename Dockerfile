# Verification image only. It does not run the WinForms UI, game, injector,
# BepInEx loader, or complete IL2CPP plugin and is not an end-user dependency.
# Official Microsoft SDK image/tag digest verified on 2026-11-14.
FROM mcr.microsoft.com/dotnet/sdk:10.0.402-noble@sha256:747cbade5bfd918a175ac32f8f1d931edec2c7d0d5397a3de72eaef1ac1f66d2 AS portable-checks
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1 \
    DOTNET_NOLOGO=1
WORKDIR /source
COPY Directory.Build.props global.json ./
COPY src/Trainer.Shared/ src/Trainer.Shared/
COPY src/Trainer.Plugin/TrainerRuntime.cs src/Trainer.Plugin/DiagnosticStore.cs src/Trainer.Plugin/HookDiagnostics.cs src/Trainer.Plugin/
COPY tests/Trainer.Runtime.Tests/ tests/Trainer.Runtime.Tests/
COPY tests/Trainer.Probe.Tests/ tests/Trainer.Probe.Tests/
COPY tools/Trainer.Probe/CommandFiles.cs tools/Trainer.Probe/StepPlans.cs tools/Trainer.Probe/
RUN dotnet build tests/Trainer.Runtime.Tests/Trainer.Runtime.Tests.csproj -c Release -m:1 -p:TargetFrameworks=net10.0 -p:TargetFramework=net10.0 -p:BuildInParallel=false /nodeReuse:false \
    && dotnet build tests/Trainer.Probe.Tests/Trainer.Probe.Tests.csproj -c Release -m:1 -p:TargetFrameworks=net10.0 -p:TargetFramework=net10.0 -p:BuildInParallel=false /nodeReuse:false \
    && dotnet tests/Trainer.Runtime.Tests/bin/Release/net10.0/Trainer.Runtime.Tests.dll \
    && dotnet tests/Trainer.Probe.Tests/bin/Release/net10.0/Trainer.Probe.Tests.dll --offline-only
CMD ["sh", "-c", "dotnet tests/Trainer.Runtime.Tests/bin/Release/net10.0/Trainer.Runtime.Tests.dll && dotnet tests/Trainer.Probe.Tests/bin/Release/net10.0/Trainer.Probe.Tests.dll --offline-only"]
