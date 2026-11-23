// tsreader_hdhr.c — HDHomeRun (SiC) source: HTTP tune + transport stream.
//
// Two generations of the HDHomeRun HTTP API (default port 5004) are
// handled, tried in this order:
//
// 1. Direct tune-URL streaming (older firmware, and devices/proxies in
//    front of them): the tune URL doubles as the stream URL.
//
//       GET    /<tune-url>           -> raw MPEG-TS byte stream
//
//    The first byte of a live stream is always the TS sync byte 0x47;
//    anything else (an error page, JSON) aborts the attempt. Closing the
//    connection releases the tuner.
//
// 2. Global API (newer firmware):
//
//       POST   /tune                 {"url": "<tune-url>"} -> {"id": N, ...}
//       GET    /transport_stream/N   -> raw MPEG-TS byte stream
//       DELETE /tuner/N              release the tuner
//
// The tune URL is passed through to the device unchanged, so every URL
// form the firmware accepts works:
//
//   auto/v5.1            auto-tune / scan
//   atsc://12/12.1       ATSC virtual / major.minor
//   8vsb:687000000       ATSC by frequency (legacy TSReader style)
//   qam:65               QAM channel
//   dvbs://... / dtv://... / cable://...
//
// The tuner is released when reading stops (duration limit, Ctrl-C or
// device disconnect), so the device is never left with a stuck tuner.
//
// Requires libcurl (compiled in when CMake finds it, TSR_HAVE_CURL).
// Without libcurl, tsr_open_hdhr() reports that this build has no
// HDHomeRun support.

#include "tsreader_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef TSR_HAVE_CURL
#include <curl/curl.h>
#endif

#ifdef TSR_HAVE_CURL

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static size_t discard_cb(char *p, size_t s, size_t n, void *ud);

static int g_curl_init = 0;

static int curl_global_ensure(void)
{
    if (!g_curl_init && curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
        return 1;
    g_curl_init = 1;
    return 0;
}

// Escape a string for embedding in a JSON string literal. The tune URL
// may contain ':', '/', '://' etc. - all legal in JSON; only quotes,
// backslashes and control characters need escaping.
static char *json_escape(const char *s)
{
    size_t n = strlen(s) * 3 + 2, i, o = 0;
    char *b = malloc(n);
    if (!b)
        return NULL;
    b[o++] = '"';
    for (i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '"' || c == '\\')
            b[o++] = '\\';
        if (c < 0x20) {
            o += (size_t)snprintf(b + o, n - o, "\\u%04x", c);
            continue;
        }
        b[o++] = (char)c;
    }
    b[o++] = '"';
    b[o] = 0;
    return b;
}

// Growable body collector for the /tune response.
typedef struct {
    char *buf;
    size_t len, cap;
} resp_buf_t;

static size_t resp_cb(char *p, size_t s, size_t n, void *ud)
{
    resp_buf_t *r = ud;
    size_t add = s * n;
    if (r->len + add + 1 > r->cap) {
        size_t nc = r->cap ? r->cap * 2 : 4096;
        char *nb;
        while (nc < r->len + add + 1)
            nc *= 2;
        nb = realloc(r->buf, nc);
        if (!nb)
            return 0;
        r->buf = nb;
        r->cap = nc;
    }
    memcpy(r->buf + r->len, p, add);
    r->len += add;
    r->buf[r->len] = 0;
    return add;
}

static int hdhr_tune(const char *host, int port, const char *tune_url,
                     int *tuner_id, char *err, size_t errlen)
{
    char url[512];
    char body[1024];
    char *esc = json_escape(tune_url);
    resp_buf_t rb = {0};
    long http_code = 0;
    CURL *e;
    CURLcode code;
    int rc = 1;

    *tuner_id = -1;
    if (!esc)
        return 1;
    snprintf(url, sizeof(url), "http://%s:%d/tune", host, port);
    snprintf(body, sizeof(body), "{\"url\":%s}", esc);
    free(esc);

    e = curl_easy_init();
    if (!e) {
        snprintf(err, errlen, "HDHomeRun: curl_easy_init failed");
        return 1;
    }
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_POST, 1L);
    curl_easy_setopt(e, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, resp_cb);
    curl_easy_setopt(e, CURLOPT_WRITEDATA, &rb);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(e, CURLOPT_TIMEOUT, 60L);  // auto-scan tunes are slow
    curl_easy_setopt(e, CURLOPT_NOPROXY, "*");  // LAN device
    code = curl_easy_perform(e);
    curl_easy_getinfo(e, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(e);

    if (code != CURLE_OK) {
        snprintf(err, errlen, "HDHomeRun tune failed: %s",
                 curl_easy_strerror(code));
        free(rb.buf);
        return 1;
    }
    // Success looks like: {"id":1,"source_url":"...","transport_stream":"..."}
    // rb.buf is NULL when the response carried no body at all.
    const char *resp = rb.buf ? rb.buf : "";
    const char *p = strstr(resp, "\"id\"");
    if (p) {
        p = strchr(p, ':');
        if (p)
            *tuner_id = atoi(p + 1);
    }
    if (http_code != 200 || *tuner_id < 0) {
        snprintf(err, errlen, "HDHomeRun tune rejected (HTTP %ld): %s",
                 http_code, resp[0] ? resp : "(empty response)");
        free(rb.buf);
        return 1;
    }
    free(rb.buf);
    rc = 0;
    return rc;
}

