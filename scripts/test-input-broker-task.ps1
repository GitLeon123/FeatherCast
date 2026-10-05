$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
. (Join-Path $PSScriptRoot 'input-broker-task.ps1')

function Assert-Equal($Actual, $Expected, [string]$Message) {
  if ($Actual -ne $Expected) { throw $Message }
}

$sid = [Security.Principal.WindowsIdentity]::GetCurrent().User.Value
$broker = 'C:\Program Files\FeatherCast & Test\bin\InputBroker.exe'
$xml = [xml](New-BrokerTaskXml $broker $sid)
Assert-Equal $xml.Task.Principals.Principal.RunLevel 'HighestAvailable' 'The broker must run elevated.'
Assert-Equal $xml.Task.Principals.Principal.LogonType 'InteractiveToken' 'The task must not store a password.'
Assert-Equal $xml.Task.Triggers.LogonTrigger.UserId $sid 'The logon trigger must belong to the interactive user.'
Assert-Equal $xml.Task.Actions.Exec.Command $broker 'Paths containing spaces and XML characters must survive.'
Assert-Equal $xml.Task.Settings.AllowStartOnDemand 'true' 'Launcher restarts must be able to run the task.'
Assert-Equal $xml.Task.Settings.ExecutionTimeLimit 'PT0S' 'The hook must not time out after three days.'
Assert-Equal $xml.Task.Settings.DisallowStartIfOnBatteries 'false' 'The broker must work on battery power.'
Assert-Equal $xml.Task.Settings.StopIfGoingOnBatteries 'false' 'Unplugging must not stop the broker.'
Assert-Equal $xml.Task.Settings.MultipleInstancesPolicy 'IgnoreNew' 'Repeated starts must not create duplicate brokers.'

$rejected = $false
try { Assert-ProtectedBrokerPath (Join-Path ([IO.Path]::GetTempPath()) 'InputBroker.exe') }
catch { $rejected = $true }
if (-not $rejected) { throw 'Elevated tasks must not point at user-writable portable binaries.' }

# TASK_VALIDATE_ONLY asks Windows to validate the actual task schema without
# registering a task, changing startup settings, or requiring elevation.
$scheduler = New-Object -ComObject 'Schedule.Service'
$scheduler.Connect()
$root = $scheduler.GetFolder('\')
$root.RegisterTask("FeatherCast Input Broker-Validation-$sid", $xml.OuterXml, 1, $null, $null, 3, $null) | Out-Null
Write-Host 'Input broker task tests passed.'
