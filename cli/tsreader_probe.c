// Optional codec-level probing of elementary streams.
//
// The engine keeps a bounded sample of each probeable ES PID (see
// tsr_probe_feed, called from the packet path). At finalization time
// (tsr_probe_streams) each sample is probed:
//
//   * Video (H.264/HEVC/MPEG-2/MPEG-4) and MP3/AC-3/E-AC-3 are decoded via
//     an FFmpeg raw *demuxer* (h264/hevc/mpegvideo/mpeg4/mp3/ac3/eac3) when
//     FFmpeg is available. The sample is written to a temp file and opened
//     with the stable avformat_open_input(filename) API, which works
//     uniformly across FFmpeg versions (unlike the AVIO/decoder APIs, which
//     have churned signatures and, in some builds, relocated headers).
//   * AAC (ADTS) is parsed by a small built-in ADTS header reader that needs
//     no FFmpeg at all — this also covers builds whose "adts" demuxer is
//     not compiled in.
//
// The probe is best-effort: any failure simply leaves probe_detail empty;
// it never affects analysis or extraction.

#include "tsreader_cli.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TSR_PROBE_BUF_MAX (256 * 1024)

int tsr_stream_probeable(int stream_type)
{
    switch (stream_type) {
    case 0x01: // MPEG-2 video
    case 0x02:
    case 0x10: // MPEG-4 visual
    case 0x1b: // H.264
    case 0x24: // HEVC
    case 0x03: // MP3
    case 0x0f: // AAC (ADTS)
    case 0x81: // AC-3
    case 0x87: // E-AC-3 (DVB)
    case 0x90: // E-AC-3
        return 1;
    default:
        return 0;
    }
}

void tsr_probe_feed(tsr_stream_t *s, const uint8_t *payload, int paylen)
{
    if (!tsr_stream_probeable(s->stream_type) || paylen <= 0)
        return;
    if (s->probe_len >= TSR_PROBE_BUF_MAX)
        return;
    if (!s->probe_buf) {
        s->probe_buf = malloc(TSR_PROBE_BUF_MAX);
        if (!s->probe_buf)
            return;
    }
    size_t room = TSR_PROBE_BUF_MAX - s->probe_len;
    if ((size_t)paylen > room)
        paylen = (int)room;
    memcpy(s->probe_buf + s->probe_len, payload, paylen);
    s->probe_len += (size_t)paylen;
}

// ------------------------------------------------------------------
// FFmpeg path (raw demuxer via temp file)
// ------------------------------------------------------------------
#ifdef TSR_HAVE_FFMPEG

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

// FF_PROFILE_* constants are not exposed in some FFmpeg builds' headers, so
// use their stable numeric values: H.264 profile_idc per ISO/IEC 14496-10,
// AAC audio-object-type-1, MPEG-2/4 & HEVC per their respective specs.
#define P_H264_BASELINE 66
#define P_H264_MAIN 77
#define P_H264_EXTENDED 88
#define P_H264_HIGH 100
#define P_H264_HIGH_10 110
#define P_H264_HIGH_422 122
#define P_H264_HIGH_444 244
#define P_AAC_MAIN 0
#define P_AAC_LOW 1
#define P_AAC_SSR 2
#define P_AAC_LTP 3
#define P_AAC_HE 4
#define P_AAC_HE_V2 29
#define P_MPEG2_SIMPLE 0
#define P_MPEG2_MAIN 1
#define P_MPEG2_SNDR 2
#define P_MPEG2_SNHC 3
#define P_MPEG2_SS 4
#define P_MPEG2_HIGH1440 5
#define P_MPEG4_SIMPLE 0
#define P_MPEG4_CORE 1
#define P_MPEG4_MAIN 2
#define P_MPEG4_BSP 3
#define P_MPEG4_HIGHPROFILE 7
#define P_MPEG4_DSP 8
#define P_HEVC_MAIN 1
#define P_HEVC_MAIN_10 2
#define P_HEVC_MAIN_STILL 3
#define P_HEVC_MAIN_10_STILL 4
#define P_HEVC_MAIN_SPATIAL 5
#define P_AC3 0x20
#define P_AC3_E 0x21
#define P_EAC3 1
#define P_MP3_MPEG1 1
#define P_MP3_MPEG2 2
#define P_MP3_MPEG2_5 3

static const char *ff_format_name(int stream_type)
{
    switch (stream_type) {
    case 0x01: return "mpegvideo";
    case 0x02:
    case 0x10: return "mpeg4";
    case 0x1b: return "h264";
    case 0x24: return "hevc";
    case 0x03: return "mp3";
    case 0x81: return "ac3";
    case 0x87:
    case 0x90: return "eac3";
    default:   return NULL; // AAC (0x0f) is handled by the built-in parser.
    }
}

