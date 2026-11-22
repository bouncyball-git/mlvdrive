# Installs MLV Drive for the current user (no administrator rights), from the folder this script
# is in (a release zip unpacked, or the staging folder `build.ps1 install` makes):
#
#   install.ps1               the programs into %LOCALAPPDATA%\Programs\MLV Drive, a
#                             Start Menu shortcut "MLV Drive" (the tray app); a running copy is asked
#                             to quit first
#   install.ps1 -Uninstall    removes them, the shortcut and "Start with Windows" (if it starts this
#                             copy); keeps the settings (%APPDATA%\MLV Drive)
#   install.ps1 -Start        starts MLV Drive once installed
#
# install.cmd / uninstall.cmd run it with PowerShell's script policy set aside for this one run.
# WinFsp (https://winfsp.dev/rel/) is installed separately: MLV Drive needs it for the drive.
[CmdletBinding()]
param([switch] $Uninstall, [switch] $Start)
$ErrorActionPreference = 'Stop'
$From = $PSScriptRoot
$InstallDir = Join-Path $env:LOCALAPPDATA 'Programs\MLV Drive'
$Shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'MLV Drive.lnk'
$RunKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'

function Step([string] $text) { Write-Host "==> $text" -ForegroundColor Cyan }

# The tray app is asked to quit (its menu's Quit: it unmounts and takes its icon away); anything
# from the install folder still running after 5 s is ended (Windows then unmounts its drive).
function Stop-Installed {
    $running = @(Get-Process mlvdrive_tray, mlvdrive -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($InstallDir, [StringComparison]::OrdinalIgnoreCase) })
    if (-not $running) { return }
    Add-Type -Namespace MlvDriveInstall -Name Win -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr wnd, uint msg, IntPtr w, IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr wnd, out uint pid);
'@
    # The tray app runs once per user: its window, if that is this installation's.
    $tray = [MlvDriveInstall.Win]::FindWindowW('MLVDriveTray', [NullString]::Value)
    $trayPid = [uint32]0
    if ($tray -ne [IntPtr]::Zero) { [MlvDriveInstall.Win]::GetWindowThreadProcessId($tray, [ref] $trayPid) | Out-Null }
    if ($trayPid -and ($running.Id -contains [int]$trayPid)) {
        [MlvDriveInstall.Win]::PostMessageW($tray, 0x0111, [IntPtr]116, [IntPtr]::Zero) | Out-Null  # WM_COMMAND Quit
    }
    foreach ($p in $running) {
        Step "stop $($p.Name) (pid $($p.Id))"
        if (-not $p.WaitForExit(5000)) { Stop-Process -Id $p.Id -Force; $p.WaitForExit(5000) | Out-Null }
    }
}

if ($Uninstall) {
    Stop-Installed
    if (Test-Path $InstallDir) { Step "remove $InstallDir"; Remove-Item -Recurse -Force $InstallDir }
    if (Test-Path $Shortcut) { Step "remove $Shortcut"; Remove-Item -Force $Shortcut }
    # "Start with Windows" only if it starts this installation (not one the setup installed).
    $run = (Get-ItemProperty $RunKey -Name 'MLV Drive' -ErrorAction SilentlyContinue).'MLV Drive'
    if ($run -and $run.Trim('"').StartsWith($InstallDir, [StringComparison]::OrdinalIgnoreCase)) {
        Step 'remove "Start with Windows"'
        Remove-ItemProperty $RunKey -Name 'MLV Drive'
    }
    Write-Host "Uninstalled. The settings are kept in $env:APPDATA\MLV Drive."
    return
}

$programs = 'mlvdrive_tray.exe', 'mlvdrive.exe', 'mlvdrive_tool.exe'
foreach ($p in $programs) { if (-not (Test-Path (Join-Path $From $p))) { throw "$p is missing from $From" } }
Stop-Installed
Step "install: $InstallDir"
New-Item -ItemType Directory -Force $InstallDir | Out-Null
# Programs since renamed (mlvdrive_cli.exe is mlvdrive_tool.exe since 0.2.0), and the pixel maps
# 0.7 to 0.9 installed here (no longer bundled).
foreach ($old in 'mlvdrive_cli.exe', 'pixelmaps') {
    Remove-Item (Join-Path $InstallDir $old) -Recurse -Force -ErrorAction SilentlyContinue
}
$items = $programs + 'README.md', 'LICENSE', 'install.ps1', 'install.cmd', 'uninstall.cmd' |
    Where-Object { Test-Path (Join-Path $From $_) }
foreach ($i in $items) { Copy-Item (Join-Path $From $i) $InstallDir -Recurse -Force }
# Files unpacked from a downloaded zip carry its "from the internet" mark: the copies installed
# here are the ones meant to run.
Get-ChildItem $InstallDir -Recurse -File | Unblock-File

$shell = New-Object -ComObject WScript.Shell
$link = $shell.CreateShortcut($Shortcut)
$link.TargetPath = Join-Path $InstallDir 'mlvdrive_tray.exe'
$link.WorkingDirectory = $InstallDir
$link.Description = 'MLV Drive: MLV clips as a drive of CinemaDNG sequences'
$link.Save()
Step "Start Menu: $Shortcut"

if (-not (Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\WinFsp' -Name InstallDir -ErrorAction SilentlyContinue)) {
    Write-Warning 'WinFsp is not installed: MLV Drive needs it for the drive (https://winfsp.dev/rel/).'
}
if ($Start) {
    Start-Process (Join-Path $InstallDir 'mlvdrive_tray.exe')
    Write-Host 'Installed and started: the MLV Drive icon is in the notification area.'
} else {
    Write-Host 'Installed. Start "MLV Drive" from the Start Menu: its icon is in the notification area.'
}
