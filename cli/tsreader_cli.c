// tsreader_cli.c — Core pipeline: input, realignment, packet processing,
// stats, PCR tracking, recording and elementary-stream extraction.
//
// SI/SI parsing lives in tsreader_si.c and is called from here.

#include "tsreader_cli.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <net/if.h>
#include <pthread.h>
#include <sys/stat.h>

// Implemented in tsreader_si.c
void tsr_si_init(tsr_mux_t *m);
void tsr_si_free(tsr_mux_t *m);
void tsr_si_packet(tsr_mux_t *m, const uint8_t *pkt);
void tsr_si_print_header(tsr_mux_t *m, FILE *f);

static double now_secs(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

static int live_reader(void *arg);

void tsr_init(tsr_mux_t *m)
{
    memset(m, 0, sizeof(*m));
    m->packet_len = 188;
    m->sock = -1;
    m->n_extract_programs = 0;
    m->n_extract_pids = 0;
    for (int i = 0; i < TSR_MAX_PIDS; i++)
        m->streams[i].pid = (uint16_t)i;
    tsr_si_init(m);
}

void tsr_free(tsr_mux_t *m)
{
    if (m->file)
        fclose(m->file), m->file = NULL;
    if (m->sock >= 0)
        close(m->sock), m->sock = -1;
    if (m->record_out)
        fclose(m->record_out), m->record_out = NULL;
    for (int i = 0; i < TSR_MAX_PIDS; i++) {
        if (m->streams[i].out)
            fclose(m->streams[i].out), m->streams[i].out = NULL;
        free(m->streams[i].prebuf);
        m->streams[i].prebuf = NULL;
        free(m->streams[i].probe_buf);
        m->streams[i].probe_buf = NULL;
        m->streams[i].probe_len = 0;
    }
    free(m->events);
    m->events = NULL;
    tsr_si_free(m);
    m->closed = 1;
}

// ---- input -----------------------------------------------------------------

int tsr_open_file(tsr_mux_t *m, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "tsreader: cannot open '%s': %s\n", path, strerror(errno));
        return -1;
    }
    m->file = f;
    snprintf(m->file_path, sizeof(m->file_path), "%s", path);
    if (fseeko(f, 0, SEEK_END) == 0) {
        off_t sz = ftello(f);
        m->file_size = sz;
        m->file_size_valid = (sz >= 0);
    }
    rewind(f);
    snprintf(m->input_desc, sizeof(m->input_desc), "%s", path);
    return 0;
}

