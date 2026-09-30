# Code review for 0.11.0

Reviewed the pending phone companion, launcher integration, animation changes,
build scripts, and release workflow. The review focused on ownership, concurrency,
permission switches, bounded work, and shipped artifacts. It did not attempt a
repository-wide architectural rewrite.

## Findings addressed

| Finding | Fix |
| --- | --- |
| Shutdown and receive workers both closed the same socket; late workers could escape shutdown. | Sessions own socket lifetime, shutdown interrupts I/O, and the accept loop joins before worker collection. |
| Concurrent pairing requests could both consume an invite. | Recheck and consume the invite under the service lock; serialize state-file writes and report persistence failures. |
| Unpairing only disconnected the active session, leaving an issued challenge usable. | Interrupt matching pending sessions and revalidate the pairing before activation. |
| Outgoing file workers selected the active phone after reading the file. | Retain the original session and reject transfers after it is replaced. |
| Size metadata was trusted before unbounded file reads. | Bound actual reads on Windows and Android, and enforce incoming transfer limits. |
| Reconnect loops checked the long-lived scope instead of their own cancellation. | Use the current coroutine context, join the previous loop, and keep beacon and ping tasks within the session's coroutine lifetime. |
| Turning off Phone Connection could leave stale data and queued events active. | Clear stored phone data and ignore queued phone events while disabled. |
| APKs were built separately but omitted from tagged Windows packages and releases. | Make Windows builds consume the tested APK, require persistent signing on tags, and publish the APK with a checksum. |
| Android release lint failed on an API 29 helper despite its guarded caller. | Annotate the helper's API requirement and run lint in local and CI release builds. |
| Gradle signing passwords appeared in command arguments. | Supply signing properties through process environment variables and restore them after the local build. |

Tests cover concurrent single-use pairing, unpairing during authentication,
stopping and restarting, expired invites after stopping, bounded Android reads,
invalid ports, and malformed pairing URIs. Existing tests cover the wire fixtures,
crypto vectors, phone data model, launcher layout, motion, and accessibility.

## Remaining maintainability concern

`native/src/main.cpp` remains large and owns phone view composition alongside the
existing launcher UI. Extracting that composition into a focused module would
make future changes easier to review. This is a follow-up refactor; moving the UI
in this release would add regression risk without correcting another known defect.

Physical device and manual Windows QA limitations are recorded in the release notes.
