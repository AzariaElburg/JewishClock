# One-time setup for sim/hotspot.py (2026-11-13). Needs administrator rights:
# run it normally, Windows asks for permission (UAC) once.
#
#   powershell -ExecutionPolicy Bypass -File sim\hotspot_admin_setup.ps1            # address + firewall
#   powershell -ExecutionPolicy Bypass -File sim\hotspot_admin_setup.ps1 -StopIIS   # ... and IIS off (plain addresses, no port)
#   powershell -ExecutionPolicy Bypass -File sim\hotspot_admin_setup.ps1 -Undo      # everything back as it was
#
# What it changes:
# 1. The Mobile Hotspot's network becomes 192.168.4.x (the laptop 192.168.4.1) - the
#    address the real clock's own Wi-Fi has - instead of Windows' 192.168.137.x. Only
#    the hotspot's own network; the laptop's home Wi-Fi is untouched.
# 2. Firewall: phones on the hotspot may reach the simulated clock's page (TCP 80, 8071,
#    8081 on the hotspot's address only - nothing on the home network).
# 3. -StopIIS: Windows' own web server (IIS, "World Wide Web Publishing Service") is
#    stopped and set to Manual start. It holds port 80 and on this laptop never got a
#    request. Undo starts it again.
param([switch]$StopIIS, [switch]$Undo)

$me = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $me.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $a = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-NoExit", "-File", "`"$PSCommandPath`"")
    if ($StopIIS) { $a += "-StopIIS" }
    if ($Undo) { $a += "-Undo" }
    Start-Process powershell -Verb RunAs -ArgumentList $a
    exit
}

$params = "HKLM:\SYSTEM\CurrentControlSet\Services\SharedAccess\Parameters"
$rule = "JewishClock simulator hotspot"

if ($Undo) {
    Remove-ItemProperty -Path $params -Name ScopeAddress, ScopeAddressBackup, StandaloneDhcpAddress -ErrorAction SilentlyContinue
    Get-NetFirewallRule -DisplayName $rule -ErrorAction SilentlyContinue | Remove-NetFirewallRule
    foreach ($s in "WAS", "W3SVC") {
        if (Get-Service $s -ErrorAction SilentlyContinue) { Set-Service $s -StartupType Automatic; Start-Service $s -ErrorAction SilentlyContinue }
    }
    Restart-Service SharedAccess -Force -ErrorAction SilentlyContinue
    Write-Host "Undone: hotspot address back to Windows' default, firewall rule removed, IIS started."
    exit
}

$ErrorActionPreference = "Stop"   # a step that fails ends here, never "Done" after it
trap { Write-Host "FAILED: $_" -ForegroundColor Red; exit 1 }

# 1. The hotspot's address (the key exists on every Windows with internet sharing; never
#    recreate it - New-Item -Force would replace it with its values and subkeys)
if (-not (Test-Path $params)) { New-Item -Path $params | Out-Null }
Set-ItemProperty -Path $params -Name ScopeAddress -Value "192.168.4.1" -Type String
Set-ItemProperty -Path $params -Name ScopeAddressBackup -Value "192.168.4.1" -Type String
Set-ItemProperty -Path $params -Name StandaloneDhcpAddress -Value "192.168.4.1" -Type String   # read by Mobile Hotspot on some Windows versions
foreach ($s in "SharedAccess", "icssvc") {   # internet sharing; Windows Mobile Hotspot Service
    if ((Get-Service $s -ErrorAction SilentlyContinue).Status -eq "Running") { Restart-Service $s -Force }
}
Write-Host "1. Hotspot network: 192.168.4.x asked for (this laptop 192.168.4.1 on it). Windows 11 may keep"
Write-Host "   192.168.137.x anyway (until a restart, or for good) - the page works on either."

# 2. Firewall
Get-NetFirewallRule -DisplayName $rule -ErrorAction SilentlyContinue | Remove-NetFirewallRule
New-NetFirewallRule -DisplayName $rule -Direction Inbound -Action Allow -Protocol TCP -LocalPort 80, 8071, 8081 `
    -LocalAddress 192.168.4.1, 192.168.137.1 -Profile Any | Out-Null
Write-Host "2. Firewall: phones on the hotspot may open the simulated clock's page."

# 3. IIS
if ($StopIIS) {
    foreach ($s in "W3SVC", "WAS") {
        if (Get-Service $s -ErrorAction SilentlyContinue) { Set-Service $s -StartupType Manual; Stop-Service $s -Force }
    }
    Write-Host "3. IIS stopped (Manual start): port 80 is free - phones open the address without a port (the hotspot window shows it)"
} else {
    Write-Host "3. IIS left as it is: phones open http://192.168.4.1:8071 (family) or :8081 (work)."
}
Write-Host "Done. This window can be closed."