int tsr_open_udp(tsr_mux_t *m, const char *addr, int port, const char *iface)
{
    // For multicast: join the group on the chosen interface (or default),
    // then bind INADDR_ANY:port. For unicast: bind the given address:port.
    char pb[16];
    snprintf(pb, sizeof(pb), "%d", port);

    int is_multicast = 0;
    int family = AF_INET;
    struct in_addr group4 = {0};
    struct in6_addr group6 = IN6ADDR_ANY_INIT;
    struct addrinfo hints, *list = NULL;

    if (addr && *addr) {
        if (inet_pton(AF_INET, addr, &group4) == 1)
            is_multicast = IN_MULTICAST(ntohl(group4.s_addr));
        else if (inet_pton(AF_INET6, addr, &group6) == 1) {
            family = AF_INET6;
            is_multicast = IN6_IS_ADDR_MULTICAST(&group6);
        } else {
            // Hostname for unicast: resolve.
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_DGRAM;
            if (getaddrinfo(addr, pb, &hints, &list) != 0 || !list) {
                fprintf(stderr, "tsreader: cannot resolve UDP address '%s'\n",
                        addr);
                return -1;
            }
            group4.s_addr = 0;
        }
    } else {
        group4.s_addr = htonl(INADDR_ANY);
    }

    int s = socket(family, SOCK_DGRAM, 0);
    if (s < 0) {
        fprintf(stderr, "tsreader: socket: %s\n", strerror(errno));
        if (list)
            freeaddrinfo(list);
        return -1;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    int buf = 8 * 1024 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, &buf, sizeof(buf));

    struct sockaddr_in bind4;
    struct sockaddr_in6 bind6;
    socklen_t blen;
    if (family == AF_INET6) {
        memset(&bind6, 0, sizeof(bind6));
        bind6.sin6_family = AF_INET6;
        bind6.sin6_addr = in6addr_any;
        bind6.sin6_port = htons((uint16_t)port);
        blen = sizeof(bind6);
        if (is_multicast) {
            struct ipv6_mreq mreq;
            memset(&mreq, 0, sizeof(mreq));
#ifdef __APPLE__
            mreq.ipv6mr_multiaddr = group6;
            if (iface && *iface) {
                int ifi = if_nametoindex(iface);
                if (ifi)
                    mreq.ipv6mr_interface = (uint32_t)ifi;
            }
#else
            mreq.imr_multiaddr = group6;
            if (iface && *iface) {
                int ifi = if_nametoindex(iface);
                if (ifi)
                    mreq.imr_interface = (uint32_t)ifi;
            }
#endif
            setsockopt(s, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq));
        }
    } else {
        memset(&bind4, 0, sizeof(bind4));
        bind4.sin_family = AF_INET;
        bind4.sin_addr.s_addr =
            is_multicast ? htonl(INADDR_ANY) : group4.s_addr;
        bind4.sin_port = htons((uint16_t)port);
        blen = sizeof(bind4);
        if (is_multicast) {
            struct ip_mreq mreq;
            memset(&mreq, 0, sizeof(mreq));
            mreq.imr_multiaddr = group4;
            mreq.imr_interface.s_addr =
                (iface && *iface && strchr(iface, '.'))
                    ? inet_addr(iface)
                    : htonl(INADDR_ANY);
            setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));
            int loop = 1;
            setsockopt(s, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
        }
    }

    if (bind(s, family == AF_INET6 ? (struct sockaddr *)&bind6
                                    : (struct sockaddr *)&bind4,
             blen) != 0) {
        fprintf(stderr, "tsreader: bind udp %s:%d: %s\n",
                addr ? addr : "*", port, strerror(errno));
        close(s);
        if (list)
            freeaddrinfo(list);
        return -1;
    }
    if (list)
        freeaddrinfo(list);

    m->sock = s;
    m->live = 1;
    if (iface)
        snprintf(m->udp_iface, sizeof(m->udp_iface), "%s", iface);
    snprintf(m->input_desc, sizeof(m->input_desc),
             "udp://%s:%d%s%s", addr ? addr : "*", port,
             (iface && *iface) ? "?" : "", (iface && *iface) ? iface : "");
    return 0;
}

int tsr_open_tcp(tsr_mux_t *m, const char *host, int port)
{
    struct addrinfo hints, *list = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char pb[16];
    snprintf(pb, sizeof(pb), "%d", port);
    if (getaddrinfo(host, pb, &hints, &list) != 0 || !list) {
        fprintf(stderr, "tsreader: cannot resolve '%s'\n", host);
        return -1;
    }
    int s = -1;
    for (struct addrinfo *ai = list; ai; ai = ai->ai_next) {
        s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s < 0)
            continue;
        if (connect(s, ai->ai_addr, ai->ai_addrlen) == 0)
            break;
        close(s);
        s = -1;
    }
    freeaddrinfo(list);
    if (s < 0) {
        fprintf(stderr, "tsreader: cannot connect TCP %s:%d\n", host, port);
        return -1;
    }
    m->sock = s;
    m->live = 1;
    snprintf(m->input_desc, sizeof(m->input_desc), "tcp://%s:%d", host, port);
    return 0;
}

// ---- realignment + feed ----------------------------------------------------

