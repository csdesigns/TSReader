// tsreader_si.c — PSI/SI section reassembly and table parsing.
//
// Section semantics: MPEG-2 sections are accumulated per PID, split on
// section boundaries (pointer_field 0xFF handled), validated with the
// MPEG-2 CRC-32, then dispatched by table_id.
//
// Time decoding matches the original TSReader's behavior:
//   - DVB dates are MJD (modified Julian day) values converted to
//     Gregorian (legacy ConvertDVBDate algorithm, ported).
//   - DVB times are 24-bit BCD; digits are extracted exactly as the
//     legacy %06x/sscanf path did (even-time 2-second flag not applied,
//     matching original EPG display).
//   - ATSC A/65 ETT uses plain BCD date/time fields.

#include "tsreader_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>



// ---- per-PID section reassembly state --------------------------------------

typedef struct tsr_si_state {
    uint8_t buf[8192];
    int     len;
    int     pending;      // expecting a section start
    int     pending_classify; // waiting for enough bytes to classify
    int     si_known;     // classified
    int     is_si;        // 1 = SI table, 0 = private data
    int     role;         // TSR_ROLE_* of last seen table
    uint16_t program;     // for PMTs: owning program number
    uint32_t sections;    // CRC-validated sections seen
    uint32_t crc_errors;
    uint32_t crc_fail_run; // consecutive CRC failures (demotion rule)
} tsr_si_state_t;

void tsr_si_init(tsr_mux_t *m)
{
    m->si = calloc(TSR_MAX_PIDS, sizeof(tsr_si_state_t));
}

void tsr_si_free(tsr_mux_t *m)
{
    free(m->si);
    m->si = NULL;
}

// ---- CRC-32 (MPEG-2 transport stream) --------------------------------------

static uint32_t crc_table[256];
static int crc_table_valid;

static void crc_make_table(void)
{
    // MSB-first (non-reflected) table for the MPEG-2 polynomial.
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i << 24;
        for (int k = 0; k < 8; k++)
            c = (c & 0x80000000u) ? ((c << 1) ^ 0x04C11DB7u) : (c << 1);
        crc_table[i] = c;
    }
    crc_table_valid = 1;
}

static uint32_t crc32_with_init(const uint8_t *data, size_t len, uint32_t init)
{
    uint32_t c = init;
    for (size_t i = 0; i < len; i++)
        c = (c << 8) ^ crc_table[(((c >> 24) ^ data[i]) & 0xff)];
    return c;
}

uint32_t tsr_crc32(const uint8_t *data, size_t len)
{
    if (!crc_table_valid)
        crc_make_table();
    return crc32_with_init(data, len, 0);
}

// Verify the trailing CRC of a PSI/SI section. Encoders disagree about
// the initial register value (ISO/IEC 13818-1 says 0; current ffmpeg
// uses 0xFFFFFFFF), so accept either variant.
int tsr_crc32_check(const uint8_t *sec, size_t total)
{
    if (!crc_table_valid)
        crc_make_table();
    uint32_t stored = ((uint32_t)sec[total - 4] << 24) |
                      ((uint32_t)sec[total - 3] << 16) |
                      ((uint32_t)sec[total - 2] << 8) |
                      (uint32_t)sec[total - 1];
    size_t body = total - 4;
    return crc32_with_init(sec, body, 0) == stored ||
           crc32_with_init(sec, body, 0xFFFFFFFFu) == stored;
}

// ---- time helpers ------------------------------------------------------------

// Port of legacy ConvertDVBDate (MJD -> Gregorian), floating-point form.
static void tsr_dvb_date(int mjd, int *y, int *mo, int *d)
{
    double dMJD = (double)mjd;
    double dYear = (dMJD - 15078.2) / 365.25;
    dYear = (double)(int)dYear;
    double dTemp = dYear * 365.25;
    dTemp = (double)(int)dTemp;
    double dMonth = ((dMJD - 14956.1) - dTemp) / 30.6001;
    dMonth = (double)(int)dMonth;
    dTemp = dYear * 365.25;
    dTemp = (double)(int)dTemp;
    double dTemp2 = dMonth * 30.6001;
    dTemp2 = (double)(int)dTemp2;
    double dDay = dMJD - 14956.0 - dTemp - dTemp2;

    *y = (int)dYear;
    *mo = (int)dMonth;
    *d = (int)dDay;
    int carry = (*mo == 14 || *mo == 15) ? 1 : 0;
    *y += carry;
    *y += 1900;
    *mo = *mo - 1 - carry * 12;
}

// BCD time: each nibble is a decimal digit (hh mm ss, 24 bits).
static void tsr_bcd_time24(uint32_t t, int *hh, int *mm, int *ss)
{
    t &= 0xffffff;
    *hh = (((t >> 20) & 0xF) * 10) + ((t >> 16) & 0xF);
    *mm = (((t >> 12) & 0xF) * 10) + ((t >> 8) & 0xF);
    *ss = (((t >> 4) & 0xF) * 10) + (t & 0xF);
}

static int64_t tsr_greg_to_unix(int y, int mo, int d, int hh, int mi, int ss)
{
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = hh;
    tmv.tm_min = mi;
    tmv.tm_sec = ss;
    return (int64_t)timegm(&tmv);
}

static int64_t tsr_dvb_start(int mjd, uint32_t t)
{
    int y, mo, d, hh, mi, ss;
    tsr_dvb_date(mjd, &y, &mo, &d);
    tsr_bcd_time24(t, &hh, &mi, &ss);
    return tsr_greg_to_unix(y, mo, d, hh, mi, ss);
}

// ---- name / role / category helpers ------------------------------------------

static const char *const role_names[] = {
    "none", "PAT", "PMT", "SDT", "NIT", "EIT", "VCT", "TDT", "CAT",
    "MGT", "BAT", "SCT", "MMT", "null", "ES", "AC3-ES", "private SI",
};

const char *tsr_role_name(int role)
{
    if (role < 0 || role >= (int)(sizeof(role_names) / sizeof(role_names[0])))
        return "?";
    return role_names[role];
}