static void hdhr_release(const char *host, int port, int tuner_id)
{
    char url[512];
    CURL *e;
    if (tuner_id < 0)
        return;
    snprintf(url, sizeof(url), "http://%s:%d/tuner/%d", host, port, tuner_id);
    e = curl_easy_init();
    if (!e)
        return;
    curl_easy_setopt(e, CURLOPT_URL, url);
    curl_easy_setopt(e, CURLOPT_CUSTOMREQUEST, "DELETE");
    curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, discard_cb);
    curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 2L);
    curl_easy_setopt(e, CURLOPT_TIMEOUT, 3L);
    curl_easy_setopt(e, CURLOPT_NOPROXY, "*");
    curl_easy_perform(e);  // best effort; a dead device means nothing to release
    curl_easy_cleanup(e);
}

// Transport-stream writer: feed bytes straight into the pipeline and
// abort the transfer when the main thread asks us to stop.
static size_t ts_feed_cb(char *p, size_t s, size_t n, void *ud)
{
    tsr_mux_t *m = ud;
    if (m->stop_flag && *m->stop_flag)
        return 0;
    tsr_feed(m, (const uint8_t *)p, s * n);
    return s * n;
}

static size_t discard_cb(char *p, size_t s, size_t n, void *ud)
{
    (void)p;
    (void)ud;
    return s * n;
}

// Direct tune-URL streaming (GET /<tune-url>): the first data chunk of a
// live transport stream always starts with the TS sync byte 0x47. If it
// doesn't (error page, JSON, ...), abort the transfer and let the caller
// fall back to the global /tune API.
typedef struct {
    tsr_mux_t *m;
    int first;    // no data seen yet
    int got_ts;   // first chunk started with 0x47
} direct_state_t;

static size_t direct_cb(char *p, size_t s, size_t n, void *ud)
{
    direct_state_t *d = ud;
    tsr_mux_t *m = d->m;
    size_t total = s * n;
    if (m->stop_flag && *m->stop_flag)
        return 0;
    if (d->first) {
        d->first = 0;
        if (total == 0 || (unsigned char)p[0] != 0x47) {
            d->got_ts = 0;
            return 0;
        }
        d->got_ts = 1;
    }
    tsr_feed(m, (const uint8_t *)p, total);  // marks stream start on first byte
    return total;
}

#endif // TSR_HAVE_CURL

// ---------------------------------------------------------------------------
// public entry points
// ---------------------------------------------------------------------------

int tsr_open_hdhr(tsr_mux_t *m, const char *host, int port,
                  const char *tune_url)
{
#ifdef TSR_HAVE_CURL
    if (curl_global_ensure()) {
        fprintf(stderr, "tsreader: curl_global_init failed\n");
        return 1;
    }
    if (port <= 0)
        port = 5004;
    snprintf(m->hdhr_host, sizeof(m->hdhr_host), "%s", host);
    m->hdhr_port = port;
    snprintf(m->hdhr_tune, sizeof(m->hdhr_tune), "%s", tune_url);

    // No network I/O here: the reader thread tries the direct tune-URL
    // stream first and falls back to POST /tune (firmware-dependent), so
    // a slow auto-scan does not block the open.
    m->hdhr_tuner_id = -1;
    m->live_kind = 1;
    m->live_reader_fn = tsr_hdhr_reader;
    m->live = 1;
    snprintf(m->input_desc, sizeof(m->input_desc),
             "hdhr://%s:%d/%s", host, port, tune_url);
    return 0;
#else
    (void)m;
    (void)host;
    (void)port;
    (void)tune_url;
    fprintf(stderr,
            "tsreader: built without libcurl - no HDHomeRun support.\n"
            "          Install libcurl (headers+library) and reconfigure.\n");
    return 1;
#endif
}

