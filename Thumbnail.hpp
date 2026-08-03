#pragma once

// ThumbnailProvider - on-demand video-frame thumbnails for the seek-bar
// hover preview. Owns its OWN AVFormatContext / AVCodecContext (never shares
// with the primary VideoPlayer), a worker thread that seeks + decodes on
// demand, and an LRU cache of SDL textures.
//
// Threading model:
//   - The worker thread does all FFmpeg work (open, seek, decode, sws).
//   - SDL_Texture creation and SDL_UpdateTexture MUST happen on the main
//     thread on Windows/D3D11. The worker fills a CPU-side "pending upload"
//     record (raw RGBA pixels + metadata) that the main thread drains during
//     requestAt().
//   - Cache lookup + return is fast and lock-only.
//
// Design intent:
//   - Non-blocking: requestAt returns nullptr on miss and posts a job. The
//     caller polls again next frame.
//   - Coalesce: hover moves the pointer dozens of times per second. If a
//     new request comes in while the worker is busy, only the LATEST target
//     is kept; intermediates are dropped.
//   - Bucket key: 2-second buckets so a small pointer wiggle reuses the
//     same thumbnail without triggering a re-decode.
//   - No HW decode: single-frame CPU decode is cheap and dodges the extra
//     complexity of hwframe transfer for a preview.

#include <SDL3/SDL.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
#include <libavutil/imgutils.h>
}

class ThumbnailProvider {
public:
    struct Result {
        SDL_Texture* tex = nullptr;
        int    w   = 0;
        int    h   = 0;
        double pts = 0.0;
    };

    explicit ThumbnailProvider(SDL_Renderer* renderer);
    ~ThumbnailProvider();

    // Open a file for on-demand thumbnail extraction. Cheap - schedules the
    // open on the worker thread and returns immediately. Safe to call while
    // the primary VideoPlayer holds the same path open (we use our own
    // AVFormatContext).
    void openFile(const std::string& path);
    void close();

    // Non-blocking. Returns a cached thumbnail if we have one bucketed near
    // t (bucket = 2 s). Otherwise returns an empty Result AND kicks off a
    // background decode. The caller should poll again next frame.
    Result requestAt(double t);

private:
    // Worker-side context lifecycle.
    void workerLoop();
    void openContextLocked(const std::string& path);
    void closeContextLocked();
    // Decode one frame near target_pts, converting to packed RGBA and
    // pushing it onto pending_uploads_. Bucketized cache key based on
    // target_pts (not the actual decoded pts) so successive hovers within
    // the same bucket coalesce even if the keyframe alignment differs.
    void decodeOne(double target_pts);

    // Main-thread cache maintenance. Drains pending_uploads_ into
    // real SDL textures and evicts overflow entries.
    void drainPendingUploadsMainThread();

    // Called on the main thread only. Returns the cached entry for a
    // bucket key, or nullptr on miss. Touches LRU order on hit.
    struct CacheEntry;
    CacheEntry* findByBucket(int64_t bucket_key);

    static int64_t bucketKey(double t) {
        // 2-second buckets. Negatives -> 0 to keep the key math trivial.
        if (t < 0.0) t = 0.0;
        return (int64_t)(t / 2.0);
    }

    SDL_Renderer* renderer_ = nullptr;

    // File-open state. `path_` is written by the main thread under mutex_
    // and consumed by the worker via a re-open request flag.
    std::string  path_;
    std::atomic<bool> reopen_requested_{false};
    std::atomic<bool> close_requested_{false};
    std::atomic<bool> quit_{false};

    // Coalesced work slot. If has_pending_ is true when a request comes
    // in, we overwrite pending_target_ with the newer value so the worker
    // always chases the freshest pointer position.
    bool   has_pending_ = false;
    double pending_target_ = 0.0;
    // File generation counter, bumped on every openFile / close.
    // The worker's in-flight decode carries the generation it started
    // with; the main-thread drain compares against current_generation_
    // and discards any pending upload whose generation is stale. Without
    // this, an old-file decode that finishes AFTER an openFile clear
    // would land in the cache under a bucket_key shared with the new
    // file - and later a matching-t hover would surface the old file's
    // frame.
    uint64_t current_generation_ = 0;
    uint64_t worker_generation_  = 0;   // stamped by workerLoop at decode start

    std::mutex mutex_;
    std::condition_variable cv_;

    // Pending upload = worker finished a decode, main thread hasn't
    // yet turned it into an SDL_Texture. Guarded by mutex_.
    struct PendingUpload {
        int64_t  bucket_key = 0;
        double   pts = 0.0;
        int      w = 0;
        int      h = 0;
        uint64_t generation = 0;
        std::vector<uint8_t> rgba;  // packed RGBA8, tightly packed rows
    };
    std::deque<PendingUpload> pending_uploads_;

    // LRU cache. Main-thread only. Newest at back; oldest at front.
    struct CacheEntry {
        int64_t bucket_key = 0;
        double  pts = 0.0;
        int     w = 0;
        int     h = 0;
        SDL_Texture* tex = nullptr;
    };
    std::vector<CacheEntry> cache_;
    static constexpr size_t CACHE_MAX = 8;

    std::thread worker_;

    // Worker-owned FFmpeg state. Access from the worker thread only.
    AVFormatContext* fmt_ctx_ = nullptr;
    AVCodecContext*  dec_ctx_ = nullptr;
    SwsContext*      sws_ctx_ = nullptr;
    int              video_stream_idx_ = -1;
    AVRational       video_time_base_{0, 1};
    int              src_w_ = 0;
    int              src_h_ = 0;
    // Target thumbnail width. Height picked from source aspect.
    static constexpr int THUMB_W = 200;
};