int tsr_feed(tsr_mux_t *m, const uint8_t *data, size_t len)
{
    if (!m->wall_valid) {
        m->wall_start = now_secs();
        m->wall_valid = 1;
    }

    // Consume the input in chunks that fit the carry buffer, processing
    // aligned packets as they become available.
    size_t off = 0;
    while (off < len || m->carry_len) {
        if (off < len) {
            size_t space = sizeof(m->carry) - m->carry_len;
            size_t take = len - off < space ? len - off : space;
            memcpy(m->carry + m->carry_len, data + off, take);
            m->carry_len += take;
            off += take;
        }

        size_t i = 0;
        // Skip until first sync byte.
        while (i < m->carry_len && m->carry[i] != 0x47)
            i++;
        if (i > 0) {
            m->discarded_packets +=
                (unsigned)(i + m->packet_len - 1) / m->packet_len;
            m->sync_errors++;
        }

        // Process aligned packets.
        while (i + (size_t)m->packet_len <= m->carry_len) {
            const uint8_t *pkt = m->carry + i;
            tsr_process_packet(m, pkt);
            i += m->packet_len;
        }

        // Compact remainder.
        m->carry_len -= i;
        if (m->carry_len)
            memmove(m->carry, m->carry + i, m->carry_len);

        if (off >= len)
            break;
        // Carry buffer still full and no packet found: pure garbage.
        if (m->carry_len == sizeof(m->carry)) {
            m->carry_len = 0;
            m->sync_errors++;
        }
    }
    return 0;
}

void tsr_finalize(tsr_mux_t *m)
{
    // Consume buffered ES samples and fill codec detail (idempotent).
    tsr_probe_streams(m);
    if (m->wall_valid) {
        if (!m->wall_end)
            m->wall_end = now_secs();
    }
    double wall = m->wall_end - m->wall_start;
    if (wall <= 0.0)
        wall = 1e-9;

    // Per-program PCR-based bitrate.
    for (int p = 0; p < TSR_MAX_PROGRAMS; p++) {
        tsr_program_t *pr = &m->programs[p];
        if (!pr->in_use)
            continue;
        uint64_t dt = 0;
        if (pr->pcr_last_ticks > pr->pcr_first_ticks)
            dt = pr->pcr_last_ticks - pr->pcr_first_ticks;
        if (dt > 0) {
            double secs = (double)dt / 90000.0;
            // Sum ES bytes of this program.
            uint64_t b = 0;
            for (int i = 0; i < TSR_MAX_PIDS; i++) {
                tsr_stream_t *s = &m->streams[i];
                if (s->in_use && s->program_number == pr->program_number &&
                    (s->role == TSR_ROLE_ES || s->role == TSR_ROLE_AC3_ES))
                    b += s->bytes;
            }
            pr->bitrate_bps = (double)b / secs;
        } else {
            pr->bitrate_bps = 0.0;
        }
    }
    // Total wall rate.
    // (stored nowhere; report computes it)

    // Fallback program<->service association: DVB has no explicit link,
    // so if program count equals service count, pair them in ascending
    // order (only fill programs that still have no name).
    int np = 0, ns = 0;
    tsr_program_t *plist[TSR_MAX_PROGRAMS];
    tsr_service_t *slist[TSR_MAX_SERVICES];
    for (int i = 0; i < TSR_MAX_PROGRAMS; i++)
        if (m->programs[i].in_use && m->programs[i].pmt_pid)
            plist[np++] = &m->programs[i];
    for (int i = 0; i < TSR_MAX_SERVICES; i++)
        if (m->services[i].in_use)
            slist[ns++] = &m->services[i];
    if (np == ns && np > 0) {
        for (int i = 0; i < np; i++) {
            tsr_program_t *pr = plist[i];
            tsr_service_t *sv = slist[i];
            if (!pr->service_id && (sv->name[0] || sv->has_number)) {
                pr->service_id = sv->service_id;
                if (!pr->name[0])
                    snprintf(pr->name, sizeof(pr->name), "%s", sv->name);
                if (!pr->provider[0])
                    snprintf(pr->provider, sizeof(pr->provider), "%s",
                             sv->provider);
            }
        }
    }
}

double tsr_wall_seconds(const tsr_mux_t *m)
{
    return (m->wall_end && m->wall_start) ? (m->wall_end - m->wall_start) : 0.0;
}

double tsr_total_bitrate_bps(const tsr_mux_t *m)
{
    double w = tsr_wall_seconds(m);
    if (w <= 0)
        w = 1.0;
    return (double)m->total_bytes / w * 8.0;
}

