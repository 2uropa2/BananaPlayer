$ErrorActionPreference = 'Stop'

# Local CI mirror. Runs the same checks the GitHub Actions workflow
# will run, so a green output here means the remote build should also
# pass. Fast to iterate: no network setup, no Docker, just call it.
#
# Steps:
#   1. cmake --build Release
#   2. banana_tests.exe (doctest suite)
#   3. BananaPlayer.exe --check (init-chain smoke)
#
# On any failure, prints the failed step + exits non-zero.

$repo = 'C:\work\Player'
$build = Join-Path $repo 'build'
$app = Join-Path $build 'Release\BananaPlayer.exe'
$tests = Join-Path $build 'Release\banana_tests.exe'

$stepFailed = $null

function Fail($msg) {
    Write-Host "FAIL: $msg" -ForegroundColor Red
    if ($script:stepFailed -eq $null) { $script:stepFailed = $msg }
}
function Pass($msg) {
    Write-Host "PASS: $msg" -ForegroundColor Green
}

Write-Host "== Build ==" -ForegroundColor Cyan
& cmake --build $build --config Release 2>&1 | Select-Object -Last 5
if ($LASTEXITCODE -ne 0) { Fail "cmake build (exit=$LASTEXITCODE)" }
elseif (-not (Test-Path $app))  { Fail "BananaPlayer.exe missing after build" }
elseif (-not (Test-Path $tests)) { Fail "banana_tests.exe missing after build" }
else { Pass "cmake build" }

if (-not $stepFailed) {
    Write-Host "`n== Unit tests ==" -ForegroundColor Cyan
    & $tests
    if ($LASTEXITCODE -ne 0) { Fail "banana_tests (exit=$LASTEXITCODE)" }
    else { Pass "unit tests" }
}

if (-not $stepFailed) {
    Write-Host "`n== Smoke test (--check) ==" -ForegroundColor Cyan
    & $app --check
    if ($LASTEXITCODE -ne 0) { Fail "smoke test (exit=$LASTEXITCODE)" }
    else { Pass "smoke test" }
}

Write-Host ""
if ($stepFailed) {
    Write-Host "FAILED: $stepFailed" -ForegroundColor Red
    exit 1
} else {
    Write-Host "All CI checks passed." -ForegroundColor Green
    exit 0
}
