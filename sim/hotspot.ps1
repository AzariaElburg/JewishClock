# Windows Mobile Hotspot, switched by sim/hotspot.py (2026-11-13).
#   powershell -File sim/hotspot.ps1 status
#   powershell -File sim/hotspot.ps1 config            -> current name and password (to restore later)
#   powershell -File sim/hotspot.ps1 start NAME PASSWORD
#   powershell -File sim/hotspot.ps1 configure NAME PASSWORD   (stays off)
#   powershell -File sim/hotspot.ps1 stop
# Prints one line: JSON. No administrator rights needed (Windows' own Settings >
# Mobile hotspot does the same). The laptop stays on its home Wi-Fi meanwhile.
param([string]$cmd = "status", [string]$ssid = "", [string]$pass = "")
$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Runtime.WindowsRuntime
$null = [Windows.Networking.Connectivity.NetworkInformation, Windows.Networking.Connectivity, ContentType = WindowsRuntime]
$null = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager, Windows.Networking.NetworkOperators, ContentType = WindowsRuntime]

$asTaskOp = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1' })[0]
$asTaskAction = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
    $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncAction' })[0]
function Await($op, [Type]$type) {
    $t = $asTaskOp.MakeGenericMethod($type).Invoke($null, @($op)); $null = $t.Wait(60000); return $t.Result
}
function AwaitAction($op) { $t = $asTaskAction.Invoke($null, @($op)); $null = $t.Wait(60000) }

function Out($h) { Write-Output ($h | ConvertTo-Json -Compress) }

try {
    $netProfile = [Windows.Networking.Connectivity.NetworkInformation]::GetInternetConnectionProfile()
    if ($null -eq $netProfile) { Out @{ ok = $false; error = "this laptop has no internet connection to share" }; exit 1 }
    $m = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager]::CreateFromConnectionProfile($netProfile)
    $resultType = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringOperationResult]
    switch ($cmd) {
        "status" {
            $c = $m.GetCurrentAccessPointConfiguration()
            Out @{ ok = $true; on = ($m.TetheringOperationalState.ToString() -eq "On"); ssid = $c.Ssid; clients = $m.ClientCount }
        }
        "config" {
            $c = $m.GetCurrentAccessPointConfiguration()
            Out @{ ok = $true; ssid = $c.Ssid; pass = $c.Passphrase }
        }
        "start" {
            $c = $m.GetCurrentAccessPointConfiguration()
            if ($c.Ssid -ne $ssid -or $c.Passphrase -ne $pass) {
                if ($m.TetheringOperationalState.ToString() -eq "On") { $null = Await ($m.StopTetheringAsync()) $resultType }
                $c.Ssid = $ssid; $c.Passphrase = $pass
                AwaitAction ($m.ConfigureAccessPointAsync($c))
            }
            if ($m.TetheringOperationalState.ToString() -ne "On") {
                $r = Await ($m.StartTetheringAsync()) $resultType
                if ($r.Status.ToString() -ne "Success") { Out @{ ok = $false; error = ($r.Status.ToString() + " " + $r.AdditionalErrorMessage) }; exit 1 }
            }
            Out @{ ok = $true; on = $true; ssid = $ssid }
        }
        "configure" {   # name and password only, stays off (putting the laptop's own back)
            if ($m.TetheringOperationalState.ToString() -eq "On") { $null = Await ($m.StopTetheringAsync()) $resultType }
            $c = $m.GetCurrentAccessPointConfiguration()
            $c.Ssid = $ssid; $c.Passphrase = $pass
            AwaitAction ($m.ConfigureAccessPointAsync($c))
            Out @{ ok = $true; on = $false; ssid = $ssid }
        }
        "stop" {
            if ($m.TetheringOperationalState.ToString() -eq "On") { $null = Await ($m.StopTetheringAsync()) $resultType }
            Out @{ ok = $true; on = $false }
        }
        default { Out @{ ok = $false; error = "unknown command $cmd" }; exit 1 }
    }
} catch {
    Out @{ ok = $false; error = $_.Exception.Message }; exit 1
}