// ---- packet processing -----------------------------------------------------

static void tsr_extract_init_stream(tsr_mux_t *m, tsr_stream_t *s);

void tsr_process_packet(tsr_mux_t *m, const uint8_t *pkt)
{
    if (pkt[0] != 0x47) {
        m->sync_errors++;
        return;
    }
    uint16_t pid = ((uint16_t)pkt[1] << 8 | pkt[2]) & 0x1fff;
    int cc = pkt[3] & 0xf;
    // adaptation_field_control is bits 5:4 of the 3rd header byte (0x30
    // mask), per ISO/IEC 13818-1: 01 = payload only, 10 = adaptation
    // field only, 11 = AF + payload, 00 = reserved.
    int afc = (pkt[3] & 0x30) >> 4;
    const uint8_t *payload = NULL;
    int paylen = 0;
    if (afc == 1) {
        payload = pkt + 4;
        paylen = m->packet_len - 4;
    } else if (afc == 3) {
        int af_len = pkt[4];
        if (5 + af_len <= m->packet_len) {
            payload = pkt + 5 + af_len;
            paylen = m->packet_len - 5 - af_len;
        }
    } else if (afc == 0) {
        // Reserved value: strictly there is no payload and no AF. Count it
        // for the report (tstools' tsinfo flags these the same way).
        m->streams[((uint16_t)pkt[1] << 8 | pkt[2]) & 0x1fff].reserved_af++;
    } // afc == 2 (adaptation field only): no payload, nothing else to do.

    m->total_packets++;
    m->total_bytes += m->packet_len;

    tsr_stream_t *s = &m->streams[pid];
    s->in_use = 1;
    s->packets++;
    s->bytes += m->packet_len;

    // Continuity check (only for packets with payload).
    if (paylen > 0) {
        if (s->cc_valid && (unsigned)cc != ((s->last_cc + 1u) & 0xfu))
            s->continuity_errors++;
        s->last_cc = (uint32_t)cc;
        s->cc_valid = 1;
    }

    // PCR: AF flags byte, then the 6-byte PCR field if present.
    // base(33) = 4 bytes + MSB of byte 5; post(9) = low 7 bits + byte 6.
    if (afc >= 2) {
        uint8_t af_flags = pkt[5];
        if ((af_flags & 0x10) && pkt[4] >= 7) {
            uint64_t base = ((uint64_t)pkt[6] << 24 | (uint64_t)pkt[7] << 16 |
                             (uint64_t)pkt[8] << 8 | pkt[9]) << 1 |
                            ((uint64_t)pkt[10] >> 7);
            uint64_t ext = ((uint64_t)(pkt[10] & 0x7f) << 8) | pkt[11];
            uint64_t ticks = (base << 9) | ext;
            s->pcr = 1;
            // Find owning program (known via PMT) or default.
            tsr_program_t *pr = NULL;
            if (s->program_number)
                pr = tsr_get_program(m, s->program_number);
            if (!pr) {
                for (int p = 0; p < TSR_MAX_PROGRAMS; p++) {
                    if (m->programs[p].in_use &&
                        m->programs[p].pcr_pid == pid) {
                        pr = &m->programs[p];
                        break;
                    }
                }
            }
            if (pr) {
                if (!pr->pcr_ticks_valid) {
                    pr->pcr_first_ticks = ticks;
                    pr->pcr_ticks_valid = 1;
                }
                pr->pcr_last_ticks = ticks;
            }
        }
    }

    // Recording.
    if (m->record_out) {
        fwrite(pkt, m->packet_len, 1, m->record_out);
        m->record_bytes += m->packet_len;
    }

    // PSI/SI.
    tsr_si_packet(m, pkt);

    // Keep a bounded ES sample for codec probing (no-op for SI/unknown).
    tsr_probe_feed(s, payload, paylen);

    // Extraction: stream role may only be known after the PMT is parsed,
    // so retry opening on every packet until it succeeds. Payloads that
    // arrive before the output opens are pre-buffered (bounded) so the
    // first access unit is not lost, then flushed on open.
    if (paylen > 0 && (m->extract_dir[0] || m->n_extract_pids)) {
        if (s->out) {
            s->out_bytes += fwrite(payload, 1, paylen, s->out);
        } else {
            if (!m->pmt_seen && s->role == TSR_ROLE_NONE) {
                if (!s->prebuf)
                    s->prebuf = malloc(TSR_EXTRACT_PREBUF_MAX);
                if (s->prebuf &&
                    s->prebuf_len + (size_t)paylen <= TSR_EXTRACT_PREBUF_MAX)
                {
                    memcpy(s->prebuf + s->prebuf_len, payload, paylen);
                    s->prebuf_len += (size_t)paylen;
                } else {
                    free(s->prebuf); // overflow or OOM: stop buffering
                    s->prebuf = NULL;
                    s->prebuf_len = 0;
                }
            }
            tsr_extract_init_stream(m, s);
            if (s->out && s->prebuf) {
                s->out_bytes += fwrite(s->prebuf, 1, s->prebuf_len, s->out);
                free(s->prebuf);
                s->prebuf = NULL;
                s->prebuf_len = 0;
            }
        }
    }
}

