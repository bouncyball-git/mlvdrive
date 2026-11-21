# MLV Drive's tests on real clips: every check that matters before a release, in a few minutes.
#
#   .\tools\test.ps1 -Folders G:\mlv, E:\mlv [-Drive N:] [-Bin build] [-Keep]
#
# The clips come from the folders (subfolders included): the shortest of each kind (camera, size,
# bit depth, compression) and the shortest spanned one (.MLV + .M00 ...). Tests:
#
#   clips      every clip opens and decodes (mlvdrive_tool info)
#   selftest   per kind: every frame, fast decoder = original, vector fixes = scalar, audio by range
#   formats    one frame as 16-bit, packed, LJ92 and pass-through DNG, each read back and compared
#   patterns   the 12 test charts are written
#   drive      a mount (on -Drive) of the folders + test patterns: every clip's middle frame equals a
#              fresh decode, a WAV equals mlvdrive_tool's, a chart's image equals mlvdrive_tool's
#   cache      a clip longer than the cache (512 MB) read twice: frames from the first pass found
#              made on the second; read speed above real time
#   live       a scratch folder mounted and watched: a clip copied in appears, one replaced by other
#              footage changes, one deleted disappears; an open file keeps reading meanwhile
#
# The mounts use their own letter (not the tray app's) and give every option, so the settings file
# doesn't change the results. Exit code 0: everything passed.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string[]] $Folders,
    [string] $Drive = '',
    [string] $Bin = (Join-Path (Split-Path $PSScriptRoot -Parent) 'build'),
    [switch] $Keep  # keep the scratch folder (to look at what failed)
)
$ErrorActionPreference = 'Stop'
$cli = Join-Path $Bin 'mlvdrive_tool.exe'
$mount = Join-Path $Bin 'mlvdrive.exe'
foreach ($p in $cli, $mount) { if (-not (Test-Path $p)) { throw "$p is missing: build first" } }
$scratch = Join-Path ([IO.Path]::GetTempPath()) ("mlvdrive-test-" + [Guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory -Force $scratch | Out-Null
$results = New-Object System.Collections.ArrayList

if (-not $Drive) {  # the first free letter from N: on
    $used = [IO.DriveInfo]::GetDrives() | ForEach-Object { $_.Name.Substring(0, 1) }
    $Drive = (('N', 'O', 'P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y') | Where-Object { $used -notcontains $_ } | Select-Object -First 1) + ':'
}
$Drive = $Drive.Substring(0, 1).ToUpper() + ':'

function Result([string] $test, [bool] $ok, [string] $detail) {
    [void]$results.Add([pscustomobject]@{ Test = $test; Result = $(if ($ok) { 'PASS' } else { 'FAIL' }); Detail = $detail })
    $color = if ($ok) { 'Green' } else { 'Red' }
    Write-Host ("  {0,-4} {1,-10} {2}" -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $test, $detail) -ForegroundColor $color
}
function Section([string] $text) { Write-Host "== $text" -ForegroundColor Cyan }
function Last($lines) { ($lines | Select-Object -Last 1) -as [string] }

# Mounts in the background; returns the process and its output file once "Mounted" shows.
function Start-Mount([string[]] $arguments) {
    $out = Join-Path $scratch ("mount-" + [Guid]::NewGuid().ToString('N').Substring(0, 6) + ".txt")
    $quoted = $arguments | ForEach-Object { if ($_ -match '\s') { '"' + $_ + '"' } else { $_ } }
    $p = Start-Process -FilePath $mount -ArgumentList $quoted -NoNewWindow -PassThru -RedirectStandardOutput $out `
        -RedirectStandardError ($out + '.err')
    for ($i = 0; $i -lt 240; $i++) {
        if ((Test-Path $out) -and (Select-String -Path $out -Pattern '^Mounted' -Quiet)) { return @{ Process = $p; Out = $out } }
        if ($p.HasExited) { break }
        Start-Sleep -Milliseconds 250
    }
    throw "mount didn't come up: $(Get-Content ($out + '.err') -Raw)"
}
function Stop-Mount($m) {
    & $mount unmount $Drive | Out-Null
    if (-not $m.Process.WaitForExit(15000)) { Stop-Process -Id $m.Process.Id -Force }
    Get-Content $m.Out
}
function Wait-Until([scriptblock] $condition, [int] $seconds) {
    $t = [Diagnostics.Stopwatch]::StartNew()
    while ($t.Elapsed.TotalSeconds -lt $seconds) {
        if (& $condition) { return $t.Elapsed.TotalSeconds }
        Start-Sleep -Milliseconds 250
    }
    return -1
}
function ImageHash([string] $path, [int] $bytes) {  # of the last `bytes` of a file (a DNG's image)
    $all = [IO.File]::ReadAllBytes($path)
    [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($all, $all.Length - $bytes, $bytes))
}
function Hash([string] $path) {
    $s = [IO.File]::OpenRead($path)
    try { [BitConverter]::ToString([Security.Cryptography.SHA256]::Create().ComputeHash($s)) } finally { $s.Dispose() }
}

$started = [Diagnostics.Stopwatch]::StartNew()
Write-Host ("MLV Drive tests: {0}; programs in {1}; test mounts on {2}" -f (& $mount --version), $Bin, $Drive)

try {
    # --- clips: every one opens; the kinds ---
    Section 'clips'
    $clips = @()
    foreach ($folder in $Folders) {
        # (Get-ChildItem -Recurse with a path that isn't there searches the whole drive for its name.)
        if (-not (Test-Path -LiteralPath $folder -PathType Container)) { throw "not a folder: $folder" }
        foreach ($f in Get-ChildItem -LiteralPath $folder -Recurse -Filter *.MLV -File | Sort-Object FullName) {
            $text = (& $cli info $f.FullName 2>&1) -join "`n"
            $get = { param($re) if ($text -match $re) { $Matches[1] } else { '' } }
            $clips += [pscustomobject]@{
                Path = $f.FullName; Folder = $folder
                Rel = $f.FullName.Substring($folder.TrimEnd('\').Length + 1)
                Kind = '{0}, {1}, {2}-bit, {3}' -f (& $get 'camera\s+(.+?) \(model'), (& $get 'image\s+(\d+ x \d+)'), (& $get ', (\d+)-bit,'), (& $get '-bit, ([^,]+),')
                Frames = [int](& $get 'frames\s+(\d+)'); Chunks = [int](& $get '\((\d+) chunk')
                Audio = $text -match 'audio\s+\d+ ch'; Decodes = $text -match 'frame \d+\s+decode'
            }
        }
    }
    $bad = @($clips | Where-Object { -not $_.Decodes })
    Result 'clips' ($clips.Count -gt 0 -and $bad.Count -eq 0) ("{0} clips open and decode{1}" -f $clips.Count, $(if ($bad) { "; failing: " + ($bad.Path -join ', ') } else { '' }))
    if (-not $clips) { throw 'no clips' }
    $kinds = $clips | Group-Object Kind
    $picked = @($kinds | ForEach-Object { $_.Group | Sort-Object Frames | Select-Object -First 1 })
    $spanned = $clips | Where-Object { $_.Chunks -gt 1 } | Sort-Object Frames | Select-Object -First 1
    if ($spanned -and $picked.Path -notcontains $spanned.Path) { $picked += $spanned }
    foreach ($k in $kinds) { Write-Host ("       {0,3} x {1}" -f $k.Count, $k.Name) }

    # --- selftest per kind ---
    Section 'selftest'
    foreach ($c in $picked) {
        $out = & $cli selftest $c.Path 2>&1
        $ok = $LASTEXITCODE -eq 0
        $what = ($out | Where-Object { $_ -match 'differ|audio:' } | ForEach-Object { ($_ -split ';')[0].Trim() }) -join ' / '
        Result 'selftest' $ok ("{0} ({1} frames{2}): {3}" -f $c.Rel, $c.Frames, $(if ($c.Chunks -gt 1) { ", $($c.Chunks) chunks" } else { '' }), $what)
    }

    # --- formats: one frame each way, read back ---
    Section 'formats'
    $one = $picked[0]
    $mid = [int]($one.Frames / 2)
    foreach ($fmt in '16', 'packed', 'lj92', 'pass') {
        $dng = Join-Path $scratch "format-$fmt.dng"
        & $cli dng $one.Path $mid $dng "--dng=$fmt" | Out-Null
        $extra = @(); if ($fmt -eq 'pass') { $extra = @('--no-fix') }
        $r = Last (& $cli check $one.Path $mid $dng @extra 2>&1)
        Result 'formats' ($r -like 'same as*') ("{0}: {1}" -f $fmt, $r)
    }

    # --- patterns ---
    Section 'patterns'
    $pat = Join-Path $scratch 'patterns'
    New-Item -ItemType Directory -Force $pat | Out-Null
    & $cli patterns $pat | Out-Null
    $n = @(Get-ChildItem $pat -Filter *.dng | Where-Object { $_.Length -eq 800 + 1920 * 1080 * 2 }).Count
    Result 'patterns' ($n -eq 12) "$n of 12 charts written, 1920x1080 16-bit"

    # --- drive: a mount of the folders and the test patterns ---
    Section "drive ($Drive)"
    $args_ = @('mount') + $Folders + @($Drive, '--dng=16', '--fix', '--focus-pixels=auto', '--bad-pixels=off',
        '--timecode=time', '--test-patterns', '--no-watch', '--cache=512')
    $m = Start-Mount $args_
    try {
        $roots = @{}
        foreach ($folder in $Folders) { $roots[$folder] = $true }
        $wrong = @(); $checked = 0
        foreach ($c in $clips) {
            $relDir = Split-Path $c.Rel -Parent
            $name = [IO.Path]::GetFileNameWithoutExtension($c.Path)
            $dir = if ($relDir) { Join-Path "$Drive\" (Join-Path $relDir $name) } else { Join-Path "$Drive\" $name }
            if (-not (Test-Path -LiteralPath $dir)) { $wrong += "$($c.Rel): no folder $dir"; continue }
            $f = [int]($c.Frames / 2)
            $r = Last (& $cli check $c.Path $f (Join-Path $dir ('{0}_{1:D6}.dng' -f $name, $f)) 2>&1)
            if ($r -like 'same as*') { $checked++ } else { $wrong += "$($c.Rel) frame ${f}: $r" }
        }
        Result 'drive' ($wrong.Count -eq 0) ("{0} of {1} clips: the middle frame equals a fresh decode{2}" -f $checked, $clips.Count, $(if ($wrong) { '; ' + ($wrong -join '; ') } else { '' }))

        $withAudio = $clips | Where-Object Audio | Sort-Object Frames | Select-Object -First 1
        if ($withAudio) {
            $name = [IO.Path]::GetFileNameWithoutExtension($withAudio.Path)
            $relDir = Split-Path $withAudio.Rel -Parent
            $dir = if ($relDir) { Join-Path "$Drive\" (Join-Path $relDir $name) } else { Join-Path "$Drive\" $name }
            $wav = Join-Path $scratch 'audio.wav'
            & $cli wav $withAudio.Path $wav | Out-Null
            $same = (Hash $wav) -eq (Hash (Join-Path $dir "$name.wav"))
            Result 'drive' $same ("{0}.wav on the drive equals mlvdrive_tool wav" -f $name)
        }
        $chart = ImageHash "$Drive\TestPatterns\TestPatterns_000125.dng" (1920 * 1080 * 2)
        $cliChart = ImageHash (Join-Path $pat '02.dng') (1920 * 1080 * 2)
        Result 'drive' ($chart -eq $cliChart) 'TestPatterns frame 125 (chart 2) equals mlvdrive_tool''s chart'

        # --- cache: a clip longer than the 512 MB cache, read twice ---
        Section 'cache'
        $long = $clips | Where-Object { $_.Frames -ge 250 -and $_.Kind -like '*1920 x 1080*' } | Sort-Object Frames | Select-Object -First 1
        if ($long) {
            $name = [IO.Path]::GetFileNameWithoutExtension($long.Path)
            $relDir = Split-Path $long.Rel -Parent
            $dir = if ($relDir) { Join-Path "$Drive\" (Join-Path $relDir $name) } else { Join-Path "$Drive\" $name }
            $read = & $cli read $dir --passes=2
            $fps = ($read | Select-String 'pass 1: ([\d.]+) frames/s').Matches[0].Groups[1].Value
            Result 'cache' ([double]$fps -gt 24) ("{0} ({1} frames, {2:N1} GB of DNG) read at {3} frames/s (above real time)" -f $name, $long.Frames, ($long.Frames * 4.15 / 1024), $fps)
        }
    } finally {
        $log = Stop-Mount $m
    }
    if ($long) {
        $st = $log | Select-String 'frames: (\d+) made for a reader, (\d+) found made.*read-ahead made (\d+), read (\d+)'
        if ($st) {
            $g = $st.Matches[0].Groups
            $kept = [int]$g[2].Value - [int]$g[4].Value  # found made, less those read-ahead made for that read
            Result 'cache' ($kept -gt 0) ("second pass found $kept frames made in the first (a plain LRU finds none)")
        } else {
            Result 'cache' $false 'no cache statistics at unmount'
        }
    }

    # --- live: a watched scratch folder ---
    Section "live ($Drive)"
    $small = @($clips | Sort-Object Frames | Select-Object -First 3)
    if ($small.Count -ge 2) {
        $live = Join-Path $scratch 'live'
        New-Item -ItemType Directory -Force $live | Out-Null
        Copy-Item $small[0].Path (Join-Path $live 'A.MLV')
        $m = Start-Mount @('mount', $live, $Drive, '--no-test-patterns', '--watch', '--dng=16', '--fix', '--bad-pixels=off')
        try {
            $aFrames = $small[0].Frames
            $held = [IO.File]::OpenRead("$Drive\A\A_000002.dng")
            Copy-Item $small[1].Path (Join-Path $live 'B.MLV')
            $t = Wait-Until { Test-Path "$Drive\B" } 20
            $r = if ($t -ge 0) { Last (& $cli check (Join-Path $live 'B.MLV') 1 "$Drive\B\B_000001.dng" 2>&1) } else { '' }
            Result 'live' ($t -ge 0 -and $r -like 'same as*') ("a clip copied in appears ({0:N1} s) and reads right" -f $t)

            $other = if ($small.Count -ge 3 -and $small[2].Frames -ne $aFrames) { $small[2] } else { $small[1] }
            Copy-Item $other.Path (Join-Path $live 'A.MLV') -Force
            $t = Wait-Until { ([IO.DirectoryInfo]::new("$Drive\A").GetFiles('*.dng')).Count -eq $other.Frames } 20
            Result 'live' ($t -ge 0) ("a clip replaced by other footage changes: {0} -> {1} frames ({2:N1} s)" -f $aFrames, $other.Frames, $t)
            $held.Position = 100000
            $n = $held.Read((New-Object byte[] 65536), 0, 65536)
            $held.Dispose()
            Result 'live' ($n -eq 65536) 'a file opened before the change keeps reading'

            [IO.File]::Delete((Join-Path $live 'B.MLV'))
            $t = Wait-Until { -not (Test-Path "$Drive\B") } 20
            Result 'live' ($t -ge 0) ("a clip deleted disappears ({0:N1} s)" -f $t)
        } finally {
            Stop-Mount $m | Out-Null
        }
    } else {
        Result 'live' $false 'needs at least 2 clips'
    }
} catch {
    Result 'error' $false $_.Exception.Message
} finally {
    if (-not $Keep) { Remove-Item -Recurse -Force $scratch -ErrorAction SilentlyContinue }
    else { Write-Host "scratch kept: $scratch" }
}

$failed = @($results | Where-Object Result -eq 'FAIL').Count
Write-Host ""
$color = if ($failed) { 'Red' } else { 'Green' }
Write-Host ("{0} of {1} passed, {2} failed ({3:N0} s)" -f ($results.Count - $failed), $results.Count, $failed, $started.Elapsed.TotalSeconds) -ForegroundColor $color
exit $(if ($failed) { 1 } else { 0 })