const char *tsr_category_name(int category)
{
    switch (category) {
    case TSR_CAT_VIDEO: return "video";
    case TSR_CAT_AUDIO: return "audio";
    case TSR_CAT_SUBTITLE: return "subtitle";
    case TSR_CAT_DATA: return "data";
    case TSR_CAT_SYNC: return "sync";
    case TSR_CAT_SI: return "SI";
    default: return "";
    }
}

static int tsr_role_of_table(uint8_t id)
{
    switch (id) {
    case 0x00: return TSR_ROLE_PAT;
    case 0x01: return TSR_ROLE_CAT;
    case 0x02: return TSR_ROLE_PMT;
    case 0x09: return TSR_ROLE_BAT;
    case 0x0a: return TSR_ROLE_MGT;
    case 0x0b: return TSR_ROLE_SCT;
    case 0x0c: return TSR_ROLE_MMT;
    case 0x11:
    case 0x42: return TSR_ROLE_SDT; // 0x42 = ATSC A/65 SDT
    case 0x13:
    case 0x14: return TSR_ROLE_TDT;
    case 0x40:
    case 0x41: return TSR_ROLE_NIT;
    case 0x46:
    case 0x47:
    case 0x4e:
    case 0x4f: return TSR_ROLE_EIT;
    case 0x55:
    case 0x56: return TSR_ROLE_VCT;
    case 0x70: case 0x71: case 0x72: case 0x73:
    case 0x74: case 0x75: return TSR_ROLE_EIT;
    case 0xff: return TSR_ROLE_SDT;
    default: return TSR_ROLE_PRIVATE_SI;
    }
}

// Stream type names: ISO/IEC 22003-2 registered values (0x01..0x25),
// plus widely used broadcaster/vendor extensions below 0x26.
const char *tsr_stream_type_name(int st)
{
    switch (st) {
    case 0x01: return "MPEG-1 Video";   // ISO/IEC 11172
    case 0x02: return "MPEG-2 Video";   // ISO/IEC 13818-2
    case 0x03: return "MPEG-1 Audio";   // ISO/IEC 11172
    case 0x04: return "MPEG-2 Audio";   // ISO/IEC 13818-3
    case 0x05: return "Private (sections)";
    case 0x06: return "Private (continuous)";
    case 0x07: return "AC-3";           // ISO/IEC 13522-2
    case 0x08: return "DSM-CC";
    case 0x09: return "MPEG-4 Systems"; // ISO/IEC 14496-1 mux
    case 0x0a: return "DSM-CC";
    case 0x0b: return "DVB MPE";
    case 0x0c: return "DVB ACM";
    case 0x0d: return "DVB AIO";
    case 0x0e: return "DVB DAB";
    case 0x0f: return "AAC (ADTS)";      // 13818-7 slot, universally ADTS AAC
    case 0x10: return "MPEG-4 Visual";  // ISO/IEC 14496-2
    case 0x11: return "MPEG-4 Audio";   // ISO/IEC 14496-3 (AAC)
    case 0x12: return "H.264/AVC";      // ISO/IEC 14496-10 (MPEG-4 AVC)
    case 0x13: return "SDP (Sync Download)";
    case 0x14: return "AC-3";           // ISO/IEC 13522-2
    case 0x15: return "MPEG-4 SM";      // ISO/IEC 14496-1
    case 0x16: return "MPEG-4 MP4";     // ISO/IEC 14496-14
    case 0x17: return "MPEG-TS (PES)";  // 13818-1 Annex A
    case 0x18: return "MPEG-TS (PES)";  // 13818-1 Annex A
    case 0x19: return "FlexMPEG";       // ISO/IEC 14496-5
    case 0x1a: return "AAC (14496-9)";
    case 0x1b: return "H.264/AVC";      // ISO/IEC 14496-10 AVC
    case 0x1c: return "SDP (14496-15)";
    case 0x1d: return "CNG Audio";      // ISO/IEC 14496-18 P2
    case 0x1e: return "MPEG-4A";        // ISO/IEC 14496-21
    case 0x1f: return "CFF (13818-6)";
    case 0x20: return "FFS (13818-6)";
    case 0x21: return "OD (14496-12)";
    case 0x22: return "Scene (14496-4)";
    case 0x23: return "SIP (13235-2)";
    case 0x24: return "SIP (13235-2)";
    case 0x25: return "HEVC";           // ISO/IEC 14496-40
    case 0x26: return "Dolby E";
    case 0x27: return "DigiCipher 2";
    case 0x30: return "DVB TDT";
    case 0x42: return "H.264/AVC";      // ATSC A/53
    case 0x4b: return "H.264/AVC";      // DVB-H / ATSC
    case 0x4d: return "HEVC";           // common vendor HEVC
    case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4a: case 0x4c:
    case 0x4e: case 0x4f: return "ATSC 3.0 IBP";
    case 0x53: return "MMT";            // ISO/IEC 23000-19 range
    case 0x60: case 0x61: case 0x62: case 0x63:
        return "DVB Subtitle";
    case 0x78: case 0x79: return "Teletext";
    case 0x80: return "Captions (ATSC)";
    case 0x81: return "AC-3";           // DVB/ATSC AC-3
    case 0x87: return "E-AC-3";         // DVB/ATSC Dolby Digital Plus
    case 0x88: return "SCTE-27";
    case 0x89: return "DTT Teletext";
    case 0x8a: return "DTT (DVB)";
    case 0x90: return "DTS";
    case 0x91: return "DTS";
    case 0x92: return "DTS ES";
    case 0x93: return "DTS 96/24";
    case 0x94: return "DTS HD";
    case 0x95: return "DTS Neo";
    case 0x96: return "DTS Neo X";
    default:
        return "private";
    }
}