static void tsr_extract_init_stream(tsr_mux_t *m, tsr_stream_t *s)
{
    if (!m->extract_dir[0] || s->out)
        return;
    int want_pid = 0, want_prog = 0;
    for (int i = 0; i < m->n_extract_pids; i++)
        if ((uint16_t)m->extract_pids[i] == s->pid)
            want_pid = 1;
    for (int i = 0; i < m->n_extract_programs; i++)
        if (s->program_number && s->program_number == (uint16_t)m->extract_programs[i])
            want_prog = 1;
    if (!want_pid && !want_prog)
        return;
    // Program selection: elementary streams only (PMT-known). An explicit
    // PID selection may pull any non-SI source.
    int es_role = (s->role == TSR_ROLE_ES || s->role == TSR_ROLE_AC3_ES);
    if (want_prog && !es_role)
        return;
    if (want_pid && s->category == TSR_CAT_SI)
        return;
    char fn[600];
    char cname[80];
    const char *nm = tsr_stream_type_name(s->stream_type);
    for (size_t i = 0; i < sizeof(cname) - 1 && nm[i]; i++)
        cname[i] = (nm[i] == '/' || nm[i] == ' ' || nm[i] == '(' ||
                    nm[i] == ')') ? '_' : nm[i];
    cname[sizeof(cname) - 1] = 0;
    snprintf(fn, sizeof(fn), "%s/extract_%s_%03X.es", m->extract_dir, cname,
             s->pid);
    s->out = fopen(fn, "wb");
    if (s->out)
        snprintf(s->out_path, sizeof(s->out_path), "%s", fn);
}

// Exposed for tsreader_si.c: (re)open extraction once a PMT assigns a
// stream to a selected program.
void tsr_extract_reopen(tsr_mux_t *m, tsr_stream_t *s)
{
    tsr_extract_init_stream(m, s);
}

// ---- lookups ----------------------------------------------------------------

tsr_program_t *tsr_get_program(tsr_mux_t *m, uint16_t number)
{
    for (int i = 0; i < TSR_MAX_PROGRAMS; i++)
        if (m->programs[i].in_use && m->programs[i].program_number == number)
            return &m->programs[i];
    return NULL;
}

tsr_service_t *tsr_get_service(tsr_mux_t *m, uint16_t service_id)
{
    for (int i = 0; i < TSR_MAX_SERVICES; i++)
        if (m->services[i].in_use && m->services[i].service_id == service_id)
            return &m->services[i];
    return NULL;
}

tsr_stream_t *tsr_get_stream(tsr_mux_t *m, uint16_t pid)
{
    if (pid >= TSR_MAX_PIDS)
        return NULL;
    return &m->streams[pid];
}

// ---- time -------------------------------------------------------------------

