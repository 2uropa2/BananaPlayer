#include "VideoPlayer.hpp"
#include "FileIO.hpp"
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <random>

namespace {
namespace fs = std::filesystem;
// Never write to the source (including hard links, symlinks and case aliases).
static bool isSameFile(const std::string& source, const std::string& destination) {
    std::error_code error;
    if (fs::equivalent(fs::u8path(source), fs::u8path(destination), error) && !error) return true;
    const auto a = fs::absolute(fs::u8path(source), error).lexically_normal().wstring();
    if (error) return false;
    const auto b = fs::absolute(fs::u8path(destination), error).lexically_normal().wstring();
    return !error && CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}
class OutputTransaction {
public:
    std::string path;
    explicit OutputTransaction(const std::string& destination) : destination_(destination) {
        const auto target = fs::u8path(destination);
        for (int attempt = 0; attempt < 16; ++attempt) {
            const auto leaf = target.stem().u8string() + ".banana-" + std::to_string(std::random_device{}()) + ".tmp" + target.extension().u8string();
            path = (target.parent_path() / fs::u8path(leaf)).u8string();
            HANDLE file = CreateFileW(FileIO::wide(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); return; }
            if (GetLastError() != ERROR_FILE_EXISTS) break;
        }
        path.clear();
    }
    ~OutputTransaction() { if (!path.empty()) FileIO::remove(path.c_str()); }
    bool commit() {
        if (!FileIO::replace(path, destination_)) return false;
        path.clear();
        return true;
    }
private:
    std::string destination_;
};
static bool copySegment(const std::string& src, const std::string& dst,
                                double start, double end, ExportControl* control) {
    if (src.empty() || dst.empty() || end <= start) return false;

    AVFormatContext* in_ctx = avformat_alloc_context();
    if (!in_ctx) return false;
    control->touch();
    in_ctx->interrupt_callback = { &ExportControl::interrupt, control };
    if (avformat_open_input(&in_ctx, src.c_str(), nullptr, nullptr) < 0) {
        return false;
    }
    if (avformat_find_stream_info(in_ctx, nullptr) < 0) {
        avformat_close_input(&in_ctx);
        return false;
    }

    AVFormatContext* out_ctx = nullptr;
    if (avformat_alloc_output_context2(&out_ctx, nullptr, nullptr, dst.c_str()) < 0
        || !out_ctx) {
        avformat_close_input(&in_ctx);
        return false;
    }

    // Map input stream index -> output stream index. Streams whose codec
    // ID doesn't fit the container (e.g. attached picture in mp4) are
    // silently dropped by leaving their map entry at -1.
    std::vector<int> stream_map(in_ctx->nb_streams, -1);
    for (unsigned i = 0; i < in_ctx->nb_streams; ++i) {
        AVStream* in_s = in_ctx->streams[i];
        AVMediaType t = in_s->codecpar->codec_type;
        if (t != AVMEDIA_TYPE_VIDEO
            && t != AVMEDIA_TYPE_AUDIO
            && t != AVMEDIA_TYPE_SUBTITLE) continue;
        AVStream* out_s = avformat_new_stream(out_ctx, nullptr);
        if (!out_s) continue;
        if (avcodec_parameters_copy(out_s->codecpar, in_s->codecpar) < 0) continue;
        // codec_tag=0 makes the muxer pick a compatible tag itself; the
        // input's tag often refers to a container-specific stream type
        // (e.g. AVI vs mp4) and mp4 mux rejects the mismatch.
        out_s->codecpar->codec_tag = 0;
        stream_map[i] = out_s->index;
    }

    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open2(&out_ctx->pb, dst.c_str(), AVIO_FLAG_WRITE, &in_ctx->interrupt_callback, nullptr) < 0) {
            avformat_free_context(out_ctx);
            avformat_close_input(&in_ctx);
            return false;
        }
    }
    out_ctx->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_NON_NEGATIVE;
    if (avformat_write_header(out_ctx, nullptr) < 0) {
        if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_ctx->pb);
        avformat_free_context(out_ctx);
        avformat_close_input(&in_ctx);
        return false;
    }

    // Seek to the keyframe at or before `start`. Using AVSEEK_FLAG_BACKWARD
    // guarantees we land on a keyframe (may be earlier than requested;
    // acceptable so the output is playable from frame 0).
    int64_t start_ts = (int64_t)(start * AV_TIME_BASE);
    control->touch();
    if (av_seek_frame(in_ctx, -1, start_ts, AVSEEK_FLAG_BACKWARD) < 0) {
        avio_closep(&out_ctx->pb);
        avformat_free_context(out_ctx);
        avformat_close_input(&in_ctx);
        return false;
    }

    // Shift output PTS so packets start at 0 (per stream). Otherwise
    // players open the file with a large initial delay while they wait
    // for pts to catch up to the mux-declared start.
    int64_t origin = AV_NOPTS_VALUE; // Shared across streams; preserve their relative timing.

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) {
        av_write_trailer(out_ctx);
        if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_ctx->pb);
        avformat_free_context(out_ctx);
        avformat_close_input(&in_ctx);
        return false;
    }

    bool ok = true;
    for (;;) {
        control->touch();
        if (control->cancelled.load()) { ok = false; break; }
        const int read_result = av_read_frame(in_ctx, pkt);
        if (read_result == AVERROR_EOF) break;
        if (read_result < 0) { ok = false; break; }
        int in_idx = pkt->stream_index;
        int out_idx = (in_idx < (int)stream_map.size()) ? stream_map[in_idx] : -1;
        if (out_idx < 0) { av_packet_unref(pkt); continue; }

        AVStream* in_s  = in_ctx->streams[in_idx];
        AVStream* out_s = out_ctx->streams[out_idx];

        // Stop condition: pts of this packet exceeds `end` on the video
        // stream. Audio may lag a bit past video in interleave; letting
        // it through until video crosses `end` keeps A/V aligned at
        // the tail. Any packet strictly beyond end is dropped.
        double pts_sec = (pkt->pts == AV_NOPTS_VALUE)
            ? -1.0
            : (double)pkt->pts * av_q2d(in_s->time_base);
        if (pts_sec > end + 0.5) { av_packet_unref(pkt); break; }
        if (pts_sec > end && in_s->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            av_packet_unref(pkt);
            break;
        }

        pkt->stream_index = out_idx;
        av_packet_rescale_ts(pkt, in_s->time_base, out_s->time_base);
        if (origin == AV_NOPTS_VALUE) {
            const auto first = pkt->dts != AV_NOPTS_VALUE ? pkt->dts : pkt->pts;
            origin = first == AV_NOPTS_VALUE ? 0 : av_rescale_q(first, out_s->time_base, AV_TIME_BASE_Q);
        }
        const auto offset = av_rescale_q(origin, AV_TIME_BASE_Q, out_s->time_base);
        if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= offset;
        if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= offset;
        control->progress.store((float)std::clamp((pts_sec - start) / (end - start), 0.0, 0.99));
        pkt->pos = -1;

        if (av_interleaved_write_frame(out_ctx, pkt) < 0) { ok = false; break; }
        av_packet_unref(pkt);
    }

    if (av_write_trailer(out_ctx) < 0) ok = false;
    av_packet_free(&pkt);
    if (!(out_ctx->oformat->flags & AVFMT_NOFILE) && avio_closep(&out_ctx->pb) < 0) ok = false;
    avformat_free_context(out_ctx);
    avformat_close_input(&in_ctx);
    return ok && !control->cancelled.load();
}

