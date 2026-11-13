// tsreader_report.c — JSON and human-readable reporting for the TSReader
// CLI data model.

#include "tsreader_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <time.h>

// ---- small formatting helpers -------------------------------------------------

static void fmt_bitrate(char *out, size_t n, double bps)
{
    if (bps >= 1e6)
        snprintf(out, n, "%.2f Mb/s", bps / 1e6);
    else if (bps >= 1e3)
        snprintf(out, n, "%.1f kb/s", bps / 1e3);
    else
        snprintf(out, n, "%.0f b/s", bps);
}

static void fmt_dur(char *out, size_t n, double secs)
{
    if (secs <= 0) {
        snprintf(out, n, "0 s");
        return;
    }
    if (secs >= 3600)
        snprintf(out, n, "%d:%02d:%02d", (int)(secs / 3600),
                 (int)(secs / 60) % 60, (int)secs % 60);
    else
        snprintf(out, n, "%.1f s", secs);
}

// PCR-based duration if available, else wall duration.
static double mux_duration_s(const tsr_mux_t *m, int *from_pcr)
{
    double best = 0;
    for (int i = 0; i < TSR_MAX_PROGRAMS; i++) {
        const tsr_program_t *pr = &m->programs[i];
        if (!pr->in_use)
            continue;
        if (pr->pcr_last_ticks > pr->pcr_first_ticks) {
            double d = (double)(pr->pcr_last_ticks - pr->pcr_first_ticks)
                       / 90000.0;
            if (d > best)
                best = d;
        }
    }
    if (best > 0) {
        if (from_pcr)
            *from_pcr = 1;
        return best;
    }
    if (from_pcr)
        *from_pcr = 0;
    double w = (m->wall_end && m->wall_start) ? (m->wall_end - m->wall_start)
                                              : 0.0;
    return w;
}

static double mux_bitrate_bps(const tsr_mux_t *m, int *from_pcr)
{
    double d = mux_duration_s(m, from_pcr);
    if (d <= 0)
        d = 1.0;
    return (double)m->total_bytes / d * 8.0;
}

static int count_programs(const tsr_mux_t *m)
{
    int n = 0;
    for (int i = 0; i < TSR_MAX_PROGRAMS; i++)
        if (m->programs[i].in_use && m->programs[i].pmt_pid)
            n++;
    return n;
}

static int count_pids(const tsr_mux_t *m)
{
    int n = 0;
    for (int i = 0; i < TSR_MAX_PIDS; i++)
        if (m->streams[i].in_use)
            n++;
    return n;
}

// ---- JSON -----------------------------------------------------------------------

static void json_escape(FILE *f, const char *s)
{
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (*p < 0x20)
                fprintf(f, "\\u%04x", *p);
            else
                fputc(*p, f);
        }
    }
}

static void json_str(FILE *f, const char *s)
{
    fputc('"', f);
    json_escape(f, s ? s : "");
    fputc('"', f);
}