static int tsr_category_of_type(int st)
{
    switch (st) {
    case 0x01: case 0x02: case 0x10: case 0x12: case 0x1b:
    case 0x25: case 0x42: case 0x4b: case 0x4d:
        return TSR_CAT_VIDEO;
    case 0x03: case 0x04: case 0x07: case 0x0f: case 0x11:
    case 0x14: case 0x1a: case 0x1d: case 0x1e: case 0x26:
    case 0x81: case 0x87: case 0x89: case 0x8a:
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94:
    case 0x95: case 0x96:
        return TSR_CAT_AUDIO;
    case 0x60: case 0x61: case 0x62: case 0x63:
    case 0x78: case 0x79: case 0x80:
        return TSR_CAT_SUBTITLE;
    case 0x13: case 0x1c: case 0x44: case 0x45: case 0x46:
    case 0x47: case 0x48: case 0x49: case 0x4a: case 0x4c:
    case 0x4e: case 0x4f:
        return TSR_CAT_SYNC;
    default:
        return TSR_CAT_DATA;
    }
}

const char *tsr_stream_kind(const tsr_stream_t *s)
{
    switch (s->category) {
    case TSR_CAT_VIDEO: return "video";
    case TSR_CAT_AUDIO: return "audio";
    case TSR_CAT_SUBTITLE: return "subtitle";
    case TSR_CAT_SYNC: return "sync";
    case TSR_CAT_DATA: return "data";
    default: return "";
    }
}

// ISO/IEC 22003 registry recognition for PMT registered descriptors.
static const char *tsr_registered_name(uint16_t reg)
{
    switch (reg) {
    case 0x0100: return "MPEG-4 Visual (14496-10)";
    case 0x0200: return "H.264/AVC (AVC1)";
    case 0x2645: return "H.264/AVC (26450)";
    case 0x0280: return "HEVC";
    case 0x0480: return "VVC (H.266)";
    case 0x0140: return "AC-3 (ATSC A/52)";
    case 0x0163:
    case 0x0164: return "E-AC-3 (ATSC A/52A)";
    default: break;
    }
    // MPEG-4 Audio (AAC) registry blocks.
    if (reg >= 0x40 && reg <= 0x7f)
        return "AAC (MPEG-4 Audio)";
    if (reg >= 0xe1 && reg <= 0xe5)
        return "AAC (MPEG-4 Audio)";
    if (reg >= 0x166 && reg <= 0x180)
        return "AAC (MPEG-4 Audio)";
    if (reg >= 0x240 && reg <= 0x275)
        return "AAC (ATSC 3.0)";
    if (reg >= 0x100 && reg <= 0x1ff && reg != 0x0140 && reg != 0x0163 &&
        reg != 0x0164)
        return "MPEG-4 (14496)";
    return NULL;
}

// ---- data model helpers --------------------------------------------------------

tsr_program_t *tsr_get_or_create_program(tsr_mux_t *m, uint16_t number)
{
    tsr_program_t *pr = tsr_get_program(m, number);
    if (pr)
        return pr;
    for (int i = 0; i < TSR_MAX_PROGRAMS; i++) {
        if (!m->programs[i].in_use) {
            m->programs[i].in_use = 1;
            m->programs[i].program_number = number;
            return &m->programs[i];
        }
    }
    return NULL;
}

tsr_service_t *tsr_get_or_create_service(tsr_mux_t *m, uint16_t service_id)
{
    tsr_service_t *sv = tsr_get_service(m, service_id);
    if (sv)
        return sv;
    for (int i = 0; i < TSR_MAX_SERVICES; i++) {
        if (!m->services[i].in_use) {
            m->services[i].in_use = 1;
            m->services[i].service_id = service_id;
            return &m->services[i];
        }
    }
    return NULL;
}

void tsr_add_event(tsr_mux_t *m, uint16_t service_id, uint16_t program,
                   int atsc, uint16_t event_id, int present, int is_now,
                   int is_next, int64_t start_unix, uint32_t duration_s,
                   const char *name, const char *description,
                   const uint8_t *genre, const char *audio_visual)
{
    tsr_event_t *ev = NULL;
    for (size_t i = 0; i < m->n_events; i++) {
        tsr_event_t *e = &m->events[i];
        if (e->service_id == service_id && e->atsc == atsc &&
            e->event_id == event_id) {
            ev = e;
            break;
        }
    }
    if (!ev) {
        if (m->n_events >= 8192)
            return;
        if (m->n_events == m->event_cap) {
            m->event_cap = m->event_cap ? m->event_cap * 2 : 256;
            m->events = realloc(m->events, m->event_cap * sizeof(tsr_event_t));
        }
        ev = &m->events[m->n_events++];
        memset(ev, 0, sizeof(*ev));
        ev->service_id = service_id;
        ev->program_number = program;
        ev->atsc = atsc;
        ev->event_id = event_id;
    }
    ev->present = present;
    if (is_now) ev->is_now = 1;
    if (is_next) ev->is_next = 1;
    if (start_unix > 0)
        ev->start_unix = start_unix;
    if (duration_s)
        ev->duration_s = duration_s;
    if (name && *name &&
        (strlen(name) > strlen(ev->name)))
        snprintf(ev->name, sizeof(ev->name), "%s", name);
    if (description && *description &&
        strlen(description) > strlen(ev->description))
        snprintf(ev->description, sizeof(ev->description), "%s", description);
    if (genre)
        memcpy(ev->genre, genre, 4);
    if (audio_visual && *audio_visual && !ev->audio_visual[0])
        snprintf(ev->audio_visual, sizeof(ev->audio_visual), "%s",
                 audio_visual);
}

