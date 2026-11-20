# Local pipe hardening checks

Run `dotnet run --project tests/Trainer.Pipe.Tests -c Release` on Windows. This
links the actual plugin pipe/peer and backup verifier source, with a fake Unity
runtime endpoint. It creates only temporary local named pipes and files, and one
short-lived child of this test executable. It never discovers/opens the game or
reads user save data.

The tests exercise native PID/session reporting, process-handle lifetime,
account-only ACL, first-instance protection, hello-before-command ordering,
timeouts/reconnection, strict UTF-8 and bounded framing, backup path restrictions,
and watchdog scheduling after an injected failure. Runtime mutation safety and
the actual three-second feature shutdown remain covered by Trainer.Runtime.Tests.

Remote rejection is set at native creation through `PIPE_REJECT_REMOTE_CLIENTS`
in `dwPipeMode`; no remote machine or SMB service is contacted/enabled by these
tests. Cross-user/session rejection is enforced in production but cannot be
impersonated by this unprivileged single-session test runner. An unsigned client
is not authenticated as an official executable by these controls.

`scripts/Verify-Source.ps1 -IncludeDesktop` builds and runs these native fixtures.
Some application sandboxes add restricted-token SIDs to child processes; their
access checks can deny this intentionally account-only pipe even when the visible
user/session matches. Run this fixture in a normal Windows user session/CI runner
rather than weakening its production ACL for sandbox compatibility. This does not
require running the trainer or game as administrator. Cross-account and remote
connections are not dynamically exercised by this suite.