static bool encodeGif(const std::string& src, const std::string& dst,
                                     double start, double end,
                                     int target_fps, int target_width, ExportControl* control) {
    if (src.empty() || dst.empty() || end <= start) return false;
    if (target_fps  <= 0 || target_fps  > 60) target_fps  = 15;
    if (target_width <= 0 || target_width > 2000) target_width = 480;

    AVFormatContext* in_ctx = avformat_alloc_context();
    if (!in_ctx) return false;
    control->touch();
    in_ctx->interrupt_callback = { &ExportControl::interrupt, control };
    if (avformat_open_input(&in_ctx, src.c_str(), nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(in_ctx, nullptr) < 0) {
        avformat_close_input(&in_ctx); return false;
    }

    // Find the video stream + decoder.
    int vidx = -1;
    for (unsigned i = 0; i < in_ctx->nb_streams; ++i) {
        if (in_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            vidx = (int)i; break;
        }
    }
    if (vidx < 0) { avformat_close_input(&in_ctx); return false; }
    AVStream* vst = in_ctx->streams[vidx];
    const AVCodec* dec = avcodec_find_decoder(vst->codecpar->codec_id);
    if (!dec) { avformat_close_input(&in_ctx); return false; }
    AVCodecContext* dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx || avcodec_parameters_to_context(dec_ctx, vst->codecpar) < 0
                 || avcodec_open2(dec_ctx, dec, nullptr) < 0) {
        if (dec_ctx) avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_ctx); return false;
    }

    // Filter graph: fps + scale + split(palettegen | paletteuse). The
    // split branches let one path build the palette while the other
    // buffers frames; paletteuse merges them into a stream of PAL8
    // frames ready for the GIF encoder.
    AVFilterGraph* graph = avfilter_graph_alloc();
    if (!graph) {
        avcodec_free_context(&dec_ctx); avformat_close_input(&in_ctx); return false;
    }
    AVFilterContext* src_ctx = nullptr;
    AVFilterContext* sink_ctx = nullptr;
    char src_args[256];
    AVRational tb = vst->time_base;
    AVRational sar = dec_ctx->sample_aspect_ratio.num
                        ? dec_ctx->sample_aspect_ratio
                        : AVRational{1, 1};
    std::snprintf(src_args, sizeof(src_args),
        "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
        dec_ctx->width, dec_ctx->height, (int)dec_ctx->pix_fmt,
        tb.num, tb.den, sar.num, sar.den);
    const AVFilter* bufsrc  = avfilter_get_by_name("buffer");
    const AVFilter* bufsink = avfilter_get_by_name("buffersink");
    if (!bufsrc || !bufsink
        || avfilter_graph_create_filter(&src_ctx, bufsrc,  "in",  src_args, nullptr, graph) < 0
        || avfilter_graph_create_filter(&sink_ctx, bufsink, "out", nullptr,  nullptr, graph) < 0) {
        avfilter_graph_free(&graph); avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_ctx); return false;
    }
    // Restrict buffersink to PAL8 so paletteuse's output arrives ready
    // for the GIF encoder without another conversion filter. Uses
    // av_opt_set_bin directly to sidestep the FFmpeg 7.x deprecation
    // warning on the av_opt_set_int_list convenience macro.
    {
        const enum AVPixelFormat pix_fmts[] = { AV_PIX_FMT_PAL8 };
        if (av_opt_set_bin(sink_ctx, "pix_fmts",
                           (const uint8_t*)pix_fmts, sizeof(pix_fmts),
                           AV_OPT_SEARCH_CHILDREN) < 0) {
            avfilter_graph_free(&graph); avcodec_free_context(&dec_ctx);
            avformat_close_input(&in_ctx); return false;
        }
    }

    // Build the middle chain from a string. `split` buffers frames so
    // palettegen can consume the whole segment before paletteuse fires
    // per-frame. `stats_mode=full` gives one palette for the whole clip
    // (best color reproduction for a GIF of a short scene).
    char middle[256];
    std::snprintf(middle, sizeof(middle),
        "fps=%d,scale=%d:-2:flags=lanczos,split[a][b];"
        "[a]palettegen=stats_mode=full[p];[b][p]paletteuse=dither=bayer:bayer_scale=3",
        target_fps, target_width);
    AVFilterInOut* parsed_in  = nullptr;
    AVFilterInOut* parsed_out = nullptr;
    if (avfilter_graph_parse2(graph, middle, &parsed_in, &parsed_out) < 0
        || !parsed_in || !parsed_out) {
        if (parsed_in)  avfilter_inout_free(&parsed_in);
        if (parsed_out) avfilter_inout_free(&parsed_out);
        avfilter_graph_free(&graph); avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_ctx); return false;
    }
    // Link src -> parsed_in.first, parsed_out.first -> sink.
    bool link_ok =
        (avfilter_link(src_ctx, 0, parsed_in->filter_ctx, parsed_in->pad_idx) >= 0)
     && (avfilter_link(parsed_out->filter_ctx, parsed_out->pad_idx, sink_ctx, 0) >= 0);
    avfilter_inout_free(&parsed_in);
    avfilter_inout_free(&parsed_out);
    if (!link_ok || avfilter_graph_config(graph, nullptr) < 0) {
        avfilter_graph_free(&graph); avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_ctx); return false;
    }

    // Output: GIF muxer with GIF encoder that accepts PAL8.
    AVFormatContext* out_ctx = nullptr;
    if (avformat_alloc_output_context2(&out_ctx, nullptr, "gif", dst.c_str()) < 0
        || !out_ctx) {
        avfilter_graph_free(&graph); avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_ctx); return false;
    }
    const AVCodec* gif_codec = avcodec_find_encoder(AV_CODEC_ID_GIF);
    AVStream* out_st = avformat_new_stream(out_ctx, nullptr);
    AVCodecContext* enc_ctx = gif_codec ? avcodec_alloc_context3(gif_codec) : nullptr;
    if (!gif_codec || !out_st || !enc_ctx) {
        if (enc_ctx) avcodec_free_context(&enc_ctx);
        avformat_free_context(out_ctx);
        avfilter_graph_free(&graph); avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_ctx); return false;
    }
    // Query the sink for its actual output resolution (post-scale).
    int gif_w = av_buffersink_get_w(sink_ctx);
    int gif_h = av_buffersink_get_h(sink_ctx);
    if (gif_w <= 0 || gif_h <= 0) {
        avcodec_free_context(&enc_ctx); avformat_free_context(out_ctx);
        avfilter_graph_free(&graph); avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_ctx); return false;
    }
    enc_ctx->width     = gif_w;
    enc_ctx->height    = gif_h;
    enc_ctx->pix_fmt   = AV_PIX_FMT_PAL8;
    enc_ctx->time_base = AVRational{1, target_fps};
    enc_ctx->framerate = AVRational{target_fps, 1};
    if (out_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }
    if (avcodec_open2(enc_ctx, gif_codec, nullptr) < 0
        || avcodec_parameters_from_context(out_st->codecpar, enc_ctx) < 0) {
        avcodec_free_context(&enc_ctx); avformat_free_context(out_ctx);
        avfilter_graph_free(&graph); avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_ctx); return false;
    }
    out_st->time_base = enc_ctx->time_base;

    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open2(&out_ctx->pb, dst.c_str(), AVIO_FLAG_WRITE, &in_ctx->interrupt_callback, nullptr) < 0) {
            avcodec_free_context(&enc_ctx); avformat_free_context(out_ctx);
            avfilter_graph_free(&graph); avcodec_free_context(&dec_ctx);
            avformat_close_input(&in_ctx); return false;
        }
    }
    out_ctx->avoid_negative_ts = AVFMT_AVOID_NEG_TS_MAKE_NON_NEGATIVE;
    if (avformat_write_header(out_ctx, nullptr) < 0) {
        if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_ctx->pb);
        avcodec_free_context(&enc_ctx); avformat_free_context(out_ctx);
        avfilter_graph_free(&graph); avcodec_free_context(&dec_ctx);
        avformat_close_input(&in_ctx); return false;
    }

    // Seek input to the keyframe at/before `start`. Later frames whose
    // decoded pts is < start get discarded before feeding the filter.
    int64_t start_ts = (int64_t)(start * AV_TIME_BASE);
    control->touch();
    if (av_seek_frame(in_ctx, -1, start_ts, AVSEEK_FLAG_BACKWARD) < 0) {
        avio_closep(&out_ctx->pb);
        avformat_free_context(out_ctx);
        avformat_close_input(&in_ctx);
        return false;
    }
    avcodec_flush_buffers(dec_ctx);

    AVPacket* pkt   = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();
    AVFrame*  filt  = av_frame_alloc();
    bool ok = true;
    int64_t out_pts = 0;

    auto write_encoded = [&](AVFrame* f) -> bool {
        if (avcodec_send_frame(enc_ctx, f) < 0) return false;
        AVPacket* op = av_packet_alloc();
        while (avcodec_receive_packet(enc_ctx, op) == 0) {
            op->stream_index = out_st->index;
            av_packet_rescale_ts(op, enc_ctx->time_base, out_st->time_base);
            if (av_interleaved_write_frame(out_ctx, op) < 0) {
                av_packet_free(&op); return false;
            }
            av_packet_unref(op);
        }
        av_packet_free(&op);
        return true;
    };

    auto drain_sink = [&](bool assign_pts) -> bool {
        while (true) {
            if (control->cancelled.load()) return false;
            int fr = av_buffersink_get_frame(sink_ctx, filt);
            if (fr == AVERROR(EAGAIN) || fr == AVERROR_EOF) return true;
            if (fr < 0) return false;
            if (assign_pts) filt->pts = out_pts++;
            if (!write_encoded(filt)) { av_frame_unref(filt); return false; }
            av_frame_unref(filt);
        }
    };

    // Feed frames from `start` up to `end` into the filter graph.
    bool eof_reached = false;
    while (ok && !eof_reached) {
        control->touch();
        if (control->cancelled.load()) { ok = false; break; }
        int rf = av_read_frame(in_ctx, pkt);
        if (rf == AVERROR_EOF) { eof_reached = true; break; }
        if (rf < 0)            { ok = false; break; }
        if (pkt->stream_index != vidx) { av_packet_unref(pkt); continue; }

        if (avcodec_send_packet(dec_ctx, pkt) < 0) { av_packet_unref(pkt); ok = false; break; }
        av_packet_unref(pkt);

        while (ok) {
            if (control->cancelled.load()) { ok = false; break; }
            int dr = avcodec_receive_frame(dec_ctx, frame);
            if (dr == AVERROR(EAGAIN)) break;
            if (dr == AVERROR_EOF)     { eof_reached = true; break; }
            if (dr < 0)                { ok = false; break; }
            double pts_sec = (frame->best_effort_timestamp == AV_NOPTS_VALUE)
                                ? -1.0
                                : (double)frame->best_effort_timestamp * av_q2d(tb);
            if (pts_sec >= 0.0 && pts_sec < start) {
                av_frame_unref(frame); continue;   // pre-start warm-up
            }
            control->progress.store((float)(0.8 * std::clamp((pts_sec - start) / (end - start), 0.0, 1.0)));
            if (pts_sec > end) { eof_reached = true; av_frame_unref(frame); break; }
            if (av_buffersrc_add_frame(src_ctx, frame) < 0) { ok = false; av_frame_unref(frame); break; }
            av_frame_unref(frame);
            // palettegen doesn't emit until after EOF, so no sink drain
            // here - just push and continue.
        }
    }

    // Flush decoder + filter graph so palettegen emits and paletteuse
    // processes the buffered frames.
    if (ok) {
        avcodec_send_packet(dec_ctx, nullptr);
        while (avcodec_receive_frame(dec_ctx, frame) == 0) {
            double pts_sec = (frame->best_effort_timestamp == AV_NOPTS_VALUE)
                                ? -1.0
                                : (double)frame->best_effort_timestamp * av_q2d(tb);
            if (pts_sec > end) { av_frame_unref(frame); continue; }
            if (pts_sec >= 0.0 && pts_sec < start) { av_frame_unref(frame); continue; }
            (void)av_buffersrc_add_frame(src_ctx, frame);
            av_frame_unref(frame);
        }
        (void)av_buffersrc_add_frame(src_ctx, nullptr);   // signal EOF
        if (!drain_sink(true)) ok = false;
        // Flush encoder.
        if (ok && !write_encoded(nullptr)) ok = false;
    }
    if (av_write_trailer(out_ctx) < 0) ok = false;

    av_frame_free(&filt);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&enc_ctx);
    if (!(out_ctx->oformat->flags & AVFMT_NOFILE)) avio_closep(&out_ctx->pb);
    avformat_free_context(out_ctx);
    avfilter_graph_free(&graph);
    avcodec_free_context(&dec_ctx);
    avformat_close_input(&in_ctx);
    return ok && !control->cancelled.load();
}

} // namespace

