param(
  [ValidateSet('Install', 'Stop', 'Uninstall', 'Validate')]
  [string]$Mode = 'Install',
  [string]$BrokerPath = (Join-Path $PSScriptRoot 'InputBroker.exe')
)

$ErrorActionPreference = 'Stop'

function Get-InteractiveUserSid {
  # The UAC account can differ from the user who is signed in.
  if (-not ('FeatherCastTaskSession' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class FeatherCastTaskSession {
  [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode)]
  public static extern bool WTSQuerySessionInformation(IntPtr server, int session, int info, out IntPtr buffer, out int bytes);
  [DllImport("wtsapi32.dll")] public static extern void WTSFreeMemory(IntPtr buffer);
  public static string Query(int session, int info) {
    IntPtr buffer; int bytes;
    if (!WTSQuerySessionInformation(IntPtr.Zero, session, info, out buffer, out bytes))
      throw new System.ComponentModel.Win32Exception();
    try { return Marshal.PtrToStringUni(buffer); }
    finally { WTSFreeMemory(buffer); }
  }
}
'@
  }
  $session = [Diagnostics.Process]::GetCurrentProcess().SessionId
  $user = [FeatherCastTaskSession]::Query($session, 5)
  $domain = [FeatherCastTaskSession]::Query($session, 7)
  if (-not $user) { throw 'The input broker requires a signed-in interactive user.' }
  $account = New-Object Security.Principal.NTAccount($domain, $user)
  return $account.Translate([Security.Principal.SecurityIdentifier]).Value
}

function New-BrokerTaskXml([string]$Path, [string]$UserSid) {
  $UserSid = (New-Object Security.Principal.SecurityIdentifier($UserSid)).Value
  $escapedPath = [Security.SecurityElement]::Escape($Path)
  $escapedDirectory = [Security.SecurityElement]::Escape((Split-Path -Parent $Path))
  return @"
<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">
  <RegistrationInfo><Description>FeatherCast Windows-key input broker</Description></RegistrationInfo>
  <Triggers><LogonTrigger><Enabled>true</Enabled><UserId>$UserSid</UserId></LogonTrigger></Triggers>
  <Principals><Principal id="User"><UserId>$UserSid</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals>
  <Settings>
    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>
    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>
    <AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>true</StartWhenAvailable>
    <AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled>
    <ExecutionTimeLimit>PT0S</ExecutionTimeLimit>
  </Settings>
  <Actions Context="User"><Exec><Command>$escapedPath</Command><WorkingDirectory>$escapedDirectory</WorkingDirectory></Exec></Actions>
</Task>
"@
}

function Assert-ProtectedBrokerPath([string]$Path) {
  # Installers may inherit PSModulePath from PowerShell 7. Load this host's
  # built-in security module rather than an incompatible module on that path.
  Import-Module (Join-Path $PSHOME 'Modules\Microsoft.PowerShell.Security\Microsoft.PowerShell.Security.psd1') -ErrorAction Stop
  $fullPath = [IO.Path]::GetFullPath($Path)
  $programRoots = @($env:ProgramW6432, $env:ProgramFiles, ${env:ProgramFiles(x86)}) | Where-Object { $_ }
  $protectedRoot = $programRoots | Where-Object {
    $fullPath.StartsWith(([IO.Path]::GetFullPath($_).TrimEnd('\') + '\'), [StringComparison]::OrdinalIgnoreCase)
  } | Select-Object -First 1
  if (-not $protectedRoot) {
    throw 'The elevated input broker must be installed inside Program Files.'
  }
  $trustedOwners = @('S-1-5-18', 'S-1-5-32-544', 'S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464')
  # GENERIC_ALL (0x10000000) and GENERIC_WRITE (0x40000000) are not named
  # FileSystemRights values but still grant write access when set on an ACE.
  $writeRights = [long]([Security.AccessControl.FileSystemRights]::Write -bor
    [Security.AccessControl.FileSystemRights]::Delete -bor
    [Security.AccessControl.FileSystemRights]::DeleteSubdirectoriesAndFiles -bor
    [Security.AccessControl.FileSystemRights]::ChangePermissions -bor
    [Security.AccessControl.FileSystemRights]::TakeOwnership) -bor 0x10000000 -bor 0x40000000
  $entry = Get-Item -LiteralPath $fullPath
  while ($entry) {
    if ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'The broker path cannot contain links.' }
    $acl = Get-Acl -LiteralPath $entry.FullName
    if ($acl.GetOwner([Security.Principal.SecurityIdentifier]).Value -notin $trustedOwners) {
      throw 'The broker path must be owned by Administrators or SYSTEM.'
    }
    foreach ($rule in $acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])) {
      if ($rule.AccessControlType -eq 'Allow' -and
          -not ($rule.PropagationFlags -band [Security.AccessControl.PropagationFlags]::InheritOnly) -and
          $rule.IdentityReference.Value -notin $trustedOwners -and ([long]$rule.FileSystemRights -band $writeRights)) {
        throw 'The broker path must not be writable without administrator rights.'
      }
    }
    if ([string]::Equals($entry.FullName.TrimEnd('\'), $protectedRoot.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) { break }
    $entry = if ($entry -is [IO.FileInfo]) { $entry.Directory } else { $entry.Parent }
  }
}

function Invoke-BrokerTaskSetup {
  $path = [IO.Path]::GetFullPath($BrokerPath)
  $scheduler = New-Object -ComObject 'Schedule.Service'
  $scheduler.Connect()
  $root = $scheduler.GetFolder('\')
  if ($Mode -eq 'Install' -or $Mode -eq 'Validate') {
    # Only registration needs the signed-in user. Stop/Uninstall also have to
    # work without an interactive session, e.g. a silent uninstall as SYSTEM.
    $userSid = Get-InteractiveUserSid
    $taskName = "FeatherCast Input Broker-$userSid"
    $xml = New-BrokerTaskXml $path $userSid
    if ($Mode -eq 'Validate') {
      $root.RegisterTask($taskName, $xml, 1, $null, $null, 3, $null) | Out-Null
      return
    }
    Assert-ProtectedBrokerPath $path
    # Users can read/run the task. Only administrators/SYSTEM can change its action.
    $security = "D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;$userSid)"
    $root.RegisterTask($taskName, $xml, 22, $userSid, $null, 3, $security) | Out-Null
    return
  }
  # An uninstall/update can be performed by another administrator. Clean up
  # every per-user task that points at this exact installation.
  foreach ($task in @($root.GetTasks(0))) {
    if (-not $task.Name.StartsWith('FeatherCast Input Broker-')) { continue }
    $taskXml = [xml]$task.Xml
    if (-not [string]::Equals($taskXml.Task.Actions.Exec.Command, $path, [StringComparison]::OrdinalIgnoreCase)) {
      continue
    }
    $task.Stop(0)
    for ($attempt = 0; $attempt -lt 40 -and $task.State -eq 4; $attempt++) {
      Start-Sleep -Milliseconds 100
    }
    if ($task.State -eq 4) { throw 'The input broker did not stop.' }
    if ($Mode -eq 'Uninstall') { $root.DeleteTask($task.Name, 0) }
  }
}

# Tests dot-source the helpers without creating or changing tasks.
if ($MyInvocation.InvocationName -ne '.') {
  try { Invoke-BrokerTaskSetup }
  catch { Write-Error $_; exit 1 }
}
