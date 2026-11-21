# Builds MLV Drive on Windows (Visual Studio 2022 or its Build Tools, with CMake and Ninja).
#
#   .\build.ps1 [clean] [build] [install] [uninstall] [package] [-Config Release|Debug|RelWithDebInfo]
#
#   clean      deletes the build folder
#   build      configures (if needed) and builds (the default action):
#                build\mlvdrive_tray.exe  the tray app (MLV Drive in the notification area)
#                build\mlvdrive.exe       the drive on the command line
#                build\mlvdrive_tool.exe   the core on the command line (clip info, DNGs, timings)
#   install    for the current user, no administrator rights (tools\install.ps1): the programs into
#              %LOCALAPPDATA%\Programs\MLV Drive, a Start Menu shortcut "MLV Drive" (the tray app);
#              stops a running copy first. WinFsp must be installed separately (https://winfsp.dev/rel/)
#   uninstall  removes them, the shortcut and "Start with Windows"; keeps the settings
#              (%APPDATA%\MLV Drive)
#   package    dist\MLVDrive-<version>-win64.zip: the programs, pixel maps, README, LICENSE and the
#              installer (install.cmd / uninstall.cmd), for a GitHub release
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(ValueFromRemainingArguments = $true)] [string[]] $Actions,
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')] [string] $Config = 'Release'
)
$ErrorActionPreference = 'Stop'
$Root = $PSScriptRoot
$BuildDir = Join-Path $Root 'build'
if (-not $Actions) { $Actions = @('build') }

function Step([string] $text) { Write-Host "==> $text" -ForegroundColor Cyan }

function Run {
    & $args[0] @($args | Select-Object -Skip 1)
    if ($LASTEXITCODE -ne 0) { throw "failed (exit code $LASTEXITCODE): $($args -join ' ')" }
}

# The version CMakeLists.txt gives the project.
function Get-Version {
    $m = Select-String -Path (Join-Path $Root 'CMakeLists.txt') -Pattern 'project\(\S+ VERSION ([0-9.]+)'
    if (-not $m) { throw 'no project version in CMakeLists.txt' }
    $m.Matches[0].Groups[1].Value
}

# What gets installed or released, gathered in build\stage: the programs, the pixel maps, the
# README and LICENSE, the installer.
function New-Stage {
    $stage = Join-Path $BuildDir 'stage'
    $programs = 'mlvdrive_tray.exe', 'mlvdrive.exe', 'mlvdrive_tool.exe' | ForEach-Object { Join-Path $BuildDir $_ }
    foreach ($f in $programs) { if (-not (Test-Path $f)) { throw "no $f : build first" } }
    if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
    New-Item -ItemType Directory -Force $stage | Out-Null
    Copy-Item $programs $stage
    Copy-Item (Join-Path $BuildDir 'pixelmaps') $stage -Recurse
    Copy-Item (Join-Path $Root 'README.md'), (Join-Path $Root 'LICENSE') $stage
    Copy-Item (Join-Path $Root 'tools\install.ps1'), (Join-Path $Root 'tools\install.cmd'), (Join-Path $Root 'tools\uninstall.cmd') $stage
    $stage
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
            $stage = New-Stage
            & (Join-Path $stage 'install.ps1')
        }
        'uninstall' {
            & (Join-Path $Root 'tools\install.ps1') -Uninstall
        }
        'package' {
            $stage = New-Stage
            $version = Get-Version
            $dist = Join-Path $Root 'dist'
            New-Item -ItemType Directory -Force $dist | Out-Null
            $zip = Join-Path $dist "MLVDrive-$version-win64.zip"
            if (Test-Path $zip) { Remove-Item -Force $zip }
            # The files inside a folder named after the release, as people unpack it.
            $named = Join-Path $BuildDir "MLVDrive-$version"
            if (Test-Path $named) { Remove-Item -Recurse -Force $named }
            Copy-Item $stage $named -Recurse
            Compress-Archive -Path $named -DestinationPath $zip
            Remove-Item -Recurse -Force $named
            Step ("package: {0} ({1:N1} MB)" -f $zip, ((Get-Item $zip).Length / 1MB))
        }
        default { throw "unknown action '$action' (clean, build, install, uninstall, package)" }
    }
}
