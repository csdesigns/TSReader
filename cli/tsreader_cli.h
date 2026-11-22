// tsreader_cli.h — Modern MPEG-TS analysis engine (POSIX, C11).
//
// This is a clean-room rewrite of the core TSReader demux/analysis engine,
// written for macOS and Linux. It replaces the Windows-specific
// ParseIncomingTSDataThread from TSReader.c with a compact, testable
// pipeline:
//
//   input (file | UDP | TCP)
//       -> 0x47 realignment
//       -> per-packet stats (PID counters, continuity, PCR)
//       -> PSI/SI section reassembly + CRC check
//       -> program / service / EPG data model
//       -> raw TS recording and elementary stream extraction
//
// No Win32, no MSVC extensions. C11 + POSIX sockets only.

#ifndef TSREADER_CLI_H
#define TSREADER_CLI_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TSR_VERSION_STRING "3.0.0"

#define TSR_MAX_PIDS          8192
#define TSR_MAX_PROGRAMS     4096
#define TSR_MAX_SERVICES     4096
#define TSR_NAME_LEN         256
#define TSR_DESC_LEN         1024
#define TSR_EXTRACT_PREBUF_MAX (256 * 1024)  // per-PID pre-PMT payload buffer

// Stream roles (what a PID carries).
enum {
    TSR_ROLE_NONE = 0,
    TSR_ROLE_PAT,
    TSR_ROLE_PMT,
    TSR_ROLE_SDT,
    TSR_ROLE_NIT,
    TSR_ROLE_EIT,
    TSR_ROLE_VCT,
    TSR_ROLE_TDT,
    TSR_ROLE_CAT,
    TSR_ROLE_MGT,
    TSR_ROLE_BAT,
    TSR_ROLE_SCT,
    TSR_ROLE_MMT,
    TSR_ROLE_NULL,
    TSR_ROLE_ES,              // elementary stream (video/audio/subtitle/data)
    TSR_ROLE_AC3_ES,
    TSR_ROLE_PRIVATE_SI,
};

// Broad stream categories (for filtering extraction).
enum {
    TSR_CAT_NONE = 0,
    TSR_CAT_VIDEO,
    TSR_CAT_AUDIO,
    TSR_CAT_SUBTITLE,
    TSR_CAT_DATA,
    TSR_CAT_SYNC,             // AC-3/EC-3, DTS (not ISO sync bytes)
    TSR_CAT_SI,               // tables
};

typedef struct tsr_stream {
    uint16_t pid;
    int      in_use;
    int      role;            // TSR_ROLE_*
    int      category;        // TSR_CAT_*
    int      stream_type;     // PMT stream_type for ES, -1 otherwise
    int      pcr;             // 1 if this PID is a PCR PID
    uint16_t program_number;  // owning program (ES), 0 otherwise
    uint32_t packets;
    uint64_t bytes;
    uint32_t continuity_errors;
    uint32_t reserved_af;      // packets with AF control 00 (reserved)
    uint32_t last_cc;
    int      cc_valid;
    char     codec[TSR_NAME_LEN];   // human readable ("H.264/AVC (video)")
    char     language[8];           // ISO-639-2 from PMT ("" if none)
    char     component_tag[8];      // DVB component tag ("" if none)
    // codec probing (bounded ES sample + result; see tsreader_probe.c)
    uint8_t *probe_buf;
    size_t   probe_len;
    char     probe_detail[128];     // "High L4.0 640 x 360", "LC 96000 Hz, 2 ch"
    // extraction
    FILE    *out;
    char     out_path[512];
    uint64_t out_bytes;
    uint8_t *prebuf;       // payloads received before the output opened
    size_t   prebuf_len;
    // PES unwrapping state for extraction (H.222.0). Mirrors tstools'
    // ts2es: a PES header (max 9 + 255 = 264 bytes) can straddle TS
    // packet boundaries, so header bytes are staged here (from the start
    // code) until the header is complete.
    uint8_t  pes_pend[288];
    int      pes_pend_len;    // staged header bytes
    int      pes_hdr_total;   // staged header size (9 + hd); 0 = unknown
    int      pes_unit_left;   // ES bytes left in current unit (0 = unknown)
    int      pes_unit_open;
    int      need_unit_start; // skip data until the first PUSI
    int      pes_stream;      // sticky: a PES start code has been seen
    int      adts_stream;     // sticky: unwrapped ADTS (raw passthrough)
    int      pusi_seen;
} tsr_stream_t;