static void copy_bounded(char *dst, size_t dstlen, const uint8_t *src, int n)
{
    if (n <= 0)
        return;
    if (n >= (int)dstlen)
        n = (int)dstlen - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

// ---- table handlers --------------------------------------------------------------

static void parse_pat(tsr_mux_t *m, tsr_si_state_t *st, const uint8_t *sec,
                      int total)
{
    (void)st;
    // PAT (ISO/IEC 13818-1 2.4.3.4, standard DVB):
    //   table_id(8) section_length(12)
    //   transport_stream_id(16)
    //   [reserved(2) version(5) cn(1)](8) section(8) last(8)
    //   program loop: program_number(16) [reserved(3) PMT_PID(13)]
    //   CRC(32).
    if (total < 11)
        return;
    const uint8_t *p = sec + 8;
    while (p + 4 <= sec + total - 4) { // stop before the CRC
        uint16_t prog = (uint16_t)((p[0] << 8) | p[1]);
        uint16_t pmt = (uint16_t)(((p[2] << 8) | p[3]) & 0x1fff);
        if (prog != 0 && pmt == 0x001) {
            // DVB network entry (program 1): PID 0x001 carries the NIT,
            // not a PMT.
            tsr_si_state_t *nitst = &m->si[0x001];
            nitst->si_known = 1;
            nitst->is_si = 1;
            nitst->role = TSR_ROLE_NIT;
            m->streams[0x001].role = TSR_ROLE_NIT;
        } else if (prog != 0 && pmt != 0x1fff) {
            tsr_program_t *pr = tsr_get_or_create_program(m, prog);
            if (pr) {
                pr->pmt_pid = pmt;
                tsr_si_state_t *pmtst = &m->si[pmt];
                pmtst->si_known = 1;
                pmtst->is_si = 1;
                pmtst->role = TSR_ROLE_PMT;
                pmtst->program = prog;
            }
        }
        // program 0 (ATSC network ID) and pmt 0x1fff: no program.
        p += 4;
    }
}

static void parse_pmt(tsr_mux_t *m, tsr_si_state_t *st, const uint8_t *sec,
                      int total)
{
    m->pmt_seen = 1;
    const uint8_t *end = sec + total;
    // PMT (ISO/IEC 13818-1 2.4.3.6, standard DVB). Unlike the PAT, the
    // PMT has no transport_stream_id field.
    //   table_id(8) section_length(12)
    //   program_number(16)
    //   [reserved(2) version(5) cn(1)](8) section(8) last(8)
    //   [reserved(3) PCR_PID(13)](16)
    //   [reserved(4) program_info_length(12)](16)
    //   program descriptors, stream loop, CRC(32).
    if (total < 16)
        return;
    uint16_t prog = (uint16_t)((sec[3] << 8) | sec[4]);
    uint16_t pcr = (uint16_t)(((sec[8] << 8) | sec[9]) & 0x1fff);
    int pil = (int)(((sec[10] & 0x0f) << 8) | sec[11]);
    const uint8_t *p = sec + 12 + pil; // past program descriptors

    while (p + 5 <= end - 4) { // stop before the CRC
        int stype = p[0];
        uint16_t epid = (uint16_t)(((p[1] << 8) | p[2]) & 0x1fff);
        int elen = (int)(((p[3] & 0x0f) << 8) | p[4]);
        p += 5;
        if (p + elen > end)
            break;

        tsr_stream_t *s = &m->streams[epid];
        if (stype != 0) {
            s->in_use = 1;
            s->role = (stype == 0x81 || stype == 0x87) ? TSR_ROLE_AC3_ES
                                                       : TSR_ROLE_ES;
            s->stream_type = stype;
            s->program_number = prog;
            s->category = tsr_category_of_type(stype);
            snprintf(s->codec, sizeof(s->codec), "%s",
                     tsr_stream_type_name(stype));
            s->language[0] = 0;
            s->component_tag[0] = 0;
        }
        const uint8_t *q = p;
        const uint8_t *qend = p + elen;
        while (q + 2 <= qend) {
            int tag = q[0];
            int dlen = q[1];
            q += 2;
            if (q + dlen > qend)
                break;
            if (s->in_use) {
                if (tag == 0x0a && dlen >= 3) {
                    // ES language descriptor: 3-char ISO-639-2.
                    for (int i = 0; i < 3; i++)
                        s->language[i] = (char)q[2 + i];
                    s->language[3] = 0;
                } else if (tag == 0x00 && dlen >= 4) {
                    uint16_t reg = (uint16_t)((q[2] << 8) | q[3]);
                    const char *nm = tsr_registered_name(reg);
                    if (nm) {
                        char kind[8];
                        snprintf(kind, sizeof(kind), "%s",
                                 tsr_category_name(tsr_category_of_type(stype)));
                        if (kind[0])
                            snprintf(s->codec, sizeof(s->codec), "%s (%s)", nm, kind);
                        else
                            snprintf(s->codec, sizeof(s->codec), "%s", nm);
                    }
                } else if (tag == 0x50 && dlen >= 9) {
                    // DVB component tag: reserved(16) type(8) tag(24) lang(24)
                    char tagbuf[4] = {0};
                    tagbuf[0] = (char)q[4];
                    tagbuf[1] = (char)q[5];
                    tagbuf[2] = (char)q[6];
                    snprintf(s->component_tag, sizeof(s->component_tag),
                             "%s", tagbuf);
                }
            }
            q += dlen;
        }
        tsr_extract_reopen(m, s);
        p = qend;
    }

    tsr_program_t *pr = tsr_get_or_create_program(m, prog);
    if (pr && pcr)
        pr->pcr_pid = pcr;
    (void)st;
}

static void parse_sdt(tsr_mux_t *m, const uint8_t *sec, int total)
{
    const uint8_t *end = sec + total;
    // SDT (0x11/0xff) layout: table_id(8) section_length(12)
    // [reserved(2) version(5) cn(1)] section(8) last(8)
    // transport_stream_id(16) original_network_id(16)
    // [reserved(2) version(5) cn(1)] section(8) last(8)
    // then service loop entries:
    //   service_id(16) [reserved(2) version(5) cn(1)] section(8) last(8)
    //   eits_pid(16) [reserved(4) descriptors_length(12)].
    if (total < 13)
        return;
    uint32_t onid = ((uint32_t)sec[8] << 8) | sec[9];
    if (!m->network_name[0] && onid) {
        m->network_id = onid;
        m->has_network_id = 1;
    }
    const uint8_t *p = sec + 13;
    while (p + 9 <= end - 4) { // stop before the CRC
        uint16_t svc = (uint16_t)((p[0] << 8) | p[1]);
        // service loop entry (9 bytes): svc(16) vergrp sec last
        // [reserved(5) EITs_PID(11)] [reserved(4) descriptors_length(12)]
        uint16_t eits = (uint16_t)(((p[5] << 8) | p[6]) & 0x07ff);
        if (eits == 0x07ff)
            eits = 0; // "EITs not present" is encoded as 0x7FF
        int dl = (int)(((p[7] & 0x0f) << 8) | p[8]);
        p += 9;
        const uint8_t *dend = p + dl;
        if (dend > end)
            break;
        tsr_service_t *sv = tsr_get_or_create_service(m, svc);
        if (sv)
            sv->eits_pid = eits;
        const uint8_t *q = p;
        while (q + 2 <= dend) {
            int tag = q[0];
            int dlen = q[1];
            q += 2;
            if (q + dlen > dend)
                break;
            if (sv) {
                if (tag == 0x48 && dlen >= 4) {
                    copy_bounded(sv->name, sizeof(sv->name), q + 3,
                                 dlen - 3);
                } else if (tag == 0x47 && dlen >= 4) {
                    copy_bounded(sv->provider, sizeof(sv->provider), q + 3,
                                 dlen - 3);
                } else if (tag == 0x64 && dlen >= 3) {
                    sv->logical_number = q[2];
                    sv->has_number = 1;
                } else if (tag == 0x50 && dlen >= 7) {
                    copy_bounded(sv->affiliation, sizeof(sv->affiliation),
                                 q + 4, dlen - 4);
                }
            }
            q += dlen;
        }
        p = dend;
        // Link program: DVB has no explicit link; convention is
        // program_number == service_id.
        tsr_program_t *pr = tsr_get_program(m, svc);
        if (pr && !pr->name[0]) {
            snprintf(pr->name, sizeof(pr->name), "%s", sv->name);
            snprintf(pr->provider, sizeof(pr->provider), "%s", sv->provider);
            pr->service_id = svc;
        }
    }
}

// Walk descriptor bytes; harvest network name (0x40) and provider (0x48).
static void nit_walk_descs(tsr_mux_t *m, const uint8_t *p, const uint8_t *end)
{
    while (p + 2 <= end) {
        int tag = p[0];
        int dlen = p[1];
        p += 2;
        if (p + dlen > end)
            break;
        if (tag == 0x40 && dlen >= 3 && !m->network_name[0]) {
            // network name: language(3) name
            copy_bounded(m->network_name, sizeof(m->network_name),
                         p + 3, dlen - 3);
        } else if (tag == 0x48 && dlen >= 5 && !m->network_provider[0]) {
            // service provider: language(3) provider_name
            copy_bounded(m->network_provider, sizeof(m->network_provider),
                         p + 3, dlen - 3);
        }
        p += dlen;
    }
}

static void parse_nit(tsr_mux_t *m, const uint8_t *sec, int total)
{
    const uint8_t *end = sec + total;
    // NIT 0x40/0x41 (EN 300 468, standard layout). The NIT has no
    // original_network_id field:
    //   TSI(16) vergrp(8) sec(8) last(8)
    //   network_id(16) vergrp(8) sec(8) last(8)
    //   [reserved(4) network_descriptors_length(12)](16) [descriptors]
    //   0x41: network loop: network_id(16) vergrp(8) sec(8) last(8)
    //         [reserved(4) dlen(12)](16) [descriptors]
    //   CRC(32).
    if (total < 15)
        return;
    uint32_t net = ((uint32_t)sec[8] << 8) | sec[9];
    if (net && !m->has_network_id) {
        m->network_id = net;
        m->has_network_id = 1;
    }
    int ndl = (int)(((sec[13] & 0x0f) << 8) | sec[14]);
    const uint8_t *p = sec + 15;
    if (p + ndl > end)
        return;
    nit_walk_descs(m, p, p + ndl);
    if (sec[0] == 0x41) {
        p += ndl;
        // first network loop entry (only one is consumed in v1)
        if (p + 7 <= end - 4) {
            uint32_t lnet = ((uint32_t)p[0] << 8) | p[1];
            int dl = (int)(((p[5] & 0x0f) << 8) | p[6]);
            if (lnet && !m->has_network_id) {
                m->network_id = lnet;
                m->has_network_id = 1;
            }
            if (p + 7 + dl <= end)
                nit_walk_descs(m, p + 7, p + 7 + dl);
        }
    }
}

static void parse_dvb_eit(tsr_mux_t *m, const uint8_t *sec, int total,
                          int present)
{
    const uint8_t *end = sec + total;
    // DVB EIT (EN 300 468 9.7, standard layout). There is no
    // per-section service_id (that is the ATSC ETT shape):
    //   table_id(8) section_length(12)
    //   TSI(16) vergrp(8) sec(8) last(8)
    //   original_network_id(16) vergrp(8) sec(8) last(8)
    //   [reserved(4) EIT_schedule_flag(1) reserved(3)](8)
    //   [reserved(4) event_descriptor_length(12)](16)
    //   EIT_SDT (0x40/0x41): service loop:
    //     service_id(16) [reserved(4) EITs_PID(11) reserved(1)](16)
    //     [reserved(4) descriptors_length(12)](16) [descriptors]
    //   event loop: event_id(16) MJD(16) BCD start(24) BCD duration(24)
    //   [reserved(4) running_status(2) descriptor_length(12)](16)
    //   [event descriptors]
    //   CRC(32).
    // Events apply to the services listed in the EIT_SDT service loop;
    // for the loop-less tables (0x42/0x43/0x46/0x47) they apply to every
    // service in the TS (TS-wide EIT), or a service-0 placeholder if
    // none is known yet.
    if (total < 18)
        return;
    int edl = (int)(((sec[14] & 0x0f) << 8) | sec[15]);
    const uint8_t *p = sec + 16 + edl; // past service-level descriptors
    if (p > end)
        return;

    uint16_t scv[8];
    int nscv = 0;
    if (sec[0] == 0x40 || sec[0] == 0x41) {
        // The service loop has no explicit length: entries are
        // self-delimiting and the event loop follows directly. A real
        // service entry carries reserved(4) bits in both 16-bit fields at
        // offsets 2 and 4 (an event's byte 4 is BCD hours, 0x00-0x19).
        while (p + 6 <= end - 4 && (p[2] & 0xF0) == 0xF0 &&
               (p[4] & 0xF0) == 0xF0) { // stop before the CRC
            uint16_t svc = (uint16_t)((p[0] << 8) | p[1]);
            uint16_t eits = (uint16_t)(((p[2] << 8) | p[3]) & 0x07ff);
            int dl = (int)(((p[4] & 0x0f) << 8) | p[5]);
            p += 6;
            if (p + dl > end)
                break;
            if (nscv < 8)
                scv[nscv++] = svc;
            tsr_service_t *sv = tsr_get_or_create_service(m, svc);
            if (sv) {
                sv->eits_pid = (eits == 0x07ff) ? 0 : eits;
                sv->original_network_id =
                    ((uint32_t)sec[8] << 8) | sec[9];
            }
            p += dl; // service descriptors (names come from the SDT)
        }
    }

    // 12-byte event header: event_id(16) MJD(16) BCD start(24)
    // BCD duration(24) [reserved(4) running_status(2) dlen(12)].
    while (p + 12 <= end - 4) { // stop before the CRC
        uint16_t id = (uint16_t)((p[0] << 8) | p[1]);
        int mjd = (p[2] << 8) | p[3];
        uint32_t t = (uint32_t)((p[4] << 16) | (p[5] << 8) | p[6]);
        uint32_t dur = (uint32_t)((p[7] << 16) | (p[8] << 8) | p[9]);
        int dlen = (int)(((p[10] << 8) | p[11]) & 0xfff);
        const uint8_t *dq = p + 12;
        if (dq + dlen > end)
            break;

        int hh, mm, ss, dh, dm, ds;
        tsr_bcd_time24(t, &hh, &mm, &ss);
        tsr_bcd_time24(dur, &dh, &dm, &ds);
        int64_t start = tsr_dvb_start(mjd, t);
        uint32_t dur_s = (uint32_t)(dh * 3600 + dm * 60 + ds);

        char name[256] = {0};
        char desc[1024] = {0};
        char lang[8] = {0};
        int is_now = 0, is_next = 0;
        const uint8_t *q = dq;
        const uint8_t *qend = dq + dlen;
        while (q + 2 <= qend) {
            int tag = q[0];
            int elen = q[1];
            q += 2;
            if (q + elen > qend)
                break;
            if (tag == 0x4d && elen >= 5) {
                // short event: language(3) encoding(1) length(1) text
                if (!lang[0]) {
                    for (int i = 0; i < 3; i++)
                        lang[i] = (char)q[i];
                    lang[3] = 0;
                }
                copy_bounded(name, sizeof(name), q + 5, (int)q[4]);
            } else if (tag == 0x54 && elen >= 5) {
                // long event: language(3) encoding(1) default_length(1)
                // text(default_length) [further language blocks]
                int n = q[4];
                if (n > elen - 5)
                    n = elen - 5;
                copy_bounded(desc, sizeof(desc), q + 5, n);
            } else if (tag == 0x80 && elen == 0) {
                is_now = 1;
            } else if (tag == 0x82 && elen == 0) {
                is_next = 1;
            }
            q += elen;
        }
        if (nscv > 0) {
            for (int i = 0; i < nscv; i++)
                tsr_add_event(m, scv[i], 0, 0, id, present, is_now,
                              is_next, start, dur_s, name, desc, NULL,
                              NULL);
        } else {
            int any = 0;
            for (int i = 0; i < TSR_MAX_SERVICES; i++) {
                if (!m->services[i].in_use)
                    continue;
                tsr_add_event(m, m->services[i].service_id, 0, 0, id,
                              present, is_now, is_next, start, dur_s,
                              name, desc, NULL, NULL);
                any = 1;
            }
            if (!any)
                tsr_add_event(m, 0, 0, 0, id, present, is_now, is_next,
                              start, dur_s, name, desc, NULL, NULL);
        }
        p = qend;
    }
}

static void parse_atsc_ett(tsr_mux_t *m, const uint8_t *sec, int total,
                           int present)
{
    const uint8_t *end = sec + total;
    // ATSC A/65 ETT layout:
    //   table_id(8) [syntax(1) reserved(1) section_length(12)]
    //   [reserved(2) version(5) cn(1)] section(8) last(8)
    //   table_id_extension(16) [reserved(2) version(5) cn(1)] section(8)
    //   last(8) protocol_version(8) source_id(16)
    //   program loop:
    //     program_number(16) [reserved(2) version(5) cn(1)] section(8)
    //     last(8) service_id(16) major_number(8) minor_number(8)
    //     short_program_name(256) [reserved(4) long_program_name_length(12)]
    //     long_program_name(8*n) [reserved(4) event_descriptor_length(12)]
    //     event descriptors: [reserved(4) tag(12)] length(8) data
    // A/65 ETT carries no per-event timestamps (those live in optional
    // time descriptors), so v1 consumes program names and text:
    // descriptor 0x00 (description) and 0x02 (extended rating/genre).
    if (total < 14)
        return;
    const uint8_t *p = sec + 14;
    while (p + 43 <= end) {
        uint16_t prog = (uint16_t)((p[0] << 8) | p[1]);
        p += 2;
        p += 3; // reserved / version / cn / section / last
        if (p + 40 > end)
            break;
        uint16_t svc = (uint16_t)((p[0] << 8) | p[1]);
        uint8_t major = p[2];
        uint8_t minor = p[3];
        p += 4;
        char shortname[33] = {0};
        copy_bounded(shortname, sizeof(shortname), p, 32);
        p += 32;
        int lnlen = (int)(((p[0] & 0x0f) << 8) | p[1]);
        p += 2;
        char longname[256] = {0};
        copy_bounded(longname, sizeof(longname), p, lnlen);
        p += lnlen;
        if (p + 2 > end)
            break;
        int edl = (int)(((p[0] & 0x0f) << 8) | p[1]);
        p += 2;
        const uint8_t *qend = p + edl;
        if (qend > end)
            break;
        char desc[1024] = {0};
        uint8_t genre[4] = {0};
        const uint8_t *q = p;
        while (q + 3 <= qend) {
            uint16_t tag = (uint16_t)(((q[0] & 0x0f) << 8) | q[1]);
            int elen = q[2];
            q += 3;
            if (q + elen > qend)
                break;
            if (tag == 0x00 && elen > 0) {
                if (desc[0])
                    strncat(desc, " ", sizeof(desc) - strlen(desc) - 1);
                strncat(desc, (const char *)q,
                        (size_t)(elen < 32 ? elen : 32));
            } else if (tag == 0x02 && elen >= 4) {
                genre[0] = q[0]; genre[1] = q[1];
                genre[2] = q[2]; genre[3] = q[3];
            }
            q += elen;
        }
        p = qend;

        tsr_program_t *pr = tsr_get_or_create_program(m, prog);
        if (pr) {
            pr->has_atsc = 1;
            pr->atsc_major = major;
            pr->atsc_minor = minor;
            if (!pr->service_id)
                pr->service_id = svc;
            if (!pr->name[0])
                snprintf(pr->name, sizeof(pr->name), "%s",
                         longname[0] ? longname : shortname);
        }
        tsr_add_event(m, svc, prog, 1, 0, present, 0, 0, 0, 0,
                      longname[0] ? longname : shortname, desc, genre, NULL);
        (void)present;
    }
}

// Decode one BCD byte (two decimal digits).
static int tsr_bcd2(uint8_t v)
{
    return ((v >> 4) & 0xF) * 10 + (v & 0xF);
}

static void parse_tdt(tsr_mux_t *m, const uint8_t *sec, int total)
{
    // TDT (EN 300 401 / 300 468 9.4.2, standard layout). The value is
    // UTC by definition and is stored as a UTC epoch.
    //   TSI(16) vergrp(8) sec(8) last(8)
    //   BCD_seconds(8) BCD_minutes(8) BCD_hours(8)
    //   [reserved(4) even_minute(1) leap_second(1)](8)
    //   BCD_year(8) BCD_month(8) BCD_day(8) CRC(32).
    //   (19 bytes total; section_length 16.)
    if (total < 19)
        return;
    int ss = tsr_bcd2(sec[8]);
    int mi = tsr_bcd2(sec[9]);
    int hh = tsr_bcd2(sec[10]);
    int yy = tsr_bcd2(sec[12]);
    int mo = tsr_bcd2(sec[13]);
    int dy = tsr_bcd2(sec[14]);
    int y = (yy < 70) ? 2000 + yy : 1900 + yy;
    m->tdt_time_raw = (uint16_t)((hh << 8) | mi);
    if (mo < 1 || mo > 12 || dy < 1 || dy > 31 || hh > 23 || mi > 59 ||
        ss > 59)
        return; // out-of-range: keep the raw value only
    m->tdt_unix = tsr_greg_to_unix(y, mo, dy, hh, mi, ss); // UTC (timegm)
    m->tdt_valid = 1;
}

// ---- dispatch ------------------------------------------------------------------

static void dispatch(tsr_mux_t *m, tsr_si_state_t *st, uint16_t pid,
                     const uint8_t *sec, int total)
{
    uint8_t id = sec[0];
    st->role = tsr_role_of_table(id);
    switch (id) {
    case 0x00:
        parse_pat(m, st, sec, total);
        break;
    case 0x02:
        parse_pmt(m, st, sec, total);
        break;
    case 0x11:
    case 0xff:
        parse_sdt(m, sec, total);
        break;
    case 0x42:
        // ATSC A/65 SDT: recognized and counted; name decoding is v2.
        break;
    case 0x40:
    case 0x41:
        // Table IDs 0x40/0x41 are shared (EN 300 468): NIT on the network
        // PID (0x001), EIT_SDT/actual on EIT PIDs. Disambiguate by PID.
        if (pid == 0x001) {
            parse_nit(m, sec, total);
        } else {
            st->role = TSR_ROLE_EIT;
            parse_dvb_eit(m, sec, total, id == 0x41);
        }
        break;
    case 0x46:
        parse_dvb_eit(m, sec, total, 1);
        break;
    case 0x47:
        parse_dvb_eit(m, sec, total, 0);
        break;
    case 0x55:
    case 0x56:
        // VCT: name handling is folded into ETT for v1.
        break;
    case 0x70: case 0x71: case 0x72: case 0x73:
        parse_atsc_ett(m, sec, total, 1);
        break;
    case 0x74: case 0x75:
        parse_atsc_ett(m, sec, total, 0);
        break;
    case 0x13:
    case 0x14:
        parse_tdt(m, sec, total);
        break;
    case 0x4e:
    case 0x4f:
        // ATSC A/53 EIT (GPS based, needs STT) — counted, not decoded.
        break;
    default:
        break;
    }
}

static int known_table(uint8_t id)
{
    switch (id) {
    case 0x00: case 0x01: case 0x02: case 0x09: case 0x0a: case 0x0b:
    case 0x0c: case 0x11: case 0x13: case 0x14: case 0x40: case 0x41:
    case 0x42:
    case 0x46: case 0x47: case 0x4e: case 0x4f: case 0x55: case 0x56:
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:
    case 0xff:
        return 1;
    default:
        return 0;
    }
}

void tsr_si_packet(tsr_mux_t *m, const uint8_t *pkt)
{
    uint16_t pid = (uint16_t)(((pkt[1] << 8) | pkt[2]) & 0x1fff);
    int pusi = (pkt[1] >> 6) & 1;
    // adaptation_field_control, bits 5:4 of the 3rd header byte (0x30 mask)
    // per ISO/IEC 13818-1: 01 payload, 10 AF only, 11 AF + payload, 00
    // reserved (strictly: no payload, no AF).
    int afc = (pkt[3] & 0x30) >> 4;
    const uint8_t *p;
    int n;
    if (afc == 1) {
        p = pkt + 4;
        n = 184;
    } else if (afc == 3) {
        int af_len = pkt[4];
        if (5 + af_len > 188)
            return;
        p = pkt + 5 + af_len;
        n = 188 - 5 - af_len;
    } else
        return; // AF only, or reserved (no payload): no section data

    tsr_si_state_t *st = &m->si[pid];

    // Classification.
    //  - PID 0 is always the PAT.
    //  - PMT PIDs are pre-marked when the PAT is parsed.
    //  - Everything else is classified once we have seen a PUSI packet and
    //    can verify the section header (a real table must pass the
    //    minimum-length and CRC checks; H.264 start codes begin with 0x00,
    //    which looks like table_id 0, so the 11-byte minimum is essential).
    if (pid == 0x1fff) {
        st->si_known = 1;
        st->is_si = 0;
        m->streams[pid].role = TSR_ROLE_NULL;
        return;
    }
    if (pid == 0x0000) {
        st->si_known = 1;
        st->is_si = 1;
        st->role = TSR_ROLE_PAT;
    } else if (st->program) {
        st->si_known = 1;
        st->is_si = 1;
    } else if (!st->si_known) {
        if (pusi && !st->pending_classify)
            st->pending_classify = 1;
        else if (st->len >= (int)sizeof(st->buf)) {
            // Buffer cycled without a valid table header: not an SI source.
            st->si_known = 1;
            st->is_si = 0;
            st->len = 0;
            return;
        }
    }

    if (!st->is_si) {
        // Keep accumulating only while classification is pending.
        if (st->si_known || !st->pending_classify)
            return;
    }

    // PSI/SI framing (13818-1 2.4.3.5): a PUSI packet of an SI source
    // starts with a pointer_field. Value 0x00/0xFF -> the section starts
    // at the next byte; value k -> k continuation bytes precede it.
    if (st->is_si && pusi && n > 0) {
        uint8_t ptr = p[0];
        int skip = (ptr == 0xff) ? 1 : (1 + ptr);
        if (skip >= n)
            return;
        p += skip;
        n -= skip;
    }

    if (pusi)
        st->pending = 1;

    if (st->len + n > (int)sizeof(st->buf)) {
        st->len = 0;
        st->pending = 1;
    }
    memcpy(st->buf + st->len, p, n);
    st->len += n;

    // Deferred classification: now that data is buffered, verify a
    // section header near the front of the buffer. The first PUSI packet
    // of an SI source still carries its pointer_field, so also try the
    // header one byte later when a zero pointer precedes it.
    if (!st->si_known && st->pending_classify) {
        int s = 0;
        while (s < st->len && st->buf[s] == 0xff)
            s++;
        int cands[2] = {s, s + 1};
        int nc = (s + 4 <= st->len && st->buf[s] == 0x00) ? 2 : 1;
        for (int ci = 0; ci < nc; ci++) {
            int t = cands[ci];
            if (t + 3 > st->len)
                continue;
            uint8_t id = st->buf[t];
            int slen = (int)(((st->buf[t + 1] & 0x0f) << 8) | st->buf[t + 2]);
            // table_id 0 (PAT) is only valid on PID 0 itself.
            if (id == 0x00 && pid != 0x0000)
                continue;
            if (!known_table(id) || 3 + slen < 11)
                continue;
            st->si_known = 1;
            st->pending_classify = 0;
            st->is_si = 1;
            st->role = tsr_role_of_table(id);
            if (t > 0) { // drop pointer_field / stuffing before the header
                st->len -= t;
                memmove(st->buf, st->buf + t, (size_t)st->len);
            }
            break;
        }
        if (!st->si_known)
            return; // wait for the next PUSI packet
    }

    if (!st->is_si)
        return;

    int pos = 0;
    for (;;) {
        int s = pos;
        while (s < st->len && st->buf[s] == 0xff)
            s++;
        if (s >= st->len) {
            pos = st->len; // drop pointer-field garbage
            break;
        }
        if (s + 3 > st->len) {
            pos = 0; // section header incomplete
            break;
        }
        if (!known_table(st->buf[s])) {
            st->is_si = 0;
            st->len = 0;
            st->pending = 0;
            break;
        }
        int slen = (int)(((st->buf[s + 1] & 0x0f) << 8) | st->buf[s + 2]);
        int total = 3 + slen;
        if (total < 11) {
            st->is_si = 0;
            st->len = 0;
            st->pending = 0;
            break;
        }
        if (s + total > st->len) {
            pos = 0; // incomplete; wait for more data
            break;
        }
        const uint8_t *sec = st->buf + s;
        if (!tsr_crc32_check(sec, (size_t)total)) {
            st->crc_errors++;
            st->crc_fail_run++;
            // Sustained CRC failures on a PID claimed to carry tables:
            // it was a misclassification; stop buffering it.
            if (st->crc_fail_run > 16) {
                st->is_si = 0;
                st->len = 0;
                st->pending = 0;
                pos = 0;
                break;
            }
        } else {
            st->crc_fail_run = 0;
            st->sections++;
            dispatch(m, st, pid, sec, total);
        }
        pos = s + total;
    }
    if (pos) {
        st->len -= pos;
        if (st->len > 0)
            memmove(st->buf, st->buf + pos, st->len);
    }

    // Keep the stream table's role in sync (only when still SI).
    if (st->is_si)
        m->streams[pid].role = st->role;
}

void tsr_si_print_header(tsr_mux_t *m, FILE *f)
{
    (void)m;
    (void)f;
}