bool VideoPlayer::exportSegment(const std::string& source, const std::string& destination,
                                double begin, double end, ExportControl* supplied) {
    if (source.empty() || destination.empty() || !std::isfinite(begin) || !std::isfinite(end)
        || begin < 0 || end <= begin || isSameFile(source, destination)) return false;
    ExportControl local;
    auto* control = supplied ? supplied : &local;
    if (control->cancelled.load()) return false;
    OutputTransaction output(destination);
    if (output.path.empty() || !copySegment(source, output.path, begin, end, control)
        || control->cancelled.load() || !output.commit()) return false;
    control->progress.store(1.0f);
    return true;
}
bool VideoPlayer::exportSegmentAsGif(const std::string& source, const std::string& destination,
                                     double begin, double end, int fps, int width, ExportControl* supplied) {
    if (source.empty() || destination.empty() || !std::isfinite(begin) || !std::isfinite(end)
        || begin < 0 || end <= begin || end - begin > 20.0 || isSameFile(source, destination)) return false;
    ExportControl local;
    auto* control = supplied ? supplied : &local;
    if (control->cancelled.load()) return false;
    OutputTransaction output(destination);
    if (output.path.empty() || !encodeGif(source, output.path, begin, end, fps, width, control)
        || control->cancelled.load() || !output.commit()) return false;
    control->progress.store(1.0f);
    return true;
}
