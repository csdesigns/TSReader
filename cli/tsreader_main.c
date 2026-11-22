// tsreader_main.c — tsreader CLI entry point.
//
// Usage:
//   tsreader list  [options] input
//   tsreader record [options] input -o out.ts
//   tsreader extract [options] input -e prog=N [-e pid=0x100 ...] -o dir/
//   tsreader live  -u addr[:port] [-i iface]      (or -t host:port)
//
// Options:
//   --json            emit machine-readable JSON instead of text
//   --no-epg          skip EIT/ETT parsing (faster)
//   --tables          also dump SI table sections as they arrive (debug)
//   --duration N      stop after N seconds (live) or N seconds of stream time
//   --max-mb N        stop after N megabytes of TS input
//   -u addr[:port]    UDP input (default port 1234)
//   -t host:port      TCP input
//   -H host[:port]/tune-url
//                     HDHomeRun input (default port 5004); the tune URL is
//                     passed to the device unchanged, e.g.
//                     -H 192.168.1.1/auto/v5.1  or  -H 192.168.1.1/atsc://12/12.1
//   -i IFACE          UDP interface (name or address)
//   -e prog=N|pid=0xNNN  elementary stream extraction (repeatable)
//   -o PATH           output file (record) or directory (extract)
//   -q                quiet (errors only)

#include "tsreader_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>

static volatile int g_stop;

