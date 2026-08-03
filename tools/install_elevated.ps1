$ErrorActionPreference = 'Stop'
$logPath = 'C:\work\Player\install_last.log'
try {
    Copy-Item -Path 'C:\work\Player\build\Release\BananaPlayer.exe' `
              -Destination 'C:\Program Files\BananaPlayer\bin\BananaPlayer.exe' `
              -Force
    $dst = Get-Item 'C:\Program Files\BananaPlayer\bin\BananaPlayer.exe'
    "OK size=$($dst.Length) time=$($dst.LastWriteTime)" | Out-File -Encoding utf8 $logPath
    exit 0
} catch {
    $_.ToString() | Out-File -Encoding utf8 $logPath
    exit 1
}