int tsr_hdhr_reader(tsr_mux_t *m)
{
#ifdef TSR_HAVE_CURL
    char url[1024];
    CURL *e;
    CURLcode code = CURLE_OK;
    int got_ts = 0;
    int tuner_id = -1;

    // The stream is continuous: no overall timeout. Curl aborts by itself
    // if the device goes silent (e.g. signal loss) for 15 s. Duration / byte
    // limits count from the first stream byte (tsr_feed marks the wall
    // clock), so a slow auto-scan does not eat into --duration.

    // Attempt 1: direct tune-URL streaming (older firmware).
    direct_state_t ds = { m, 1, 0 };
    snprintf(url, sizeof(url), "http://%s:%d/%s",
             m->hdhr_host, m->hdhr_port, m->hdhr_tune);
    e = curl_easy_init();
    if (e) {
        curl_easy_setopt(e, CURLOPT_URL, url);
        curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, direct_cb);
        curl_easy_setopt(e, CURLOPT_WRITEDATA, &ds);
        curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(e, CURLOPT_NOPROXY, "*");
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1L);   // 1 byte/s
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, 15L);
        code = curl_easy_perform(e);
        got_ts = ds.got_ts;
        curl_easy_cleanup(e);
        e = NULL;
        if (!got_ts && code != CURLE_COULDNT_CONNECT &&
            code != CURLE_WRITE_ERROR && code != CURLE_OPERATION_TIMEDOUT &&
            code != CURLE_RECV_ERROR && code != CURLE_GOT_NOTHING)
            fprintf(stderr, "tsreader: HDHomeRun direct stream: %s\n",
                    curl_easy_strerror(code));
    }

    // Attempt 2 (fallback): global SiC API (firmware with POST /tune).
    if (!got_ts) {
        if (code == CURLE_COULDNT_CONNECT) {
            fprintf(stderr, "tsreader: HDHomeRun %s:%d not reachable\n",
                    m->hdhr_host, m->hdhr_port);
            m->live_reader_done = 1;
            return 1;
        }
        char err[1024];
        if (hdhr_tune(m->hdhr_host, m->hdhr_port, m->hdhr_tune,
                      &tuner_id, err, sizeof(err))) {
            fprintf(stderr, "tsreader: %s\n", err);
            m->live_reader_done = 1;
            return 1;
        }
        m->hdhr_tuner_id = tuner_id;
        snprintf(url, sizeof(url), "http://%s:%d/transport_stream/%d",
                 m->hdhr_host, m->hdhr_port, tuner_id);
        e = curl_easy_init();
        if (!e) {
            fprintf(stderr, "tsreader: HDHomeRun: curl_easy_init failed\n");
            m->live_reader_done = 1;
            return 1;
        }
        curl_easy_setopt(e, CURLOPT_URL, url);
        curl_easy_setopt(e, CURLOPT_WRITEFUNCTION, ts_feed_cb);
        curl_easy_setopt(e, CURLOPT_WRITEDATA, m);
        curl_easy_setopt(e, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(e, CURLOPT_NOPROXY, "*");
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_LIMIT, 1L);   // 1 byte/s
        curl_easy_setopt(e, CURLOPT_LOW_SPEED_TIME, 15L);
        code = curl_easy_perform(e);
        curl_easy_cleanup(e);
        // Closing the stream releases the tuner (direct mode); otherwise
        // release it explicitly. Best effort: a dead device needs nothing.
        hdhr_release(m->hdhr_host, m->hdhr_port, tuner_id);
        m->hdhr_tuner_id = -1;
        m->live_reader_done = 1;

        if (code != CURLE_OK && code != CURLE_WRITE_ERROR &&
            code != CURLE_OPERATION_TIMEDOUT && code != CURLE_RECV_ERROR &&
            code != CURLE_GOT_NOTHING)
            fprintf(stderr, "tsreader: HDHomeRun transport ended: %s\n",
                    curl_easy_strerror(code));
        return 0;
    }

    // Direct stream ran; closing its connection releases the tuner.
    m->live_reader_done = 1;
    if (code != CURLE_OK && code != CURLE_WRITE_ERROR &&
        code != CURLE_OPERATION_TIMEDOUT && code != CURLE_RECV_ERROR &&
        code != CURLE_GOT_NOTHING)
        fprintf(stderr, "tsreader: HDHomeRun transport ended: %s\n",
                curl_easy_strerror(code));
    return 0;
#else
    (void)m;
    fprintf(stderr, "tsreader: built without libcurl - no HDHomeRun support\n");
    m->live_reader_done = 1;
    return 1;
#endif
}
