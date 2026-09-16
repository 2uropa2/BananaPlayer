#include "Thumbnail.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>

ThumbnailProvider::ThumbnailProvider(SDL_Renderer* renderer)
    : renderer_(renderer) {
    worker_ = std::thread(&ThumbnailProvider::workerLoop, this);
}

ThumbnailProvider::~ThumbnailProvider() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        quit_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();

    // Drain any remaining pending uploads (their pixel buffers just get
    // dropped - no SDL texture created for them) and destroy every cached
    // texture. Main thread is the only one that ever created textures.
    pending_uploads_.clear();
    for (auto& e : cache_) {
        if (e.tex) SDL_DestroyTexture(e.tex);
    }
    cache_.clear();
}

void ThumbnailProvider::openFile(const std::string& path) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        path_ = path;
        reopen_requested_ = true;
        // Any pending job from the previous file is meaningless now.
        has_pending_ = false;
        // Bump generation. Old-file decodes still in flight will land
        // in pending_uploads_ with the pre-bump generation number;
        // the main-thread drain discards them by generation mismatch,
        // preventing the old file's frames from getting cached under
        // bucket keys the new file's requests will hit.
        ++current_generation_;
    }
    // Destroy all cached textures for the previous file (main thread call).
    for (auto& e : cache_) {
        if (e.tex) SDL_DestroyTexture(e.tex);
    }
    cache_.clear();
    // Drop any yet-to-be-uploaded pixel buffers from the previous file too.
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pending_uploads_.clear();
    }
    cv_.notify_all();
}

void ThumbnailProvider::close() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        path_.clear();
        close_requested_ = true;
        has_pending_ = false;
        pending_uploads_.clear();
        ++current_generation_;   // same rationale as openFile
    }
    // Main-thread texture destruction.
    for (auto& e : cache_) {
        if (e.tex) SDL_DestroyTexture(e.tex);
    }
    cache_.clear();
    cv_.notify_all();
}

ThumbnailProvider::Result ThumbnailProvider::requestAt(double t) {
    // Drain any completed decodes into SDL textures before returning.
    drainPendingUploadsMainThread();

    int64_t key = bucketKey(t);
    if (CacheEntry* e = findByBucket(key)) {
        Result r;
        r.tex = e->tex;
        r.w   = e->w;
        r.h   = e->h;
        r.pts = e->pts;
        return r;
    }

    // Miss - post/replace the pending work item.
    {
        std::lock_guard<std::mutex> lk(mutex_);
        has_pending_ = true;
        pending_target_ = t;
    }
    cv_.notify_one();
    return Result{};
}

ThumbnailProvider::CacheEntry* ThumbnailProvider::findByBucket(int64_t key) {
    for (size_t i = 0; i < cache_.size(); ++i) {
        if (cache_[i].bucket_key == key) {
            // LRU touch: move to the back.
            CacheEntry hit = cache_[i];
            cache_.erase(cache_.begin() + (long)i);
            cache_.push_back(hit);
            return &cache_.back();
        }
    }
    return nullptr;
}

void ThumbnailProvider::drainPendingUploadsMainThread() {
    // Move ownership of the queued uploads to a local first so we can
    // release the mutex before spending time on SDL calls.
    std::deque<PendingUpload> local;
    uint64_t gen_now = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        local.swap(pending_uploads_);
        gen_now = current_generation_;
    }
    for (auto& up : local) {
        // Discard uploads stamped with an obsolete generation - they
        // were decoded against the pre-openFile file and should NOT
        // land in the cache (bucket_key alone isn't file-unique).
        if (up.generation != gen_now) continue;
        // If a bucket already lives in the cache (a duplicate finished
        // decode) skip creating a second texture.
        if (findByBucket(up.bucket_key)) continue;
        SDL_Texture* tex = SDL_CreateTexture(renderer_,
                                             SDL_PIXELFORMAT_ABGR8888,
                                             SDL_TEXTUREACCESS_STATIC,
                                             up.w, up.h);
        if (!tex) continue;
        // Nearest-neighbor is fine for a 200 px preview.
        SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_LINEAR);
        SDL_UpdateTexture(tex, nullptr, up.rgba.data(), up.w * 4);
        CacheEntry ce;
        ce.bucket_key = up.bucket_key;
        ce.pts = up.pts;
        ce.w = up.w;
        ce.h = up.h;
        ce.tex = tex;
        cache_.push_back(ce);
        while (cache_.size() > CACHE_MAX) {
            if (cache_.front().tex) SDL_DestroyTexture(cache_.front().tex);
            cache_.erase(cache_.begin());
        }
    }
}

