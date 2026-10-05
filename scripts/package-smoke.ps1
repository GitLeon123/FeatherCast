param(
  [string]$BuildDirectory = "build-native",
  [string]$Configuration = "Release",
  [switch]$SkipInstalledSmoke,
  [string]$InstallRoot = ""
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 3.0

# Start-Process -Wait can hang forever and a GUI exe started with & does not
# wait at all, so every child process gets an explicit wait, timeout and
# exit-code check.
function Invoke-CheckedProcess {
  param(
    [string]$FilePath,
    [string[]]$Arguments,
    [string]$Label,
    [int]$TimeoutSeconds = 300
  )
  $process = Start-Process -FilePath $FilePath -ArgumentList $Arguments -WindowStyle Hidden -PassThru
  # Holding the handle keeps ExitCode readable after the process has exited.
  $null = $process.Handle
  if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
    try { $process.Kill() } catch { }
    throw "$Label did not finish within $TimeoutSeconds seconds."
  }
  if ($process.ExitCode -ne 0) { throw "$Label failed with exit code $($process.ExitCode)." }
}

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
    Invoke-CheckedProcess $binary.FullName @("--self-test") "$name ZIP self-test" 120
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
  Invoke-CheckedProcess $installer.FullName $installArguments "NSIS install"
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
    $binary = Join-Path $InstallRoot "bin\$name"
    Invoke-CheckedProcess $binary @("--self-test") "$name installed self-test" 120
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

  Invoke-CheckedProcess $installer.FullName $installArguments "Repeated NSIS install"
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

  # A plain NSIS uninstaller copies itself to %TEMP% and exits at once. Run a
  # copy with _?= (last argument, unquoted) so it works in place and its exit
  # code is the real result.
  $uninstallCopy = Join-Path $temporaryRoot (
    "feathercast-package-smoke-uninstall-" + [Guid]::NewGuid().ToString("N"))
  New-Item -ItemType Directory -Force -Path $uninstallCopy | Out-Null
  try {
    $uninstallerCopy = Join-Path $uninstallCopy "Uninstall.exe"
    Copy-Item -LiteralPath $uninstaller -Destination $uninstallerCopy
    Invoke-CheckedProcess $uninstallerCopy @("/S", ("_?=" + $InstallRoot)) "NSIS uninstall"
  } finally {
    Remove-Item -LiteralPath $uninstallCopy -Recurse -Force -ErrorAction SilentlyContinue
  }
  for ($attempt = 0; $attempt -lt 20 -and (Test-Path $InstallRoot); $attempt++) {
    Start-Sleep -Milliseconds 250
  }
  if (Get-ScheduledTask -TaskName $brokerTaskName -ErrorAction SilentlyContinue) {
    throw 'Uninstall left the elevated input broker task behind.'
  }
  if (Test-Path $InstallRoot) {
    throw "NSIS uninstall left the installation directory behind."
  }
} finally {
  Pop-Location
}
