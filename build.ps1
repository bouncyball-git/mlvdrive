# Builds MLV Drive on Windows (Visual Studio 2022 or its Build Tools, with CMake and Ninja).
#
#   .\build.ps1 [clean] [build] [-Config Release|Debug|RelWithDebInfo]
#
#   clean    deletes the build folder
#   build    configures (if needed) and builds (the default action):
#              build\mlvdrive_cli.exe   the core on the command line (clip info, timings)
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
        default { throw "unknown action '$action' (clean, build)" }
    }
}