typedef struct tsr_program {
    uint16_t program_number;
    int      in_use;
    uint16_t pmt_pid;
    uint16_t pcr_pid;
    char     name[TSR_NAME_LEN];        // from SDT/VCT if linked
    char     provider[TSR_NAME_LEN];
    uint16_t service_id;                // linked DVB service id, 0 if none
    uint16_t atsc_major;
    uint8_t  atsc_minor;
    int      has_atsc;
    uint64_t pcr_first_ticks;           // 90 kHz ticks, 0 = none
    uint64_t pcr_last_ticks;
    int      pcr_ticks_valid;           // pcr_first/last carry real ticks
    double   bitrate_bps;               // computed at close
} tsr_program_t;

typedef struct tsr_service {
    uint16_t service_id;
    int      in_use;
    uint16_t eits_pid;
    uint32_t original_network_id;
    char     name[TSR_NAME_LEN];
    char     provider[TSR_NAME_LEN];
    uint16_t logical_number;            // 0 if none
    int      has_number;
    char     affiliation[TSR_NAME_LEN];
} tsr_service_t;

typedef struct tsr_event {
    uint16_t service_id;
    uint16_t program_number;   // ATSC: program number
    int      atsc;             // 1 = ATSC ETT, 0 = DVB EIT
    uint16_t event_id;
    int      present;          // EIT flag bit0
    int      is_now;
    int      is_next;
    int64_t  start_unix;       // seconds since epoch (approx, see notes)
    uint32_t duration_s;
    char     name[TSR_NAME_LEN];
    char     description[TSR_DESC_LEN];
    char     language[8];
    uint8_t  genre[4];         // ATSC content categories
    char     audio_visual[8];  // ATSC AV flags summary
} tsr_event_t;

// Live-reader thread body (returns when the input ends or stop is set).
struct tsr_mux;
typedef int (*tsr_live_reader_fn)(struct tsr_mux *m);

typedef struct tsr_mux {
    // input
    char    input_desc[512];
    int     packet_len;          // 188
    FILE   *file;
    char    file_path[512];
    int     file_size_valid;
    int64_t file_size;
    int     file_eof;
    // network
    int     sock;
    char    udp_iface[64];
    int     live;
    volatile int *stop_flag;
    int     live_kind;            // 0 = socket (UDP/TCP), 1 = HDHomeRun
    tsr_live_reader_fn live_reader_fn; // NULL = built-in socket reader
    volatile int live_reader_done;     // reader thread finished
    // HDHomeRun source (live_kind == 1)
    char    hdhr_host[256];
    int     hdhr_port;
    char    hdhr_tune[512];
    int     hdhr_tuner_id;        // -1 = not tuned / released

    // feed state
    uint8_t  carry[8192];        // realignment carryover
    size_t   carry_len;
    int      resync_depth;

    // stats
    uint64_t total_packets;
    uint64_t total_bytes;
    uint32_t sync_errors;
    uint32_t discarded_packets;
    double   wall_start;         // seconds
    double   wall_end;
    int      wall_valid;

    // data model
    tsr_stream_t   streams[TSR_MAX_PIDS];
    tsr_program_t  programs[TSR_MAX_PROGRAMS];
    tsr_service_t  services[TSR_MAX_SERVICES];
    tsr_event_t   *events;
    size_t         n_events;
    size_t         event_cap;

    char     network_name[TSR_NAME_LEN];
    char     network_provider[TSR_NAME_LEN];
    uint32_t network_id;
    int      has_network_id;
    int      tdt_valid;          // TDT (0x14) seen
    uint16_t tdt_time_raw;       // raw 13-bit TDT time field
    int64_t  tdt_unix;           // last TDT/TM value

    // control / output
    int      record_all;
    char     record_path[512];
    FILE    *record_out;
    uint64_t record_bytes;
    double   max_duration;       // seconds, 0 = unlimited
    uint64_t max_bytes;          // 0 = unlimited
    char     extract_dir[512];
    int      extract_programs[64];
    int      n_extract_programs;
    int      extract_pids[64];
    int      n_extract_pids;
    int      dump_tables;        // print raw SI sections
    int      ignore_eit;         // don't parse EIT (speed)
    int      pmt_seen;           // at least one PMT parsed

    // si section reassembly per SI pid
    struct tsr_si_state *si;     // index [8192], NULL entries

    int      closed;
} tsr_mux_t;

// ---- lifecycle -------------------------------------------------------------

// Initialize all fields; caller provides a zeroed struct.
void tsr_init(tsr_mux_t *m);
void tsr_free(tsr_mux_t *m);

