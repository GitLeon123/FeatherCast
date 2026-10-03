# Phone Screen verification

The shared C++ and Kotlin tests cover matching screen message fixtures,
Unicode text, input and packet limits, aspect-ratio mapping, and bounded
video/audio queues. The native service test also authenticates a real secondary
socket and rejects incorrect keys, incorrect phone identities, concurrent reuse,
reuse after Stop, and stale input sessions without replacing the primary link.

`FeatherCastPhoneScreenTests` generates H.264 and AAC using Windows Media
Foundation, decodes them through the production playback worker, and verifies
the output image. The optional development harness uses isolated pairing data:

```powershell
build-native/FeatherCastPhoneScreenTests.exe --live build-native/screen-e2e
```

The harness listens on port 47970 and writes an emulator pairing URI to
`pairing.txt`. It renders the production Phone Screen window, counts decoded
frames and audio packets, and reports codec errors. A `stop` marker ends it.
`capture` writes a screenshot of its own window. `command.json` can supply a
single local test operation (`button`, `mouse`, `text`, `key`, `input`, `file`)
to exercise production UI and input paths. This harness is a test executable;
the installed launcher does not expose these controls or store media.

Release validation commands:

```powershell
cmake --build build-native --config Release
ctest --test-dir build-native -C Release --output-on-failure
scripts/build-android.ps1
```

The Android script runs shared Kotlin protocol tests and release lint, then
builds a signed APK and copies it next to the EXE. Verify real devices for
Android 8/9 (video without audio), Android 10+ (permitted playback audio), and
Android 14/15 (fresh, full-display consent and rotation of an existing virtual
display). Check rejection, 60-second expiry, permission withdrawal, screen lock,
disconnect/reconnect, tap/hold/drag/scroll/navigation, Unicode paste and editing,
keyboard restoration, fullscreen, and simultaneous file transfers. Emulator
testing and native codec tests do not substitute for this physical-device matrix.

Completed on 2026-09-30: all 24 native test executables and 14 Kotlin tests
passed; release lint and the signed Android release build succeeded. Android 15
(API 35) emulator testing verified H.264 video and AAC audio, portrait/landscape
rotation within one projection, Unicode input and deletion of a whole emoji,
select-all, Home navigation, stale-generation rejection, 60-second request
expiry, previous-keyboard restoration, and a simultaneous 1 MB file transfer.
Screen lock stopped projection and restored the previous keyboard; waking the
phone did not resume sharing. The native window was also checked with fullscreen
and increased DPI/text scaling, and Android setup labels were inspected.

Physical Android 8/9, 10+, and 14/15 devices have not been tested. OEM behavior,
protected-app audio, gesture feel, and Narrator/TalkBack still require practical
validation on those devices.
