[CmdletBinding()]
param(
  [string]$BuildDirectory = "build-native",
  [string]$Configuration = "Release",
  [Parameter(Mandatory = $true)]
  [string]$ExpectedPublisher,
  [Parameter(Mandatory = $true)]
  [string[]]$AllowedSignerThumbprints,
  [string]$PortableZipPath = "",
  [string]$InstallerPath = "",
  [string]$MsixPath = "",
  [switch]$SkipSelfTests
)

$ErrorActionPreference = "Stop"
$repository = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repository $BuildDirectory
$configuredBuild = Join-Path $build $Configuration
if (-not (Test-Path -LiteralPath $configuredBuild)) {
  $configuredBuild = $build
}

$normalizedPins = @($AllowedSignerThumbprints |
  ForEach-Object { ($_ -replace '[^0-9A-Fa-f]', '').ToLowerInvariant() } |
  Where-Object { $_ })
if ($normalizedPins.Count -eq 0) {
  throw "At least one allowed SHA-256 signer-certificate thumbprint is required."
}

function Resolve-RequiredFile([string]$Path, [string]$Label) {
  if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
    throw "$Label was not found: $Path"
  }
  return (Resolve-Path -LiteralPath $Path).Path
}

function Assert-AuthenticodeIdentity([string]$Path, [string]$Label) {
  $resolved = Resolve-RequiredFile $Path $Label
  $signature = Get-AuthenticodeSignature -LiteralPath $resolved
  if ($signature.Status -ne 'Valid' -or -not $signature.SignerCertificate) {
    throw "$Label does not have a valid Authenticode signature: $($signature.StatusMessage)"
  }
  if (-not $signature.TimeStamperCertificate) {
    throw "$Label does not have an Authenticode timestamp."
  }

  $publisher = $signature.SignerCertificate.GetNameInfo(
    [Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
  if ($publisher -ne $ExpectedPublisher) {
    throw "$Label publisher '$publisher' does not match '$ExpectedPublisher'."
  }

  $pin = [Convert]::ToHexString(
    [Security.Cryptography.SHA256]::HashData($signature.SignerCertificate.RawData)
  ).ToLowerInvariant()
  if ($pin -notin $normalizedPins) {
    throw "$Label signer certificate is not in the allowed SHA-256 pin set."
  }

  Write-Host "Verified $Label signature, timestamp, publisher, and certificate pin."
  return $resolved
}

function Invoke-SelfTest([string]$Path, [string]$Label) {
  if ($SkipSelfTests) { return }
  $process = Start-Process -FilePath $Path -ArgumentList '--self-test' `
    -PassThru -Wait -WindowStyle Hidden
  if ($process.ExitCode -ne 0) {
    throw "$Label self-test failed with exit code $($process.ExitCode)."
  }
  Write-Host "Passed $Label self-test."
}

function Test-PackageContents(
  [string]$Path,
  [string]$Label,
  [switch]$RunPayloadSelfTests
) {
  $resolved = Resolve-RequiredFile $Path $Label
  $temporaryRoot = Join-Path ([IO.Path]::GetTempPath()) (
    "feathercast-release-qa-" + [Guid]::NewGuid().ToString('N'))
  New-Item -ItemType Directory -Path $temporaryRoot | Out-Null
  try {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($resolved, $temporaryRoot)
    $manifest = Get-ChildItem -LiteralPath $temporaryRoot -Recurse -File |
      Where-Object Name -eq 'AppxManifest.xml' | Select-Object -First 1
    if ($Label -eq 'MSIX package' -and -not $manifest) {
      throw "The MSIX package does not contain AppxManifest.xml."
    }

    foreach ($name in @('FeatherCast.exe', 'FeatherCastPluginHost.exe')) {
      $binary = Get-ChildItem -LiteralPath $temporaryRoot -Recurse -File |
        Where-Object Name -eq $name | Select-Object -First 1
      if (-not $binary) { throw "$Label does not contain $name." }
      $verified = Assert-AuthenticodeIdentity $binary.FullName "$Label $name"
      if ($RunPayloadSelfTests) {
        Invoke-SelfTest $verified "$Label $name"
      }
    }
    Write-Host "Verified $Label contents."
  } finally {
    if (Test-Path -LiteralPath $temporaryRoot) {
      Remove-Item -LiteralPath $temporaryRoot -Recurse -Force
    }
  }
}

$application = Assert-AuthenticodeIdentity `
  (Join-Path $configuredBuild 'FeatherCast.exe') 'FeatherCast.exe'
$pluginHost = Assert-AuthenticodeIdentity `
  (Join-Path $configuredBuild 'FeatherCastPluginHost.exe') 'FeatherCastPluginHost.exe'
Invoke-SelfTest $application 'FeatherCast.exe'
Invoke-SelfTest $pluginHost 'FeatherCastPluginHost.exe'

if ($PortableZipPath) {
  Test-PackageContents $PortableZipPath 'portable ZIP' -RunPayloadSelfTests
}
if ($InstallerPath) {
  Assert-AuthenticodeIdentity $InstallerPath 'NSIS installer' | Out-Null
}
if ($MsixPath) {
  Assert-AuthenticodeIdentity $MsixPath 'MSIX package' | Out-Null
  Test-PackageContents $MsixPath 'MSIX package'
}

Write-Host 'Release artifact verification passed.'