void tsr_format_time(char *out, size_t outlen, int64_t unix_s)
{
    if (outlen == 0)
        return;
    if (unix_s <= 0) {
        snprintf(out, outlen, "n/a");
        return;
    }
    time_t t = (time_t)unix_s;
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(out, outlen, "%Y-%m-%d %H:%M:%S", &tmv);
}

// ---- options ----------------------------------------------------------------

void tsr_set_record(tsr_mux_t *m, const char *path)
{
    if (!path)
        return;
    snprintf(m->record_path, sizeof(m->record_path), "%s", path);
    m->record_all = 1;
    m->record_out = fopen(path, "wb");
    if (!m->record_out)
        fprintf(stderr, "tsreader: cannot open '%s' for recording: %s\n",
                path, strerror(errno));
}

// Create a directory path (components included); existing dirs are fine.
static void tsr_mkdirs(const char *path)
{
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    if (len && tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                fprintf(stderr, "tsreader: mkdir '%s': %s\n", tmp,
                        strerror(errno));
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        fprintf(stderr, "tsreader: mkdir '%s': %s\n", tmp, strerror(errno));
}

void tsr_set_extract_dir(tsr_mux_t *m, const char *dir)
{
    if (!dir)
        return;
    snprintf(m->extract_dir, sizeof(m->extract_dir), "%s", dir);
    tsr_mkdirs(m->extract_dir);
}

void tsr_extract_program(tsr_mux_t *m, uint16_t number)
{
    if (m->n_extract_programs < 64)
        m->extract_programs[m->n_extract_programs++] = number;
}

void tsr_extract_pid(tsr_mux_t *m, uint16_t pid)
{
    if (m->n_extract_pids < 64)
        m->extract_pids[m->n_extract_pids++] = pid;
}

void tsr_set_max_duration(tsr_mux_t *m, double seconds) { m->max_duration = seconds; }
void tsr_set_max_bytes(tsr_mux_t *m, uint64_t bytes) { m->max_bytes = bytes; }
void tsr_set_dump_tables(tsr_mux_t *m, int enable) { m->dump_tables = enable; }

// ---- process loop -----------------------------------------------------------

static int live_reader(void *arg)
{
    tsr_mux_t *m = arg;
    uint8_t buf[65536];
    volatile int *stop = (volatile int *)m->stop_flag;
    for (;;) {
        if (stop && *stop)
            break;
        ssize_t n = recv(m->sock, buf, sizeof(buf), 0);
        if (n <= 0) {
            if (n == 0)
                break;
            if (errno == EINTR)
                continue;
            break;
        }
        tsr_feed(m, buf, (size_t)n);
        if ((m->max_duration &&
             (now_secs() - m->wall_start) >= m->max_duration) ||
            (m->max_bytes && m->total_bytes >= m->max_bytes))
            break;
    }
    return 0;
}

int tsr_process(tsr_mux_t *m, volatile int *stop)
{
    if (m->live) {
        pthread_t th;
        m->stop_flag = stop;
        if (pthread_create(&th, NULL, (void *(*)(void *))live_reader, m) != 0) {
            // Fallback: run inline.
            live_reader(m);
        } else {
            while ((!stop || !*stop) &&
                   !((m->max_duration && (now_secs() - m->wall_start) >= m->max_duration) ||
                     (m->max_bytes && m->total_bytes >= m->max_bytes))) {
                struct timespec ts = {0, 100 * 1000 * 1000};
                nanosleep(&ts, NULL);
            }
            if (stop)
                *stop = 1;
            pthread_join(th, NULL);
        }
    } else if (m->file) {
        uint8_t buf[1 << 20];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), m->file)) > 0) {
            tsr_feed(m, buf, n);
            if ((m->max_duration && (now_secs() - m->wall_start) >= m->max_duration) ||
                (m->max_bytes && m->total_bytes >= m->max_bytes))
                break;
        }
    }
    m->wall_end = now_secs();
    tsr_finalize(m);
    return 0;
}

int tsr_analyze_file(tsr_mux_t *m, const char *path, volatile int *stop)
{
    int rc = tsr_open_file(m, path);
    if (rc)
        return rc;
    rc = tsr_process(m, stop);
    return rc;
}
