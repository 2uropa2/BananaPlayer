param(
    [Parameter(Mandatory=$true)]
    [ValidatePattern('^\d+\.\d+\.\d+$')]
    [string]$Version
)

$ErrorActionPreference = 'Stop'

# Bumps the version string across the three canonical locations that
# must stay in sync. Doesn't build or tag - just edits + prints a diff
# summary. Follow up with: run_ci.ps1, then `git commit -am "vX.Y.Z"`,
# then `git tag vX.Y.Z && git push --tags` (Actions will pick up).

$repo = 'C:\work\Player'
$parts = $Version -split '\.'
$M = $parts[0]; $N = $parts[1]; $P = $parts[2]

Write-Host "Bumping to $Version" -ForegroundColor Cyan

# Version.hpp
$vh = Join-Path $repo 'Version.hpp'
$c = Get-Content $vh -Raw
$c = $c -replace '#define BP_VERSION "\d+\.\d+\.\d+"',
                 "#define BP_VERSION `"$Version`""
Set-Content -Path $vh -Value $c -Encoding UTF8 -NoNewline
Write-Host "  * $vh"

# CMakeLists.txt project()
$cml = Join-Path $repo 'CMakeLists.txt'
$c = Get-Content $cml -Raw
$c = $c -replace 'project\(BananaPlayer VERSION \d+\.\d+\.\d+ LANGUAGES CXX\)',
                 "project(BananaPlayer VERSION $Version LANGUAGES CXX)"
Set-Content -Path $cml -Value $c -Encoding UTF8 -NoNewline
Write-Host "  * $cml"

# resources\app.rc (both the tuple + string form)
$rc = Join-Path $repo 'resources\app.rc'
$c = Get-Content $rc -Raw
$c = $c -replace '#define APP_VERSION\s+\d+,\d+,\d+,\d+',
                 "#define APP_VERSION       $M,$N,$P,0"
$c = $c -replace '#define APP_VERSION_STR\s+"\d+\.\d+\.\d+\.\d+"',
                 "#define APP_VERSION_STR   `"$Version.0`""
Set-Content -Path $rc -Value $c -Encoding UTF8 -NoNewline
Write-Host "  * $rc"

# vcpkg.json
$vjs = Join-Path $repo 'vcpkg.json'
if (Test-Path $vjs) {
    $c = Get-Content $vjs -Raw
    $c = $c -replace '"version-string"\s*:\s*"\d+\.\d+\.\d+"',
                     "`"version-string`": `"$Version`""
    Set-Content -Path $vjs -Value $c -Encoding UTF8 -NoNewline
    Write-Host "  * $vjs"
}

Write-Host "`nSummary: files updated in-place. Next steps:" -ForegroundColor Green
Write-Host "  powershell -File tools\run_ci.ps1"
Write-Host "  # add CHANGELOG.md entry"
Write-Host "  git commit -am `"v$Version`""
Write-Host "  git tag v$Version"
Write-Host "  git push origin master --tags   # (when remote is set up)"
