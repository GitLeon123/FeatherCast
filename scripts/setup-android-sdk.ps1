# Downloads a self-contained Android build toolchain (JDK 17, Android SDK
# command-line tools, Gradle) into %LOCALAPPDATA%\FeatherCast-dev so the phone
# app can be built without Android Studio. Safe to run repeatedly.
[CmdletBinding()]
param(
  [string]$Root = (Join-Path $env:LOCALAPPDATA 'FeatherCast-dev')
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$ProgressPreference = 'SilentlyContinue'

$jdkUrl = 'https://api.adoptium.net/v3/binary/latest/17/ga/windows/x64/jdk/hotspot/normal/eclipse'
$toolsUrl = 'https://dl.google.com/android/repository/commandlinetools-win-11076708_latest.zip'
$gradleVersion = '8.10.2'
$gradleUrl = "https://services.gradle.org/distributions/gradle-$gradleVersion-bin.zip"

New-Item -ItemType Directory -Force $Root | Out-Null
$downloads = Join-Path $Root 'downloads'
New-Item -ItemType Directory -Force $downloads | Out-Null

function Get-Archive([string]$url, [string]$name) {
  $target = Join-Path $downloads $name
  if (-not (Test-Path $target)) {
    Write-Host "Downloading $name..."
    Invoke-WebRequest -Uri $url -OutFile "$target.part"
    Move-Item "$target.part" $target
  }
  return $target
}

# JDK 17
$jdkHome = Join-Path $Root 'jdk17'
if (-not (Test-Path (Join-Path $jdkHome 'bin\java.exe'))) {
  $zip = Get-Archive $jdkUrl 'jdk17.zip'
  $staging = Join-Path $Root 'jdk-staging'
  if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
  Expand-Archive $zip $staging
  $inner = Get-ChildItem $staging -Directory | Select-Object -First 1
  if (Test-Path $jdkHome) { Remove-Item -Recurse -Force $jdkHome }
  Move-Item $inner.FullName $jdkHome
  Remove-Item -Recurse -Force $staging
}

# Gradle
$gradleHome = Join-Path $Root "gradle-$gradleVersion"
if (-not (Test-Path (Join-Path $gradleHome 'bin\gradle.bat'))) {
  $zip = Get-Archive $gradleUrl "gradle-$gradleVersion.zip"
  Expand-Archive $zip $Root -Force
}

# Android SDK command-line tools
$sdk = Join-Path $Root 'android-sdk'
$latest = Join-Path $sdk 'cmdline-tools\latest'
if (-not (Test-Path (Join-Path $latest 'bin\sdkmanager.bat'))) {
  $zip = Get-Archive $toolsUrl 'cmdline-tools.zip'
  $staging = Join-Path $Root 'tools-staging'
  if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
  Expand-Archive $zip $staging
  New-Item -ItemType Directory -Force (Join-Path $sdk 'cmdline-tools') | Out-Null
  if (Test-Path $latest) { Remove-Item -Recurse -Force $latest }
  Move-Item (Join-Path $staging 'cmdline-tools') $latest
  Remove-Item -Recurse -Force $staging
}

$env:JAVA_HOME = $jdkHome
$env:ANDROID_HOME = $sdk
$sdkmanager = Join-Path $latest 'bin\sdkmanager.bat'
Write-Host 'Accepting Android SDK licenses...'
$yes = ('y' + [Environment]::NewLine) * 20
$yes | & $sdkmanager --sdk_root=$sdk --licenses | Out-Null
Write-Host 'Installing Android platform and build tools...'
& $sdkmanager --sdk_root=$sdk 'platforms;android-35' 'build-tools;35.0.0' 'platform-tools' | Out-Null
if ($LASTEXITCODE -ne 0) { throw "sdkmanager failed with exit code $LASTEXITCODE" }

Write-Host "JAVA_HOME=$jdkHome"
Write-Host "ANDROID_HOME=$sdk"
Write-Host "GRADLE_HOME=$gradleHome"
