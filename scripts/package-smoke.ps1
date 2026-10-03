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
$packageDirectory = Join-Path $build "packages"
if (-not (Test-Path $cpackConfig)) { throw "CPackConfig.cmake was not found." }
New-Item -ItemType Directory -Force -Path $packageDirectory | Out-Null

Push-Location $repository
try {
  & cpack --config $cpackConfig -C $Configuration -G ZIP -B $packageDirectory
  if ($LASTEXITCODE -ne 0) { throw "ZIP packaging failed." }
  & cpack --config $cpackConfig -C $Configuration -G NSIS -B $packageDirectory
  if ($LASTEXITCODE -ne 0) { throw "NSIS packaging failed." }

  $zip = Get-ChildItem -LiteralPath $packageDirectory -Filter "FeatherCast-*-win64.zip" -File |
    Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
  $installer = Get-ChildItem -LiteralPath $packageDirectory -Filter "FeatherCast-*-win64.exe" -File |
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
  foreach ($name in @("FeatherCast.exe", "FeatherCastPluginHost.exe", "InputBroker.exe")) {
    $binary = Get-ChildItem $portable -Recurse -Filter $name | Select-Object -First 1
    if (-not $binary) { throw "$name is missing from the ZIP package." }
    $process = Start-Process $binary.FullName -WindowStyle Hidden -ArgumentList "--self-test" -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "$name ZIP self-test failed." }
  }

  $phoneApp = Join-Path $build 'FeatherCast-Phone.apk'
  if (Test-Path -LiteralPath $phoneApp) {
    $packagedPhone = Get-ChildItem $portable -Recurse -Filter 'FeatherCast-Phone.apk' | Select-Object -First 1
    if (-not $packagedPhone -or
        (Get-FileHash $packagedPhone.FullName).Hash -ne (Get-FileHash $phoneApp).Hash) {
      throw 'The portable ZIP does not contain the built companion APK.'
    }
  }

  if ($SkipInstalledSmoke) { return }

  if (-not $InstallRoot) {
    $InstallRoot = Join-Path $env:ProgramFiles (
      "feathercast-package-smoke-install-" + [Guid]::NewGuid().ToString("N"))
  }
  $userSid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
  $brokerTaskName = "FeatherCast Input Broker-$userSid"
  if (Get-ScheduledTask -TaskName $brokerTaskName -ErrorAction SilentlyContinue) {
    throw 'Installed package smoke must run on a machine without an existing FeatherCast broker task.'
  }
  # NSIS requires /D= to be the final argument and does not accept quotes.
  $installArguments = @("/S", ("/D=" + $InstallRoot))
  $install = Start-Process $installer.FullName -WindowStyle Hidden -ArgumentList $installArguments -PassThru -Wait
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
  foreach ($name in @("FeatherCast.exe", "FeatherCastPluginHost.exe", "InputBroker.exe")) {
    $binary = Join-Path $installRoot "bin\$name"
    $process = Start-Process $binary -WindowStyle Hidden -ArgumentList "--self-test" -PassThru -Wait
    if ($process.ExitCode -ne 0) { throw "$name installed self-test failed." }
  }
  if (Test-Path -LiteralPath $phoneApp) {
    $installedPhone = Join-Path $installRoot 'bin\FeatherCast-Phone.apk'
    if (-not (Test-Path -LiteralPath $installedPhone) -or
        (Get-FileHash $installedPhone).Hash -ne (Get-FileHash $phoneApp).Hash) {
      throw 'The installer does not contain the built companion APK.'
    }
  }

  $task = Get-ScheduledTask -TaskName $brokerTaskName -ErrorAction Stop
  if ($task.Principal.RunLevel -ne 'Highest' -or $task.Principal.LogonType -ne 'Interactive') {
    throw 'The installed input broker task is not elevated and interactive.'
  }
  if ($task.Actions.Execute -ne (Join-Path $InstallRoot 'bin\InputBroker.exe')) {
    throw 'The input broker task does not point at the installed executable.'
  }

  $repeatInstall = Start-Process $installer.FullName -WindowStyle Hidden -ArgumentList $installArguments -PassThru -Wait
  if ($repeatInstall.ExitCode -ne 0) { throw "Repeated NSIS install failed." }
  $uninstallRoots = @(
    "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall",
    "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall",
    "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall"
  )
  $identities = @($uninstallRoots | ForEach-Object {
    Get-ChildItem $_ -ErrorAction SilentlyContinue |
      Where-Object PSChildName -eq "FeatherCast"
  })
  if ($identities.Count -ne 1) {
    throw "Expected one FeatherCast uninstall identity after repeat install; found $($identities.Count)."
  }
  $identity = Get-ItemProperty $identities[0].PSPath
  $uninstallString = [string]$identity.UninstallString
  if (-not $uninstallString -or
      $uninstallString.IndexOf($InstallRoot, [StringComparison]::OrdinalIgnoreCase) -lt 0) {
    throw "The uninstall entry does not point at the custom install root."
  }

  & $uninstaller /S
  $uninstallExitCode = $LASTEXITCODE
  for ($attempt = 0; $attempt -lt 20 -and (Test-Path $InstallRoot); $attempt++) {
    Start-Sleep -Milliseconds 250
  }
  if (Get-ScheduledTask -TaskName $brokerTaskName -ErrorAction SilentlyContinue) {
    throw 'Uninstall left the elevated input broker task behind.'
  }
  if ($uninstallExitCode -ne 0 -or (Test-Path $InstallRoot)) {
    throw "NSIS uninstall smoke failed."
  }
} finally {
  Pop-Location
}