// Open a file for analysis (does not start reading).
int tsr_open_file(tsr_mux_t *m, const char *path);
// Open a UDP input: addr may be multicast or unicast, iface optional.
int tsr_open_udp(tsr_mux_t *m, const char *addr, int port, const char *iface);
// Open a TCP input (server must push a raw MPEG-TS).
int tsr_open_tcp(tsr_mux_t *m, const char *host, int port);
// Open an HDHomeRun source: POST /tune {"url": tune_url}, then stream
// GET /transport_stream/<id>. The tuner is released when reading stops.
// tune_url is passed to the device unchanged (auto/v5.1, atsc://12/12.1,
// 8vsb:687000000, qam:65, ...). Requires a libcurl-enabled build.
int tsr_open_hdhr(tsr_mux_t *m, const char *host, int port,
                  const char *tune_url);
// Thread body for the HDHomeRun transport reader (see tsr_open_hdhr).
int tsr_hdhr_reader(tsr_mux_t *m);
// Start the wall-clock used for --duration / --max-mb accounting. Live
// sources call this when the stream becomes available (for HDHomeRun,
// after the tune handshake), so duration limits are measured from real
// streaming, not from process start.
void tsr_mark_stream_start(tsr_mux_t *m);

// Set output options (before processing).
void tsr_set_record(tsr_mux_t *m, const char *path);
void tsr_set_extract_dir(tsr_mux_t *m, const char *dir);
void tsr_extract_program(tsr_mux_t *m, uint16_t program_number);
void tsr_extract_pid(tsr_mux_t *m, uint16_t pid);
void tsr_set_max_duration(tsr_mux_t *m, double seconds);
void tsr_set_max_bytes(tsr_mux_t *m, uint64_t bytes);
void tsr_set_dump_tables(tsr_mux_t *m, int enable);

// Feed raw bytes (any framing) into the pipeline. Returns 0 on success.
int tsr_feed(tsr_mux_t *m, const uint8_t *data, size_t len);

// Process until EOF (file) or until stop flag is set (live). Returns 0.
int tsr_process(tsr_mux_t *m, volatile int *stop);

// Runs the whole thing: open -> process(stop) -> finalize. Returns 0 ok.
int tsr_analyze_file(tsr_mux_t *m, const char *path, volatile int *stop);

// Compute final rates; safe to call twice.
void tsr_finalize(tsr_mux_t *m);

// ---- codec probing (optional, FFmpeg) --------------------------------------
// Keep a bounded payload sample of a probeable ES PID (call from the packet
// path). No-op for non-probeable stream types or once the buffer is full.
void tsr_probe_feed(tsr_stream_t *s, const uint8_t *payload, int paylen);
// Probe all buffered ES samples, filling s->probe_detail. Consumes the
// buffers. Idempotent (safe to call twice).
void tsr_probe_streams(tsr_mux_t *m);

// Feed one already-aligned 188-byte packet (used by tests).
void tsr_process_packet(tsr_mux_t *m, const uint8_t *pkt);

// ---- lookups / helpers -----------------------------------------------------

tsr_program_t *tsr_get_program(tsr_mux_t *m, uint16_t number);
tsr_service_t *tsr_get_service(tsr_mux_t *m, uint16_t service_id);
tsr_stream_t  *tsr_get_stream(tsr_mux_t *m, uint16_t pid);
tsr_program_t *tsr_get_or_create_program(tsr_mux_t *m, uint16_t number);
tsr_service_t *tsr_get_or_create_service(tsr_mux_t *m, uint16_t service_id);
void tsr_extract_reopen(tsr_mux_t *m, tsr_stream_t *s);

double tsr_wall_seconds(const tsr_mux_t *m);
double tsr_total_bitrate_bps(const tsr_mux_t *m);

void tsr_add_event(tsr_mux_t *m, uint16_t service_id, uint16_t program,
                   int atsc, uint16_t event_id, int present, int is_now,
                   int is_next, int64_t start_unix, uint32_t duration_s,
                   const char *name, const char *description,
                   const uint8_t *genre, const char *audio_visual);

const char *tsr_role_name(int role);
const char *tsr_category_name(int category);
const char *tsr_stream_type_name(int stream_type);
// Returns "Video"/"Audio"/"Subtitle"/"Data"/"" based on PMT info.
const char *tsr_stream_kind(const tsr_stream_t *s);

// Format unix seconds as YYYY-MM-DD HH:MM:SS (local time).
void tsr_format_time(char *out, size_t outlen, int64_t unix_s);

// CRC-32 (MPEG-2 transport stream polynomial, no init/final XOR).
uint32_t tsr_crc32(const uint8_t *data, size_t len);

// ---- JSON / text reporting -------------------------------------------------

// Write JSON representation of the mux to f.
void tsr_write_json(const tsr_mux_t *m, FILE *f);
// Write a human-readable report to f.
void tsr_write_report(const tsr_mux_t *m, FILE *f);

#ifdef __cplusplus
}
#endif

#endif // TSREADER_CLI_H