static const char *profile_name(enum AVCodecID id, int profile)
{
    switch (id) {
    case AV_CODEC_ID_H264:
        switch (profile) {
        case P_H264_BASELINE: return "Baseline";
        case P_H264_MAIN: return "Main";
        case P_H264_EXTENDED: return "Extended";
        case P_H264_HIGH: return "High";
        case P_H264_HIGH_10: return "High 10";
        case P_H264_HIGH_422: return "High 4:2:2";
        case P_H264_HIGH_444: return "High 4:4:4";
        }
        return NULL;
    case AV_CODEC_ID_AAC:
        switch (profile) {
        case P_AAC_MAIN: return "Main";
        case P_AAC_LOW: return "LC";
        case P_AAC_SSR: return "SSR";
        case P_AAC_LTP: return "LTP";
        case P_AAC_HE: return "HE";
        case P_AAC_HE_V2: return "HE v2";
        }
        return NULL;
    case AV_CODEC_ID_MPEG2VIDEO:
        switch (profile) {
        case P_MPEG2_SIMPLE: return "Simple";
        case P_MPEG2_MAIN: return "Main";
        case P_MPEG2_SNDR: return "SNDR";
        case P_MPEG2_SNHC: return "SNHC";
        case P_MPEG2_SS: return "SS";
        case P_MPEG2_HIGH1440: return "High@1440";
        }
        return NULL;
    case AV_CODEC_ID_MPEG4:
        switch (profile) {
        case P_MPEG4_SIMPLE: return "Simple";
        case P_MPEG4_CORE: return "Core";
        case P_MPEG4_MAIN: return "Main";
        case P_MPEG4_BSP: return "BSL";
        case P_MPEG4_HIGHPROFILE: return "High";
        case P_MPEG4_DSP: return "DSP";
        }
        return NULL;
    case AV_CODEC_ID_HEVC:
        switch (profile) {
        case P_HEVC_MAIN: return "Main";
        case P_HEVC_MAIN_10: return "Main 10";
        case P_HEVC_MAIN_STILL: return "Main Still";
        case P_HEVC_MAIN_10_STILL: return "Main 10 Still";
        case P_HEVC_MAIN_SPATIAL: return "Main Spatiotemporal";
        }
        return NULL;
    case AV_CODEC_ID_AC3:
        return profile == P_AC3_E ? "E-AC-3" : "AC-3";
    case AV_CODEC_ID_EAC3:
        return "E-AC-3";
    case AV_CODEC_ID_MP3:
        switch (profile) {
        case P_MP3_MPEG1: return "MPEG-1";
        case P_MP3_MPEG2: return "MPEG-2";
        case P_MP3_MPEG2_5: return "MPEG 2.5";
        }
        return NULL;
    default:
        return NULL;
    }
}

// Level idc to "L#.#" text. H.264: idc/10 (40 = Level 4.0). HEVC uses a
// non-linear idc scale (93 = Level 3.1, 123 = 4.1, 153 = 5.2, ...).
static void level_text(enum AVCodecID id, int level, char *out, size_t n)
{
    out[0] = 0;
    if (level <= 0)
        return;
    if (id == AV_CODEC_ID_HEVC) {
        static const struct {
            int idc;
            const char *t;
        } hevc[] = {
            {30, "1"},   {60, "2"},   {90, "3"},   {93, "3.1"},
            {120, "4"},  {123, "4.1"}, {150, "5"}, {153, "5.2"},
            {186, "5.3"}, {189, "6"},  {195, "6.2"}, {205, "6.3"},
        };
        for (size_t i = 0; i < sizeof(hevc) / sizeof(hevc[0]); i++)
            if (hevc[i].idc == level) {
                snprintf(out, n, "L%s", hevc[i].t);
                return;
            }
        return; // unknown HEVC level: do not guess
    }
    if (id == AV_CODEC_ID_H264)
        snprintf(out, n, "L%d.%d", level / 10, level % 10);
}

