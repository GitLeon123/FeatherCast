# Windows-key input broker

The NSIS installer requests administrator approval once. It registers a
`FeatherCast Input Broker-<user SID>` Task Scheduler task for the signed-in user
with `HighestAvailable` privileges and `InteractiveToken` logon. No password is
stored. The task starts at sign-in, and FeatherCast can run it again on demand
after the launcher/broker exits. Starting FeatherCast does not request UAC.

The task launches only `bin\InputBroker.exe` from the installed copy. The
installer checks that the executable and its parent folders are protected from
non-administrator writes and owned by Administrators, SYSTEM, or TrustedInstaller.
Install in Program Files. Portable and development builds use a broker with
normal user permissions, so their Windows-key handling over elevated apps is
subject to Windows integrity restrictions. An administrator account with a
split UAC token is required for the per-user task to gain elevated privileges;
standard accounts cannot gain administrator privileges through this task.

The broker's pipe and mutex are scoped to the user and session. Only the launcher
beside the broker can register its window. The broker intercepts lone Windows-key
presses only while that launcher is connected and the bare Windows key is its
selected search shortcut. Shortcut changes are sent through the pipe immediately.

The installer stops matching tasks before updates. Uninstall stops and removes
all FeatherCast broker tasks whose action points at that installation, including
tasks registered for other users. Other installations' tasks are left alone.

Run `scripts\test-input-broker-task.ps1` to check the task policy and validate its
XML with Windows Task Scheduler without registering anything. The installed
portion of `scripts\package-smoke.ps1` requires administrator rights and checks
task registration, repeat installation, and removal. Use a disposable test
machine for installed package smoke tests.