void ThumbnailProvider::workerLoop() {
    for (;;) {
        std::string pending_path;
        bool do_reopen = false;
        bool do_close = false;
        bool do_decode = false;
        double target = 0.0;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [&]{
                return quit_.load()
                    || reopen_requested_.load()
                    || close_requested_.load()
                    || has_pending_;
            });
            if (quit_.load()) {
                lk.unlock();
                closeContextLocked();
                return;
            }
            if (reopen_requested_.load()) {
                pending_path = path_;
                do_reopen = true;
                reopen_requested_ = false;
            }
            if (close_requested_.load()) {
                do_close = true;
                close_requested_ = false;
            }
            if (has_pending_) {
                target = pending_target_;
                has_pending_ = false;
                do_decode = true;
                // Snapshot the generation we're decoding under. If
                // openFile bumps current_generation_ mid-decode, this
                // captured value is what the pending upload gets tagged
                // with, and the main-thread drain will discard it.
                worker_generation_ = current_generation_;
            }
        }
        if (do_close) {
            closeContextLocked();
            // A close plus a pending decode from before the close: the
            // decode is stale, drop it.
            do_decode = false;
        }
        if (do_reopen) {
            closeContextLocked();
            if (!pending_path.empty()) openContextLocked(pending_path);
        }
        if (do_decode && fmt_ctx_ && dec_ctx_) {
            decodeOne(target);
        }
    }
}

void ThumbnailProvider::openContextLocked(const std::string& path) {
    fmt_ctx_ = avformat_alloc_context();
    if (!fmt_ctx_) return;
    fmt_ctx_->interrupt_callback = { &ThumbnailProvider::interruptIo, this };
    io_deadline_ = SDL_GetTicks() + 5000;
    if (avformat_open_input(&fmt_ctx_, path.c_str(), nullptr, nullptr) < 0) {
        fmt_ctx_ = nullptr;
        return;
    }
    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
        return;
    }
    video_stream_idx_ = -1;
    for (unsigned int i = 0; i < fmt_ctx_->nb_streams; ++i) {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx_ = (int)i;
            break;
        }
    }
    if (video_stream_idx_ < 0) {
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
        return;
    }
    AVCodecParameters* par = fmt_ctx_->streams[video_stream_idx_]->codecpar;
    const AVCodec* codec = avcodec_find_decoder(par->codec_id);
    if (!codec) {
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
        return;
    }
    dec_ctx_ = avcodec_alloc_context3(codec);
    if (!dec_ctx_
        || avcodec_parameters_to_context(dec_ctx_, par) < 0
        || avcodec_open2(dec_ctx_, codec, nullptr) < 0) {
        if (dec_ctx_) avcodec_free_context(&dec_ctx_);
        dec_ctx_ = nullptr;
        avformat_close_input(&fmt_ctx_);
        fmt_ctx_ = nullptr;
        return;
    }
    // Prefer a fast, single-thread setup - we're grabbing one keyframe at
    // a time, threading buys nothing and just wastes CPU.
    dec_ctx_->thread_count = 1;
    video_time_base_ = fmt_ctx_->streams[video_stream_idx_]->time_base;
    src_w_ = dec_ctx_->width;
    src_h_ = dec_ctx_->height;
    io_deadline_ = 0;
}

int ThumbnailProvider::interruptIo(void* opaque) {
    auto* self = static_cast<ThumbnailProvider*>(opaque);
    return self->quit_.load() || self->reopen_requested_.load()
        || self->close_requested_.load()
        || (self->io_deadline_ && SDL_GetTicks() >= self->io_deadline_);
}

void ThumbnailProvider::closeContextLocked() {
    if (sws_ctx_) { sws_freeContext(sws_ctx_); sws_ctx_ = nullptr; }
    if (dec_ctx_) { avcodec_free_context(&dec_ctx_); dec_ctx_ = nullptr; }
    if (fmt_ctx_) { avformat_close_input(&fmt_ctx_); fmt_ctx_ = nullptr; }
    video_stream_idx_ = -1;
    src_w_ = 0;
    src_h_ = 0;
}

