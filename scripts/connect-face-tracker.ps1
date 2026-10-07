<#
Attaches the VIVE Facial Tracker shared by the "Quest USB/IP" app to this PC
and starts the SRanipal runtime (which is what switches the tracker's camera on).

Usage:  powershell -ExecutionPolicy Bypass -File connect-face-tracker.ps1 [-Quest 192.168.137.16]
#>
param(
    [string]$Quest = '192.168.137.16',
    [string]$UsbIp = "$env:ProgramFiles\USBip\usbip.exe",
    [string]$VidPid = '0bb4:0321',
    [string]$SrRuntime = "$env:ProgramFiles\VIVE\SRanipal\sr_runtime.exe"
)

if (-not (Test-Path $UsbIp)) {
    Write-Host "usbip.exe not found at $UsbIp. Install usbip-win2 or pass -UsbIp <path>." -ForegroundColor Red
    exit 1
}

$list = & $UsbIp list -r $Quest 2>&1 | Out-String
$m = [regex]::Match($list, "(?m)^\s*(\S+)\s*:.*\($VidPid\)")
if (-not $m.Success) {
    Write-Host "Device $VidPid not found on $Quest. Is the Quest USB/IP app open and the tracker plugged in?" -ForegroundColor Red
    Write-Host $list
    exit 1
}

& $UsbIp attach -r $Quest -b $m.Groups[1].Value
Start-Sleep 5
if ((Test-Path $SrRuntime) -and -not (Get-Process sr_runtime -ErrorAction SilentlyContinue)) {
    Start-Process $SrRuntime -WorkingDirectory (Split-Path $SrRuntime)
}
Write-Host "Tracker attached." -ForegroundColor Green
