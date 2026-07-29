param(
  [string]$BuildDirectory = "build-native",
  [string]$Configuration = "Release",
  [switch]$SkipInstalledSmoke,
  [string]$InstallRoot = ""
)

$ErrorActionPreference = "Stop"
$repository = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repository $BuildDirectory
$cpackConfig = Join-Path $build "CPackConfig.cmake"
if (-not (Test-Path $cpackConfig)) { throw "CPackConfig.cmake was not found." }

Push-Location $repository
try {
  & cpack --config $cpackConfig -C $Configuration -G ZIP
  if ($LASTEXITCODE -ne 0) { throw "ZIP packaging failed." }
  & cpack --config $cpackConfig -C $Configuration -G NSIS
  if ($LASTEXITCODE -ne 0) { throw "NSIS packaging failed." }

  $zip = Get-ChildItem "FeatherCast-*-win64.zip" |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
  $installer = Get-ChildItem "FeatherCast-*-win64.exe" |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
  if (-not $zip -or -not $installer) { throw "Expected ZIP and NSIS packages." }

  $temporaryRoot = if ($env:RUNNER_TEMP) {
    $env:RUNNER_TEMP
  } else {
    [IO.Path]::GetTempPath()
  }
  $portable = Join-Path $temporaryRoot (
    "feathercast-package-smoke-portable-" + [Guid]::NewGuid().ToString("N"))
  Expand-Archive $zip.FullName $portable -Force
  foreach ($name in @("FeatherCast.exe", "FeatherCastPluginHost.exe")) {
    $binary = Get-ChildItem $portable -Recurse -Filter $name | Select-Object -First 1
    if (-not $binary) { throw "$name is missing from the ZIP package." }
    $process = Start-Process $binary.FullName -ArgumentList "--self-test" -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "$name ZIP self-test failed." }
  }

  if ($SkipInstalledSmoke) { return }

  if (-not $InstallRoot) {
    $InstallRoot = Join-Path $temporaryRoot (
      "feathercast-package-smoke-install-" + [Guid]::NewGuid().ToString("N"))
  }
  $installArguments = @("/S", "/D=$InstallRoot")
  $install = Start-Process $installer.FullName -ArgumentList $installArguments -PassThru -Wait
  if ($install.ExitCode -ne 0) { throw "NSIS install failed." }
  $uninstaller = Join-Path $InstallRoot "Uninstall.exe"
  if (-not (Test-Path $uninstaller)) { throw "NSIS uninstall artifact is missing." }
  $shortcutCandidates = @(
    (Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\FeatherCast.lnk"),
    (Join-Path $env:ProgramData "Microsoft\Windows\Start Menu\Programs\FeatherCast\FeatherCast.lnk"),
    (Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\FeatherCast.lnk"),
    (Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs\FeatherCast\FeatherCast.lnk")
  )
  if (-not ($shortcutCandidates | Where-Object { Test-Path $_ })) {
    throw "The FeatherCast Start menu shortcut is missing."
  }
  foreach ($name in @("FeatherCast.exe", "FeatherCastPluginHost.exe")) {
    $binary = Join-Path $installRoot "bin\$name"
    $process = Start-Process $binary -ArgumentList "--self-test" -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "$name installed self-test failed." }
  }
  & $uninstaller /S
  $uninstallExitCode = $LASTEXITCODE
  for ($attempt = 0; $attempt -lt 20 -and (Test-Path $InstallRoot); $attempt++) {
    Start-Sleep -Milliseconds 250
  }
  if ($uninstallExitCode -ne 0 -or (Test-Path $InstallRoot)) {
    throw "NSIS uninstall smoke failed."
  }
} finally {
  Pop-Location
}