void ThumbnailProvider::decodeOne(double target_pts) {
    if (!fmt_ctx_ || !dec_ctx_ || video_stream_idx_ < 0) return;
    if (src_w_ <= 0 || src_h_ <= 0) return;

    // Clamp target to something sane. Duration may be unknown - fall back
    // to the target as-is and let av_seek_frame do the clamping.
    if (target_pts < 0.0) target_pts = 0.0;

    // av_seek_frame expects timestamps in the video stream's time_base.
    int64_t ts = 0;
    if (video_time_base_.num > 0 && video_time_base_.den > 0) {
        ts = (int64_t)(target_pts * (double)video_time_base_.den
                       / (double)video_time_base_.num);
    }

    // BACKWARD flag lands on the previous keyframe so decode can start
    // cleanly. Small alignment error (up to ~2 s) is acceptable for a
    // preview.
    io_deadline_ = SDL_GetTicks() + 5000;
    if (av_seek_frame(fmt_ctx_, video_stream_idx_, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        return;
    }
    avcodec_flush_buffers(dec_ctx_);

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  raw = av_frame_alloc();
    if (!pkt || !raw) {
        if (pkt) av_packet_free(&pkt);
        if (raw) av_frame_free(&raw);
        return;
    }

    // Read forward until we get a frame at-or-past the target, or hit a
    // bounded attempt cap so a broken file can't spin forever.
    const int MAX_PACKETS = 400;
    int packets_read = 0;
    bool got_frame = false;
    double got_pts = 0.0;
    AVFrame* keep = av_frame_alloc();
    if (!keep) {
        av_packet_free(&pkt);
        av_frame_free(&raw);
        return;
    }

    while (packets_read < MAX_PACKETS) {
        if (interruptIo(this)) break;
        io_deadline_ = SDL_GetTicks() + 5000;
        int rr = av_read_frame(fmt_ctx_, pkt);
        if (rr < 0) {
            // EOF or error: flush the decoder to release any buffered
            // frame we already have.
            avcodec_send_packet(dec_ctx_, nullptr);
            while (avcodec_receive_frame(dec_ctx_, raw) == 0) {
                double p = (raw->best_effort_timestamp != AV_NOPTS_VALUE
                            && video_time_base_.den > 0)
                    ? raw->best_effort_timestamp * (double)video_time_base_.num
                                                 / (double)video_time_base_.den
                    : got_pts;
                got_pts = p;
                av_frame_unref(keep);
                av_frame_move_ref(keep, raw);
                got_frame = true;
            }
            break;
        }
        packets_read++;
        if (pkt->stream_index != video_stream_idx_) {
            av_packet_unref(pkt);
            continue;
        }
        if (avcodec_send_packet(dec_ctx_, pkt) < 0) {
            av_packet_unref(pkt);
            continue;
        }
        av_packet_unref(pkt);
        for (;;) {
            int rf = avcodec_receive_frame(dec_ctx_, raw);
            if (rf == AVERROR(EAGAIN) || rf == AVERROR_EOF) break;
            if (rf < 0) break;
            double p = (raw->best_effort_timestamp != AV_NOPTS_VALUE
                        && video_time_base_.den > 0)
                ? raw->best_effort_timestamp * (double)video_time_base_.num
                                             / (double)video_time_base_.den
                : got_pts;
            // Keep the most recent frame we have. We stop as soon as one
            // lands at-or-past the target so the preview matches the
            // hover point rather than the previous keyframe.
            av_frame_unref(keep);
            av_frame_move_ref(keep, raw);
            got_pts = p;
            got_frame = true;
            if (p + 0.05 >= target_pts) {
                // Close enough. Break out of both loops.
                packets_read = MAX_PACKETS;
                break;
            }
        }
    }

    av_packet_free(&pkt);
    av_frame_free(&raw);

    if (!got_frame) {
        av_frame_free(&keep);
        return;
    }

    // Downscale + convert to packed RGBA.
    int out_w = THUMB_W;
    int out_h = std::max(1, (int)((double)THUMB_W * (double)src_h_ / (double)src_w_));
    // Enforce even dimensions for some sws internals and keep the popup
    // rectangle predictable.
    if (out_h < 1) out_h = 1;

    sws_ctx_ = sws_getCachedContext(sws_ctx_,
        keep->width, keep->height, (AVPixelFormat)keep->format,
        out_w, out_h, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx_) { av_frame_free(&keep); return; }

    std::vector<uint8_t> buf((size_t)out_w * out_h * 4);
    uint8_t* dst[4] = { buf.data(), nullptr, nullptr, nullptr };
    int      dst_ls[4] = { out_w * 4, 0, 0, 0 };
    sws_scale(sws_ctx_, keep->data, keep->linesize, 0, keep->height,
              dst, dst_ls);
    av_frame_free(&keep);

    PendingUpload up;
    up.bucket_key = bucketKey(target_pts);
    up.pts = got_pts;
    up.w = out_w;
    up.h = out_h;
    up.generation = worker_generation_;   // set by workerLoop at decode start
    up.rgba = std::move(buf);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        pending_uploads_.push_back(std::move(up));
    }
}
