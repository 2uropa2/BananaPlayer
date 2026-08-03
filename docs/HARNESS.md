# Build + release harness

Point-in-time snapshot of what the CI / build / release pipeline for
Banana Player looks like. Kept short so onboarding a fresh contributor
(or a future Claude session) doesn't have to grep the repo.

## Local dev loop

```
cmake --build build --config Release
powershell -File tools\install_local.ps1   # -> %LOCALAPPDATA%\Programs\BananaPlayer\bin\
```

No UAC prompt. `install_local.ps1` uses `copy_if_different`, so
successive rebuilds only touch `BananaPlayer.exe` when it changes.

## Local CI mirror

Runs the same three checks the remote CI runs, but from the current
tree with the current binaries. Green here = green on Actions.

```
powershell -File tools\run_ci.ps1
```

Checks:
1. `cmake --build build --config Release`
2. `build\Release\banana_tests.exe`  (doctest, currently 21 cases / 61 assertions)
3. `build\Release\BananaPlayer.exe --check` (smoke test - 3 render frames + exit)

## Smoke test

`BananaPlayer.exe --check` (or `--smoke-test`) exercises the full init chain:

* SDL video + audio init
* Window + renderer creation
* ImGui bootstrap + font atlas build (Segoe UI + Malgun Gothic merged
  atlas plus three standalone subtitle atlases)
* `VideoPlayer` construction
* `UIController` constructor (loads every sidecar: resume, bookmarks,
  stats, config, recents, last-session playlist)
* `Promo::start` (spawns fetch thread, reads cache)
* Three render frames of the splash (draws wordmark, banana glyph,
  recent-files list, promo banner if cached, version footer)
* Clean shutdown

Exit code:
* `0` - init + 3 frames + shutdown all succeeded
* Non-zero (SDL error) or process crash - CI fails, artifact not uploaded

## GitHub Actions

`.github/workflows/build.yml` runs on:

* Every push to `master` / `main`
* Every PR against `master` / `main`
* Manual `workflow_dispatch`
* Every tag matching `v*` (releases)

Steps:
1. Checkout
2. Set up MSVC dev cmd
3. Point at pre-installed vcpkg (windows-latest runner ships it)
4. cmake configure + build Release
5. Run `banana_tests.exe`
6. Run `BananaPlayer.exe --check`
7. **If tag push**: CPack `ZIP;NSIS` and upload draft GitHub Release

vcpkg binary cache uses the GitHub Actions cache backend, keyed on
`vcpkg.json`. Cold build: ~30 minutes (FFmpeg compile). Warm: ~5 min.

## Release ceremony

```
powershell -File tools\bump_version.ps1 -Version 1.5.0
# updates: Version.hpp, CMakeLists.txt, resources\app.rc, vcpkg.json

powershell -File tools\run_ci.ps1        # verify green locally

# edit CHANGELOG.md - add a section for 1.5.0

git commit -am "v1.5.0"
git tag v1.5.0
git push origin master --tags   # (once remote is wired up)
```

The tag push triggers the Actions release job, which builds + packs +
attaches artifacts to a **draft** GitHub Release. Human publishes.

## Coverage reality

Only the SRT / SMI subtitle parsers are unit-tested. `VideoPlayer`,
`UIController`, `Promo`, `Thumbnail`, `SingleInstance`, `Youtube` are
zero-covered. Smoke test catches crashes in the init chain but not
logic bugs after init.

**Not yet in the harness (but should be before monetization launch):**
* `MediaClock` invariants (nudge dead zone, seek freeze, first-audio-
  frame capture) - all three had recent regressions
* Promo JSON parser (malformed input, oversize payload, injection)
* License key verification (once it exists)
* HDR tone-map golden-frame comparison
* Deinterlace filter graph rebuild
