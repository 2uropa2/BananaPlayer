#pragma once

// Promo / ad slot data structures + fetcher.
//
// The Banana Player revenue model is: FREE builds show a single promo
// banner on the splash (never during playback), Pro builds skip it. The
// promo is a small JSON manifest hosted on the vendor domain; Banana
// fetches it once per launch (with a per-day cache) and renders the
// resulting text + click URL on the splash bottom.
//
// Design decisions:
//   - Text-only banner (no remote image). Keeps the network fetch tiny
//     (~200 bytes) and eliminates image codec + storage risk. Banana's
//     existing Style::TextButton renders it, no new pipeline.
//   - Fetch runs on a background thread so a slow / dead server can't
//     stall the splash. Splash renders whatever's in the local cache.
//   - Cache lives at %APPDATA%\BananaPlayer\banana.promo.json;
//     hand-editable for dev testing.
//   - Fetch failures are silent - promo is best-effort marketing, not
//     playback.
//   - Every promo entry gets an expires_at ISO date so stale campaigns
//     drop off automatically.
//
// Wire from UIController:
//   PromoSlot::start(cache_dir);              // in ctor
//   auto cur = PromoSlot::current();          // in drawSplash
//   if (cur) render banner + click surface;
//   PromoSlot::shutdown();                    // in dtor

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace Promo {

struct Banner {
    std::string headline;    // main line (e.g. "Banana Pro - remove ads")
    std::string subline;     // optional second line
    std::string click_url;   // opened in default browser on click
    // Empty = no expiry; otherwise ISO date "YYYY-MM-DD" - server-side
    // rolls forward, but the cache honors it too so a stale on-disk
    // cache doesn't linger past its intended window.
    std::string expires_at;
};

// Kick off the background fetcher. Reads a stale local cache (if any)
// immediately so current() has something usable right away, then tries
// the remote fetch on a background thread. Safe to call more than once
// per process; second call is a no-op unless shutdown() ran first.
void start(const std::string& cache_dir);

// Cleanly stop the fetcher thread. Called from UIController dtor.
void shutdown();

// The banner to display right now. Returns nullptr when:
//   - no cache and no successful fetch yet
//   - the cached banner's expires_at is in the past
//   - the caller has explicitly disabled promos (Pro users, etc.)
// Thread-safe. The returned pointer is owned by Promo:: internals and
// stays valid until the next start()/shutdown() cycle.
const Banner* current();

// Pro / license state toggles the slot off. Set true when a valid
// license is loaded; the slot then always returns nullptr.
void setSuppressed(bool suppressed);

}   // namespace Promo
