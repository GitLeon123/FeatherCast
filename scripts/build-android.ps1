# Builds and signs the FeatherCast phone app (FeatherCast-Phone.apk).
# Uses the toolchain from scripts/setup-android-sdk.ps1. The release keystore is
# created once under %LOCALAPPDATA%\FeatherCast-dev and never stored in the repo;
# keep it, because Android only installs updates signed with the same key.
[CmdletBinding()]
param(
  [string]$DevRoot = (Join-Path $env:LOCALAPPDATA 'FeatherCast-dev'),
  [string]$NativeBuildDir = 'build-native',
  [switch]$SkipTests
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$repo = Split-Path -Parent $PSScriptRoot

if (-not $env:JAVA_HOME -or -not (Test-Path (Join-Path $env:JAVA_HOME 'bin\java.exe'))) {
  $env:JAVA_HOME = Join-Path $DevRoot 'jdk17'
}
if (-not $env:ANDROID_HOME -or -not (Test-Path $env:ANDROID_HOME)) {
  $env:ANDROID_HOME = Join-Path $DevRoot 'android-sdk'
}
if (-not (Test-Path (Join-Path $env:JAVA_HOME 'bin\java.exe')) -or -not (Test-Path $env:ANDROID_HOME)) {
  throw "Android toolchain missing. Run scripts\setup-android-sdk.ps1 first."
}

# Local release keystore.
$keystore = Join-Path $DevRoot 'phone-release.jks'
$secretFile = Join-Path $DevRoot 'phone-release.password'
if (-not (Test-Path $keystore)) {
  New-Item -ItemType Directory -Force $DevRoot | Out-Null
  $bytes = New-Object byte[] 24
  [System.Security.Cryptography.RandomNumberGenerator]::Fill($bytes)
  $password = [Convert]::ToBase64String($bytes) -replace '[+/=]', 'x'
  Set-Content -Path $secretFile -Value $password -NoNewline -Encoding ascii
  & (Join-Path $env:JAVA_HOME 'bin\keytool.exe') -genkeypair -noprompt `
    -keystore $keystore -storetype PKCS12 -storepass $password -keypass $password `
    -alias feathercast -keyalg EC -groupname secp256r1 -validity 10000 `
    -dname 'CN=FeatherCast Phone, O=FeatherCast'
  if ($LASTEXITCODE -ne 0) { throw 'keytool failed.' }
  Write-Host "Created release keystore at $keystore"
}
$password = (Get-Content -Path $secretFile -Raw).Trim()

$tasks = @(':app:lintRelease', ':app:assembleRelease')
if (-not $SkipTests) { $tasks = @(':protocol:test', ':app:testReleaseUnitTest') + $tasks }

Push-Location (Join-Path $repo 'android')
try {
  $signingVariables = @{
    ORG_GRADLE_PROJECT_fcStoreFile = $keystore
    ORG_GRADLE_PROJECT_fcStorePassword = $password
    ORG_GRADLE_PROJECT_fcKeyAlias = 'feathercast'
    ORG_GRADLE_PROJECT_fcKeyPassword = $password
  }
  $previousSigningVariables = @{}
  foreach ($name in $signingVariables.Keys) {
    $previousSigningVariables[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    [Environment]::SetEnvironmentVariable($name, $signingVariables[$name], 'Process')
  }
  & .\gradlew.bat --no-daemon @tasks
  if ($LASTEXITCODE -ne 0) { throw 'Gradle build failed.' }
} finally {
  foreach ($name in $previousSigningVariables.Keys) {
    [Environment]::SetEnvironmentVariable($name, $previousSigningVariables[$name], 'Process')
  }
  Pop-Location
}

$apk = Join-Path $repo 'android\app\build\outputs\apk\release\app-release.apk'
if (-not (Test-Path $apk)) { throw "APK not found: $apk" }

$buildTools = Get-ChildItem (Join-Path $env:ANDROID_HOME 'build-tools') -Directory |
  Sort-Object { [version]($_.Name -replace '[^0-9.]', '') } | Select-Object -Last 1
if (-not $buildTools) { throw 'Android build-tools missing. Run scripts\setup-android-sdk.ps1 first.' }
$verification = & (Join-Path $buildTools.FullName 'apksigner.bat') verify --verbose --print-certs $apk |
  Out-String
if ($LASTEXITCODE -ne 0) { throw "apksigner verify failed.`n$verification" }
# Phones only accept updates signed with the same key; never ship the debug key.
if ($verification -match 'CN=Android Debug') { throw 'The APK is signed with the Android debug key.' }
Write-Host $verification.Trim()

# FeatherCast serves the APK from next to its exe (http://<pc>:47800/app.apk).
$native = Join-Path $repo $NativeBuildDir
$targets = @((Join-Path $native 'FeatherCast-Phone.apk'), (Join-Path $native 'packages\FeatherCast-Phone.apk'))
$configuredNative = Join-Path $native 'Release'
if (Test-Path $configuredNative -PathType Container) {
  $targets += Join-Path $configuredNative 'FeatherCast-Phone.apk'
}
foreach ($target in $targets) {
  New-Item -ItemType Directory -Force (Split-Path -Parent $target) | Out-Null
  Copy-Item $apk $target -Force
}
$size = [math]::Round((Get-Item $apk).Length / 1MB, 1)
Write-Host "FeatherCast-Phone.apk ($size MB) copied to:"
$targets | ForEach-Object { Write-Host "  $_" }
