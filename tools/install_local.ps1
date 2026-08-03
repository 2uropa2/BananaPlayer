$ErrorActionPreference = 'Stop'
$logPath = 'C:\work\Player\install_last.log'

# Non-elevated install to %LOCALAPPDATA%\Programs\BananaPlayer\bin\ - the
# standard Windows per-user install location (VS Code, GitHub Desktop,
# Discord, etc. all use this). No UAC prompt needed on each iteration.
$srcDir = 'C:\work\Player\build\Release'
$dstDir = Join-Path $env:LOCALAPPDATA 'Programs\BananaPlayer\bin'

try {
    if (-not (Test-Path $dstDir)) {
        New-Item -ItemType Directory -Path $dstDir -Force | Out-Null
    }

    # Copy the runtime binaries. Skip tests, config sidecars (belong under
    # dataDir), and dev-only artifacts. copy_if_different-style: skip if
    # dst has same size + newer/equal mtime.
    $whitelist = @(
        'BananaPlayer.exe',
        'SDL3.dll',
        'avcodec-62.dll', 'avfilter-11.dll', 'avformat-62.dll', 'avutil-60.dll',
        'swresample-6.dll', 'swscale-9.dll',
        'yt-dlp.exe'
    )
    $copied = 0
    $skipped = 0
    foreach ($name in $whitelist) {
        $src = Join-Path $srcDir $name
        if (-not (Test-Path $src)) { continue }
        $dst = Join-Path $dstDir $name
        $needCopy = $true
        if (Test-Path $dst) {
            $s = Get-Item $src
            $d = Get-Item $dst
            if ($s.Length -eq $d.Length -and $s.LastWriteTime -le $d.LastWriteTime) {
                $needCopy = $false
            }
        }
        if ($needCopy) {
            Copy-Item -Path $src -Destination $dst -Force
            $copied++
        } else {
            $skipped++
        }
    }

    $dst = Join-Path $dstDir 'BananaPlayer.exe'
    if (-not (Test-Path $dst)) { throw "BananaPlayer.exe missing from $dstDir" }
    $info = Get-Item $dst
    "OK size=$($info.Length) time=$($info.LastWriteTime) copied=$copied skipped=$skipped path=$dstDir" |
        Out-File -Encoding utf8 $logPath
    exit 0
} catch {
    $_.ToString() | Out-File -Encoding utf8 $logPath
    exit 1
}