void tsr_write_json(const tsr_mux_t *m, FILE *f)
{
    fprintf(f, "{\n");
    fprintf(f, "  \"tool\": \"tsreader-cli\", ");
    fprintf(f, "\"version\": \"%s\",\n", TSR_VERSION_STRING);
    fprintf(f, "  \"input\": ");
    json_str(f, m->input_desc);
    fprintf(f, ",\n");
    fprintf(f, "  \"mux\": {\n");
    if (m->network_name[0]) {
        fprintf(f, "    \"network\": ");
        json_str(f, m->network_name);
        fprintf(f, ",\n");
    }
    int from_pcr = 0;
    fprintf(f, "    \"duration_s\": %.3f, \"duration_basis\": %s,\n",
            mux_duration_s(m, &from_pcr),
            from_pcr ? "\"pcr\"" : "\"wall\"");
    fprintf(f, "    \"bitrate_bps\": %.0f,\n", mux_bitrate_bps(m, NULL));
    fprintf(f, "    \"packets\": %" PRIu64 ",\n", m->total_packets);
    fprintf(f, "    \"bytes\": %" PRIu64 ",\n", m->total_bytes);
    fprintf(f, "    \"sync_errors\": %" PRIu32 ",\n", m->sync_errors);
    uint32_t ce = 0, raf = 0;
    for (int i = 0; i < TSR_MAX_PIDS; i++) {
        ce += m->streams[i].continuity_errors;
        raf += m->streams[i].reserved_af;
    }
    fprintf(f, "    \"continuity_errors\": %" PRIu32 ",\n", ce);
    if (raf)
        fprintf(f, "    \"reserved_af_packets\": %" PRIu32 ",\n", raf);
    if (m->tdt_unix) {
        fprintf(f, "    \"stream_time_unix\": %" PRId64 ",\n", m->tdt_unix);
    }
    fprintf(f, "    \"programs\": %d, \"pids\": %d\n", count_programs(m),
            count_pids(m));
    fprintf(f, "  },\n");

    // services
    fprintf(f, "  \"services\": [");
    int first = 1;
    for (int i = 0; i < TSR_MAX_SERVICES; i++) {
        const tsr_service_t *sv = &m->services[i];
        if (!sv->in_use)
            continue;
        fprintf(f, "%s\n    {\"service_id\": %u", first ? "" : ",", sv->service_id);
        fprintf(f, ", \"name\": ");
        json_str(f, sv->name);
        fprintf(f, ", \"provider\": ");
        json_str(f, sv->provider);
        if (sv->has_number)
            fprintf(f, ", \"channel\": %u", sv->logical_number);
        fprintf(f, ", \"eits_pid\": %u}", sv->eits_pid);
        first = 0;
    }
    fprintf(f, "%s],\n", first ? "" : "\n  ");

    // programs
    fprintf(f, "  \"programs\": [");
    first = 1;
    for (int i = 0; i < TSR_MAX_PROGRAMS; i++) {
        const tsr_program_t *pr = &m->programs[i];
        if (!pr->in_use || !pr->pmt_pid)
            continue;
        fprintf(f, "%s\n    {\"program\": %u, \"name\": ", first ? "" : ",",
                pr->program_number);
        json_str(f, pr->name);
        fprintf(f, ", \"pmt_pid\": %u, \"pcr_pid\": %u", pr->pmt_pid,
                pr->pcr_pid);
        if (pr->service_id)
            fprintf(f, ", \"service_id\": %u", pr->service_id);
        if (pr->has_atsc)
            fprintf(f, ", \"atsc\": \"%u.%u\"", pr->atsc_major,
                    pr->atsc_minor);
        fprintf(f, ", \"bitrate_bps\": %.0f, \"streams\": [", pr->bitrate_bps);
        int sf = 1;
        for (int p = 0; p < TSR_MAX_PIDS; p++) {
            const tsr_stream_t *s = &m->streams[p];
            if (!s->in_use || s->program_number != pr->program_number)
                continue;
            if (s->role != TSR_ROLE_ES && s->role != TSR_ROLE_AC3_ES)
                continue;
            fprintf(f, "%s{\"pid\": %u, \"type\": ", sf ? "" : ",", s->pid);
            json_str(f, tsr_stream_type_name(s->stream_type));
            fprintf(f, ", \"codec\": ");
            json_str(f, s->codec);
            fprintf(f, ", \"category\": ");
            json_str(f, tsr_category_name(s->category));
            if (s->language[0]) {
                fprintf(f, ", \"language\": ");
                json_str(f, s->language);
            }
            if (s->component_tag[0]) {
                fprintf(f, ", \"component\": ");
                json_str(f, s->component_tag);
            }
            if (s->probe_detail[0]) {
                fprintf(f, ", \"codec_detail\": ");
                json_str(f, s->probe_detail);
            }
            fprintf(f, ", \"packets\": %" PRIu32 ", \"bytes\": %" PRIu64,
                    s->packets, s->bytes);
            if (s->out_path[0]) {
                fprintf(f, ", \"extracted_to\": ");
                json_str(f, s->out_path);
            }
            fprintf(f, "}");
            sf = 0;
        }
        fprintf(f, "]}");
        first = 0;
    }
    fprintf(f, "%s],\n", first ? "" : "\n  ");

    // EPG
    fprintf(f, "  \"events\": [");
    first = 1;
    for (size_t i = 0; i < m->n_events; i++) {
        const tsr_event_t *e = &m->events[i];
        if (!e->name[0] && !e->description[0])
            continue;
        fprintf(f, "%s\n    {\"service_id\": %u", first ? "" : ",",
                e->service_id);
        if (e->atsc && e->program_number)
            fprintf(f, ", \"program\": %u", e->program_number);
        fprintf(f, ", \"event_id\": %u, \"present\": %s", e->event_id,
                e->present ? "true" : "false");
        if (e->is_now)
            fprintf(f, ", \"now\": true");
        if (e->is_next)
            fprintf(f, ", \"next\": true");
        if (e->start_unix)
            fprintf(f, ", \"start_unix\": %" PRId64, e->start_unix);
        if (e->duration_s)
            fprintf(f, ", \"duration_s\": %" PRIu32, e->duration_s);
        fprintf(f, ", \"name\": ");
        json_str(f, e->name);
        if (e->description[0]) {
            fprintf(f, ", \"description\": ");
            json_str(f, e->description);
        }
        fprintf(f, "}");
        first = 0;
    }
    fprintf(f, "%s],\n", first ? "" : "\n  ");

    // all PIDs (stats)
    fprintf(f, "  \"pids\": [");
    first = 1;
    for (int i = 0; i < TSR_MAX_PIDS; i++) {
        const tsr_stream_t *s = &m->streams[i];
        if (!s->in_use)
            continue;
        fprintf(f, "%s\n    {\"pid\": %u, \"role\": ", first ? "" : ",",
                s->pid);
        json_str(f, tsr_role_name(s->role));
        fprintf(f, ", \"packets\": %" PRIu32, s->packets);
        fprintf(f, ", \"bytes\": %" PRIu64, s->bytes);
        if (s->continuity_errors)
            fprintf(f, ", \"continuity_errors\": %" PRIu32,
                    s->continuity_errors);
        if (s->reserved_af)
            fprintf(f, ", \"reserved_af\": %" PRIu32, s->reserved_af);
        if (s->stream_type &&
            (s->role == TSR_ROLE_ES || s->role == TSR_ROLE_AC3_ES)) {
            fprintf(f, ", \"stream_type\": %d, \"codec\": ",
                    s->stream_type);
            json_str(f, s->codec);
            fprintf(f, ", \"category\": ");
            json_str(f, tsr_category_name(s->category));
            if (s->language[0]) {
                fprintf(f, ", \"language\": ");
                json_str(f, s->language);
            }
            if (s->probe_detail[0]) {
                fprintf(f, ", \"codec_detail\": ");
                json_str(f, s->probe_detail);
            }
        }
        fprintf(f, "}");
        first = 0;
    }
    fprintf(f, "%s]\n", first ? "" : "\n  ");
    fprintf(f, "}\n");
}

