# Builds MLV Drive on Windows (Visual Studio 2022 or its Build Tools, with CMake and Ninja).
#
#   .\build.ps1 [clean] [build] [install] [uninstall] [-Config Release|Debug|RelWithDebInfo]
#
#   clean      deletes the build folder
#   build      configures (if needed) and builds (the default action):
#                build\mlvdrive_tray.exe  the tray app (MLV Drive in the notification area)
#                build\mlvdrive.exe       the drive on the command line
#                build\mlvdrive_cli.exe   the core on the command line (clip info, DNGs, timings)
#   install    for the current user, no administrator rights: the programs into
#              %LOCALAPPDATA%\Programs\MLV Drive, a Start Menu shortcut "MLV Drive" (the tray app);
#              stops a running copy first. WinFsp must be installed separately (https://winfsp.dev/rel/)
#   uninstall  removes them, the shortcut and "Start with Windows"; keeps the settings
#              (%APPDATA%\MLV Drive)
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)] [string[]] $Actions,
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')] [string] $Config = 'Release'
)
$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$BuildDir = Join-Path $Root 'build'
if (-not $Actions) { $Actions = @('build') }

$InstallDir = Join-Path $env:LOCALAPPDATA 'Programs\MLV Drive'
$Shortcut = Join-Path ([Environment]::GetFolderPath('Programs')) 'MLV Drive.lnk'

function Step([string] $text) { Write-Host "==> $text" -ForegroundColor Cyan }

# Stops MLV Drive running from the install folder: the tray app is asked to quit (its menu's Quit:
# it unmounts and takes its icon away), anything still running after 5 s is ended (Windows then
# unmounts its drive).
function Stop-Installed {
    $running = @(Get-Process mlvdrive_tray, mlvdrive -ErrorAction SilentlyContinue |
        Where-Object { $_.Path -and $_.Path.StartsWith($InstallDir, [StringComparison]::OrdinalIgnoreCase) })
    if (-not $running) { return }
    Add-Type -Namespace MlvDrive -Name Win -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr FindWindowW(string cls, string title);
[DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr wnd, uint msg, IntPtr w, IntPtr l);
'@
    $tray = [MlvDrive.Win]::FindWindowW('MLVDriveTray', [NullString]::Value)
    if ($tray -ne [IntPtr]::Zero) { [MlvDrive.Win]::PostMessageW($tray, 0x0111, [IntPtr]116, [IntPtr]::Zero) | Out-Null }  # WM_COMMAND Quit
    foreach ($p in $running) {
        Step "stop $($p.Name) (pid $($p.Id))"
        if (-not $p.WaitForExit(5000)) { Stop-Process -Id $p.Id -Force; $p.WaitForExit(5000) | Out-Null }
    }
}
function Run {
    & $args[0] @($args | Select-Object -Skip 1)
    if ($LASTEXITCODE -ne 0) { throw "failed (exit code $LASTEXITCODE): $($args -join ' ')" }
}

# The Visual Studio C++ environment (compiler, Windows SDK, CMake, Ninja) in this session.
function Use-VisualStudio {
    if ((Get-Command cl.exe -ErrorAction SilentlyContinue) -and (Get-Command cmake -ErrorAction SilentlyContinue) -and
        (Get-Command ninja -ErrorAction SilentlyContinue)) { return }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw 'Visual Studio 2022 (or Build Tools) not found: vswhere.exe is missing.' }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'No Visual Studio with the C++ tools found ("Desktop development with C++").' }
    $arch = if ($env:PROCESSOR_ARCHITECTURE -eq 'ARM64') { 'arm64' } else { '64' }
    $vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars$arch.bat"
    Step "Visual Studio environment ($vs)"
    $lines = cmd /c "`"$vcvars`" >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) { throw "vcvars failed: $vcvars" }
    foreach ($line in $lines) {
        $eq = $line.IndexOf('=')
        if ($eq -gt 0) { Set-Item -Path "env:$($line.Substring(0, $eq))" -Value $line.Substring($eq + 1) }
    }
}

foreach ($action in $Actions) {
    switch ($action) {
        'clean' {
            if (Test-Path $BuildDir) { Step "clean: $BuildDir"; Remove-Item -Recurse -Force $BuildDir }
        }
        'build' {
            # A build folder configured for another source folder (the project moved or was renamed)
            # makes CMake refuse to run: start it again.
            $cache = Join-Path $BuildDir 'CMakeCache.txt'
            if (Test-Path $cache) {
                $home_ = (Select-String -Path $cache -Pattern '^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$').Matches |
                    ForEach-Object { $_.Groups[1].Value }
                if ($home_ -and ([IO.Path]::GetFullPath($home_).TrimEnd('\') -ne [IO.Path]::GetFullPath($Root).TrimEnd('\'))) {
                    Step "build folder was configured for $home_ : starting it again"
                    Remove-Item -Recurse -Force $BuildDir
                }
            }
            Use-VisualStudio
            Step "configure ($Config) in $BuildDir"
            Run cmake -S $Root -B $BuildDir -G Ninja "-DCMAKE_BUILD_TYPE=$Config"
            Step 'build'
            Run cmake --build $BuildDir
        }
        'install' {
            $files = 'mlvdrive_tray.exe', 'mlvdrive.exe', 'mlvdrive_cli.exe' | ForEach-Object { Join-Path $BuildDir $_ }
            foreach ($f in $files) { if (-not (Test-Path $f)) { throw "no $f : build first" } }
            Stop-Installed
            Step "install: $InstallDir"
            New-Item -ItemType Directory -Force $InstallDir | Out-Null
            Copy-Item $files $InstallDir -Force
            Copy-Item (Join-Path $Root 'README.md'), (Join-Path $Root 'LICENSE') $InstallDir -Force
            $shell = New-Object -ComObject WScript.Shell
            $link = $shell.CreateShortcut($Shortcut)
            $link.TargetPath = Join-Path $InstallDir 'mlvdrive_tray.exe'
            $link.WorkingDirectory = $InstallDir
            $link.Description = 'MLV Drive: MLV clips as a drive of CinemaDNG sequences'
            $link.Save()
            Step "Start Menu: $Shortcut"
            $winfsp = Get-ItemProperty 'HKLM:\SOFTWARE\WOW6432Node\WinFsp' -Name InstallDir -ErrorAction SilentlyContinue
            if (-not $winfsp) {
                Write-Warning 'WinFsp is not installed: MLV Drive needs it for the drive (https://winfsp.dev/rel/).'
            }
            Write-Host 'Installed. Start "MLV Drive" from the Start Menu: its icon is in the notification area.'
        }
        'uninstall' {
            Stop-Installed
            if (Test-Path $InstallDir) { Step "remove $InstallDir"; Remove-Item -Recurse -Force $InstallDir }
            if (Test-Path $Shortcut) { Step "remove $Shortcut"; Remove-Item -Force $Shortcut }
            $run = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
            if (Get-ItemProperty $run -Name 'MLV Drive' -ErrorAction SilentlyContinue) {
                Step 'remove "Start with Windows"'
                Remove-ItemProperty $run -Name 'MLV Drive'
            }
            Write-Host "Uninstalled. The settings are kept in $env:APPDATA\MLV Drive."
        }
        default { throw "unknown action '$action' (clean, build, install, uninstall)" }
    }
}