static int write_temp(const uint8_t *buf, size_t len, char *path, size_t psz)
{
    snprintf(path, psz, P_tmpdir "/tsrprobe_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0)
        return -1;
    FILE *f = fdopen(fd, "wb");
    if (!f) {
        close(fd);
        unlink(path);
        return -1;
    }
    size_t w = fwrite(buf, 1, len, f);
    if (fclose(f) != 0 || w != len) {
        unlink(path);
        return -1;
    }
    return 0;
}

static int tsr_probe_ffmpeg(tsr_stream_t *s, const uint8_t *buf, size_t len)
{
    const char *fn = ff_format_name(s->stream_type);
    if (!fn)
        return 0;
    const AVInputFormat *fmt = av_find_input_format(fn);
    if (!fmt)
        return 0; // demuxer not built into this FFmpeg
    char path[160];
    if (write_temp(buf, len, path, sizeof path) < 0)
        return 0;
    int ok = 0;
    AVFormatContext *fc = NULL;
    if (avformat_open_input(&fc, path, fmt, NULL) >= 0) {
        if (avformat_find_stream_info(fc, NULL) >= 0 && fc->nb_streams > 0) {
            const AVCodecParameters *cp = fc->streams[0]->codecpar;
            const char *pn = profile_name(cp->codec_id, cp->profile);
            char lvl[16];
            level_text(cp->codec_id, cp->level, lvl, sizeof lvl);
            char body[160];
            if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
                if (cp->width > 0 && cp->height > 0)
                    snprintf(body, sizeof body, "%d x %d", cp->width,
                             cp->height);
                else
                    body[0] = 0;
            } else {
                int ch = cp->ch_layout.nb_channels;
                if (cp->sample_rate > 0 && ch > 0)
                    snprintf(body, sizeof body, "%d Hz, %d ch",
                             cp->sample_rate, ch);
                else
                    body[0] = 0;
            }
            size_t o = 0;
            s->probe_detail[0] = 0;
            if (pn)
                o += (size_t)snprintf(s->probe_detail + o,
                                      sizeof s->probe_detail - o, "%s", pn);
            if (lvl[0]) {
                s->probe_detail[o++] = ' ';
                o += (size_t)snprintf(s->probe_detail + o,
                                      sizeof s->probe_detail - o, "%s", lvl);
            }
            if (body[0]) {
                s->probe_detail[o++] = ' ';
                o += (size_t)snprintf(s->probe_detail + o,
                                      sizeof s->probe_detail - o, "%s", body);
            }
            ok = (s->probe_detail[0] != 0);
        }
        avformat_close_input(&fc);
    }
    unlink(path);
    return ok;
}

#endif // TSR_HAVE_FFMPEG

// ------------------------------------------------------------------
// Dependency-free AAC/ADTS header parser
// ------------------------------------------------------------------

// ADTS fixed header (protection_absent == 1), ISO/IEC 13818-7 5.3.1.1.2:
//   b0 = 0xFF
//   b1 = sync(3) | id(1) | layer(2) | protection_absent(1)  (0xF0-0xF3)
//   b2 = profile(2) | sampling_frequency_index(4) | private(1)
//        | channel_configuration(1: MSB)
//   b3 = channel_configuration(2: LSB) | originality(1) | ...
// profile is audio_object_type - 1 (so 1 == LC).
static int tsr_probe_adts(tsr_stream_t *s, const uint8_t *p, size_t n)
{
    static const int freqs[13] = { 96000, 88200, 64000, 48000, 44100,
                                   32000, 24000, 22050, 16000, 12000,
                                   11025, 8000, 0 };
    static const char *objs[6] = { "Main", "LC", "SSR", "LTP", "SWSR",
                                   "SDT" };
    for (size_t i = 0; i + 7 <= n; i++) {
        if (p[i] != 0xFF || (p[i + 1] & 0xF0) != 0xF0)
            continue;
        int profile = (p[i + 2] >> 6) & 0x3;
        int sfi = (p[i + 2] >> 2) & 0x0F; // full index lives in byte 2
        int ch = ((p[i + 2] & 0x01) << 2) | ((p[i + 3] >> 6) & 0x3);
        if (sfi > 12)
            continue; // not a plausible header; keep scanning
        ch = ch ? ch : 1; // channel_config 0 is reserved; assume mono
        snprintf(s->probe_detail, sizeof s->probe_detail, "%s %d Hz, %d ch",
                 profile < 6 ? objs[profile] : "AAC", freqs[sfi], ch);
        return 1;
    }
    return 0;
}

// ------------------------------------------------------------------
// Driver (idempotent: buffers are consumed)
// ------------------------------------------------------------------

void tsr_probe_streams(tsr_mux_t *m)
{
#ifdef TSR_HAVE_FFMPEG
    av_log_set_level(AV_LOG_PANIC); // keep probe chatter off stderr
#endif
    for (int i = 0; i < TSR_MAX_PIDS; i++) {
        tsr_stream_t *s = &m->streams[i];
        if (!s->probe_buf || s->probe_len == 0)
            continue;
        int ok = 0;
        if (s->stream_type == 0x0f)
            ok = tsr_probe_adts(s, s->probe_buf, s->probe_len);
        if (!ok) {
#ifdef TSR_HAVE_FFMPEG
            ok = tsr_probe_ffmpeg(s, s->probe_buf, s->probe_len);
#endif
        }
        if (!ok)
            s->probe_detail[0] = 0;
        free(s->probe_buf);
        s->probe_buf = NULL;
        s->probe_len = 0;
    }
}