// ---- human report ----------------------------------------------------------------

void tsr_write_report(const tsr_mux_t *m, FILE *f)
{
    char buf[64];
    fprintf(f, "TSReader %s — MPEG-TS analyzer\n", TSR_VERSION_STRING);
    fprintf(f, "Input: %s", m->input_desc);
    if (m->file_size_valid) {
        fprintf(f, " (%.1f MiB)", (double)m->file_size / (1024.0 * 1024.0));
    }
    fprintf(f, "\n\nMUX\n");
    if (m->network_name[0])
        fprintf(f, "  Network:      %s\n", m->network_name);
    int pcr_ok = 0;
    double dur = mux_duration_s(m, &pcr_ok);
    fmt_dur(buf, sizeof(buf), dur);
    fprintf(f, "  Duration:     %s%s\n", buf, pcr_ok ? "" : " (wall clock)");
    fmt_bitrate(buf, sizeof(buf), mux_bitrate_bps(m, &pcr_ok));
    fprintf(f, "  Bitrate:      %s%s\n", buf, pcr_ok ? "" : " (wall clock)");
    uint32_t ce = 0, raf = 0;
    for (int i = 0; i < TSR_MAX_PIDS; i++) {
        ce += m->streams[i].continuity_errors;
        raf += m->streams[i].reserved_af;
    }
    fprintf(f, "  TS packets:   %" PRIu64, m->total_packets);
    if (m->sync_errors || ce || raf)
        fprintf(f, "  (%" PRIu32 " sync, %" PRIu32 " continuity, %" PRIu32
                 " reserved-AF)",
                m->sync_errors, ce, raf);
    fprintf(f, "\n  Programs:     %d\n  PIDs:         %d\n", count_programs(m),
            count_pids(m));
    if (m->tdt_unix) {
        // TDT is UTC by definition (EN 300 468); render it as UTC.
        char t[32];
        time_t tt = (time_t)m->tdt_unix;
        struct tm tmv;
        gmtime_r(&tt, &tmv);
        strftime(t, sizeof(t), "%Y-%m-%d %H:%M:%S", &tmv);
        fprintf(f, "  Stream time:  %s (TDT)\n", t);
    }

    // services (DVB)
    int nservices = 0;
    for (int i = 0; i < TSR_MAX_SERVICES; i++)
        if (m->services[i].in_use)
            nservices++;
    if (nservices) {
        fprintf(f, "\nSERVICES\n");
        for (int i = 0; i < TSR_MAX_SERVICES; i++) {
            const tsr_service_t *sv = &m->services[i];
            if (!sv->in_use)
                continue;
            fprintf(f, "  %4u  %-24.24s  provider: %s  EIT: 0x%03X",
                    sv->service_id, sv->name[0] ? sv->name : "-",
                    sv->provider[0] ? sv->provider : "-", sv->eits_pid);
            if (sv->has_number)
                fprintf(f, "  ch %u", sv->logical_number);
            fprintf(f, "\n");
        }
    }

    // programs
    fprintf(f, "\nPROGRAMS\n");
    int any = 0;
    for (int i = 0; i < TSR_MAX_PROGRAMS; i++) {
        const tsr_program_t *pr = &m->programs[i];
        if (!pr->in_use || !pr->pmt_pid)
            continue;
        any = 1;
        fprintf(f, "  Program %u", pr->program_number);
        if (pr->name[0])
            fprintf(f, " — \"%s\"", pr->name);
        if (pr->has_atsc)
            fprintf(f, "  (ATS %u.%u)", pr->atsc_major, pr->atsc_minor);
        fprintf(f, "   [PMT 0x%03X, PCR 0x%03X]", pr->pmt_pid, pr->pcr_pid);
        fmt_bitrate(buf, sizeof(buf), pr->bitrate_bps);
        fprintf(f, "   %s\n", buf);
        for (int p = 0; p < TSR_MAX_PIDS; p++) {
            const tsr_stream_t *s = &m->streams[p];
            if (!s->in_use || s->program_number != pr->program_number)
                continue;
            if (s->role != TSR_ROLE_ES && s->role != TSR_ROLE_AC3_ES)
                continue;
            fprintf(f, "    0x%03X  %-22.22s", s->pid, s->codec);
            const char *kind = tsr_stream_kind(s);
            if (kind[0])
                fprintf(f, "  (%s)", kind);
            if (s->language[0])
                fprintf(f, "  [%s]", s->language);
            if (s->component_tag[0])
                fprintf(f, "  comp %s", s->component_tag);
            if (s->probe_detail[0])
                fprintf(f, "  %s", s->probe_detail);
            if (s->out_path[0])
                fprintf(f, "  -> %s", s->out_path);
            fprintf(f, "\n");
        }
    }
    if (!any)
        fprintf(f, "  (none found)\n");

    // other PIDs
    fprintf(f, "\nPIDs\n");
    for (int i = 0; i < TSR_MAX_PIDS; i++) {
        const tsr_stream_t *s = &m->streams[i];
        if (!s->in_use)
            continue;
        if (s->role == TSR_ROLE_ES || s->role == TSR_ROLE_AC3_ES)
            continue; // shown under programs
        int p0 = 0;
        double dm = mux_duration_s(m, &p0);
        fmt_bitrate(buf, sizeof(buf), (double)s->bytes * 8.0 /
                            (dm > 0 ? dm : 1.0));
        fprintf(f, "  0x%03X  %-10s  %10" PRIu32 " pkts  %s", s->pid,
                tsr_role_name(s->role), s->packets, buf);
        if (s->continuity_errors)
            fprintf(f, "  (%" PRIu32 " continuity)", s->continuity_errors);
        if (s->reserved_af)
            fprintf(f, "  (%" PRIu32 " reserved-AF)", s->reserved_af);
        fprintf(f, "\n");
    }

    // EPG
    int nevents = 0;
    for (size_t i = 0; i < m->n_events; i++)
        if (m->events[i].name[0] || m->events[i].description[0])
            nevents++;
    if (nevents) {
        fprintf(f, "\nEPG (%d events)\n", nevents);
        for (size_t i = 0; i < m->n_events; i++) {
            const tsr_event_t *e = &m->events[i];
            if (!e->name[0] && !e->description[0])
                continue;
            char t[32];
            tsr_format_time(t, sizeof(t), e->start_unix);
            fprintf(f, "  %s  [svc %u]", t, e->service_id);
            if (e->is_now)
                fprintf(f, " NOW");
            else if (e->is_next)
                fprintf(f, " NEXT");
            fprintf(f, "  %s", e->name[0] ? e->name : "(untitled)");
            if (e->description[0])
                fprintf(f, " — %s", e->description);
            if (e->audio_visual[0])
                fprintf(f, "  {%s}", e->audio_visual);
            fprintf(f, "\n");
        }
    }

    // recording
    if (m->record_out || m->record_bytes) {
        fprintf(f, "\nRecorded %s (%.1f MiB)\n",
                m->record_path[0] ? m->record_path : "-",
                (double)m->record_bytes / (1024.0 * 1024.0));
    }
}