static void on_sigint(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void usage(FILE *f)
{
    fprintf(f,
        "tsreader %s — MPEG-TS analyzer (modern CLI port of TSReader)\n"
        "Usage:\n"
        "  tsreader list  [options] input.ts\n"
        "  tsreader record [options] input.ts -o out.ts\n"
        "  tsreader extract [options] input.ts -e prog=N [-e pid=0x100] -o dir/\n"
        "  tsreader live -u addr[:port] [-i iface] | -t host:port\n"
        "  tsreader list -H 192.168.1.1/auto/v5.1 --duration 10\n"
        "  tsreader record -H 192.168.1.1/atsc://12/12.1 --duration 30 -o rec.ts\n"
        "\n"
        "Options:\n"
        "  --json         machine-readable JSON output\n"
        "  --no-epg       skip EPG (EIT/ETT) parsing\n"
        "  --tables       dump SI sections as received (debug)\n"
        "  --duration N   stop after N seconds\n"
        "  --max-mb N     stop after N MiB of input\n"
        "  -u addr[:port] UDP input (default port 1234)\n"
        "  -t host:port   TCP input\n"
        "  -i IFACE       UDP multicast interface\n"
        "  -e sel         extract ES: prog=N or pid=0xNNN (repeatable)\n"
        "  -o PATH        output file (record) or directory (extract)\n"
        "  -q             quiet\n",
        TSR_VERSION_STRING);
}

static int parse_selection(tsr_mux_t *m, const char *sel)
{
    if (strncmp(sel, "prog=", 5) == 0) {
        long n = strtol(sel + 5, NULL, 0);
        if (n < 0 || n > 65535)
            return -1;
        tsr_extract_program(m, (uint16_t)n);
        return 0;
    }
    if (strncmp(sel, "pid=", 4) == 0) {
        long n = strtol(sel + 4, NULL, 0);
        if (n < 0 || n > 8191)
            return -1;
        tsr_extract_pid(m, (uint16_t)n);
        return 0;
    }
    fprintf(stderr, "tsreader: bad -e selection '%s' (use prog=N or pid=0xNNN)\n",
            sel);
    return -1;
}

int main(int argc, char **argv)
{
    const char *command = NULL;
    const char *input = NULL;
    const char *output = NULL;
    const char *udp_addr = NULL, *tcp_addr = NULL, *udp_iface = NULL;
    int udp_port = 1234, tcp_port = 0;
    const char *hdhr_host = NULL, *hdhr_tune = NULL;
    int hdhr_port = 5004;
    int json = 0, quiet = 0, no_epg = 0, tables = 0;
    double duration = 0;
    long max_mb = 0;
    char selections[64][64];
    int n_selections = 0;

    // Command-first parsing.
    if (argc > 1 && (strcmp(argv[1], "list") == 0 ||
                     strcmp(argv[1], "record") == 0 ||
                     strcmp(argv[1], "extract") == 0 ||
                     strcmp(argv[1], "live") == 0 ||
                     strcmp(argv[1], "--help") == 0 ||
                     strcmp(argv[1], "-h") == 0)) {
        if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0) {
            usage(stdout);
            return 0;
        }
        command = argv[1];
        for (int i = 1; i < argc; i++)
            argv[i] = argv[i + 1];
        argc--;
        argv[argc] = NULL;
    }

    static const struct option lopts[] = {
        {"json", no_argument, NULL, 'j'},
        {"no-epg", no_argument, NULL, 'E'},
        {"tables", no_argument, NULL, 'T'},
        {"duration", required_argument, NULL, 'd'},
        {"max-mb", required_argument, NULL, 'b'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "u:t:i:e:o:qH:h", lopts, NULL)) != -1) {
        switch (opt) {
        case 'j': json = 1; break;
        case 'E': no_epg = 1; break;
        case 'T': tables = 1; break;
        case 'd': duration = atof(optarg); break;
        case 'b': max_mb = atol(optarg); break;
        case 'u': {
            udp_addr = optarg;
            const char *cp = strchr(optarg, ':');
            if (cp) {
                udp_addr = optarg; // pass full; port parsed below
                // Re-parse: addr may be [v6]:port; keep simple IPv4 handling.
                char tmp[256];
                snprintf(tmp, sizeof(tmp), "%s", optarg);
                char *colon = strrchr(tmp, ':');
                if (colon && !strchr(colon + 1, ':')) {
                    *colon = 0;
                    udp_addr = tmp;
                    udp_port = atoi(colon + 1);
                }
            }
            break;
        }
        case 't': {
            tcp_addr = optarg;
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "%s", optarg);
            char *colon = strrchr(tmp, ':');
            if (colon) {
                *colon = 0;
                tcp_addr = tmp;
                tcp_port = atoi(colon + 1);
            }
            break;
        }
        case 'i': udp_iface = optarg; break;
        case 'H': {
            // Static buffers: the pointers outlive the option loop and are
            // used later, in tsr_open_hdhr().
            static char urlbuf[512];
            static char hostbuf[256];
            snprintf(urlbuf, sizeof(urlbuf), "%s", optarg);
            if (!strncmp(urlbuf, "http://", 7))
                memmove(urlbuf, urlbuf + 7, strlen(urlbuf) - 6);
            char *slash = strchr(urlbuf, '/');
            if (!slash || !slash[1]) {
                fprintf(stderr,
                        "tsreader: -H needs host[:port]/tune-url "
                        "(e.g. 192.168.1.1/auto/v5.1)\n");
                return 2;
            }
            *slash = 0;
            hdhr_tune = slash + 1;
            snprintf(hostbuf, sizeof(hostbuf), "%s", urlbuf);
            char *colon = strrchr(hostbuf, ':');
            if (colon && !strchr(colon + 1, ':')) {
                *colon = 0;
                hdhr_host = hostbuf;
                hdhr_port = atoi(colon + 1);
            } else {
                hdhr_host = hostbuf;
                hdhr_port = 5004;
            }
            break;
        }
        case 'e':
            if (n_selections < 64)
                snprintf(selections[n_selections++], sizeof(selections[0]),
                         "%s", optarg);
            break;
        case 'o': output = optarg; break;
        case 'q': quiet = 1; break;
        case 'h': usage(stdout); return 0;
        default: usage(stderr); return 2;
        }
    }
    if (optind < argc)
        input = argv[optind];

    if (!command) {
        fprintf(stderr, "tsreader: missing command (list|record|extract|live)\n");
        usage(stderr);
        return 2;
    }
    if (strcmp(command, "live") != 0 && !input && !hdhr_host) {
        fprintf(stderr, "tsreader: missing input file\n");
        return 2;
    }
    if (strcmp(command, "record") == 0 && !output) {
        fprintf(stderr, "tsreader: record requires -o out.ts\n");
        return 2;
    }
    if (strcmp(command, "extract") == 0 && !output) {
        fprintf(stderr, "tsreader: extract requires -o dir/\n");
        return 2;
    }

    signal(SIGINT, on_sigint);
    signal(SIGTERM, on_sigint);

    // The mux state is ~10 MB; keep it off the stack.
    tsr_mux_t *m = calloc(1, sizeof(tsr_mux_t));
    if (!m)
        return 1;
    tsr_init(m);
    for (int i = 0; i < n_selections; i++)
        if (parse_selection(m, selections[i])) {
            tsr_free(m);
            free(m);
            return 2;
        }
    if (no_epg)
        m->ignore_eit = 1;
    if (tables)
        tsr_set_dump_tables(m, 1);
    if (duration > 0)
        tsr_set_max_duration(m, duration);
    if (max_mb > 0)
        tsr_set_max_bytes(m, (uint64_t)max_mb << 20);

    if (strcmp(command, "record") == 0) {
        tsr_set_record(m, output);
        if (!m->record_out) {
            tsr_free(m);
            free(m);
            return 1;
        }
    }
    if (strcmp(command, "extract") == 0)
        tsr_set_extract_dir(m, output);

    int rc = 0;
    if (hdhr_host) {
        rc = tsr_open_hdhr(m, hdhr_host, hdhr_port, hdhr_tune);
        if (!rc && !quiet)
            fprintf(stderr, "tsreader: HDHomeRun %s (Ctrl-C to stop)\n",
                    m->input_desc);
    } else if (strcmp(command, "live") == 0) {
        if (tcp_addr)
            rc = tsr_open_tcp(m, tcp_addr, tcp_port);
        else
            rc = tsr_open_udp(m, udp_addr, udp_port, udp_iface);
        if (!rc && !quiet)
            fprintf(stderr, "tsreader: listening on %s (Ctrl-C to stop)\n",
                    m->input_desc);
    } else {
        rc = tsr_open_file(m, input);
    }
    if (rc) {
        tsr_free(m);
        free(m);
        return 1;
    }

    tsr_process(m, &g_stop);
    tsr_finalize(m);

    FILE *out = stdout;
    if (json)
        tsr_write_json(m, out);
    else
        tsr_write_report(m, out);
    fflush(out);

    tsr_free(m);
    free(m);
    return 0;
}
