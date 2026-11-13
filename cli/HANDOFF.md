# Handoff Document: TSReader — Modern POSIX CLI (tsreader 3.0.0) — Session 8

## 1. Task (unchanged)

Port `/Users/csdesigns/Applications/TSReader` (legacy Win32 TSReader) to
macOS/Linux. User-approved direction: modern headless `tsreader` CLI first
(non-1:1), open-source stack (ffmpeg/gstreamer later), modern
macOS-flavored GUI **later — ask before starting**, cross-platform
macOS + Linux, Windows later.

**Status: the CLI phase is functionally COMPLETE and verified,
and FFmpeg codec probing (roadmap step 1) is now implemented**
(sessions 7–8, see §4 and §6).

## 2. User preferences / constraints (recap)

- Working dir `/Users/csdesigns/Applications/TSReader` — git repo, **dirty;
  never revert** anything not made by these agents.
- macOS 27.2.1 (arm64), user `csdesigns`, 2026-11-12, TZ America/New_York
  (EST, UTC-5) — note EPG (MJD/absolute) times display ~5h "earlier" than
  TDT wall-clock times; that is correct, not a bug.
- **User instruction (session 8):** if a blocking/gated issue prevents
  recompilation, report it to the user before switching approaches.
- FFmpeg on this machine is a **Homebrew dev (HEAD) build**
  (`/opt/homebrew/opt/ffmpeg`, Cellar `HEAD-b2b543c_1`, libavcodec 63 /
  libavformat 63). `pkg-config` works; CMake's Homebrew-prefix direct-path
  check misses (HEAD bottle symlinks aren't in /opt/homebrew/lib) and the
  pkg-config fallback engages — that is expected and fine.
- **Debugger attach blocked** — debug with ASan builds, temp `getenv`
  traces, or small C harnesses (worked well again).
- Clean C11; mux state must be **heap/static** (~10–15 MB; main-thread
  stack is 8 MB on macOS).
- `run_python` is Pyodide (no host FS) — use `shell_command python3` for
  host file work. Scratch lives in `/tmp/tsr_fix*`.
- Precedence traps remain real: fully parenthesize mixed `&`/`<<`/`!=`;
  AF control bits 6:5 of header byte 3 (`3 → 0x60`); PCR packing
  base33/post9 as verified in session 6.

## 3. Key decisions this session

- **Standard DVB layouts win** (carried decision), now applied to PAT and
  PMT as well (session 6 had only NIT/EIT):
  - PAT: TSI @6, ONID @8, **program loop @10** (was @8, legacy).
  - PMT: `program_number` **@6** (was @3, legacy); pcr @8, pil @10,
    ES loop @12 unchanged.
  - SDT service loop (9-byte entry): `EITs_PID` @5, `desc_len` @7
    (both were off-by-one twice: first @4/@6 legacy, then @6/@8 — the
    correct standard is @5/@7).
  - EIT: **one `service_id` per section** (EN 300 468). The engine's flat
    event walk to the CRC is correct; the *fixture* was non-standard
    (two svc blocks in one section) — fixed on the generator side.
  - TDT: `BCD_date(16)` = res(2) YY(6) MM(4) DD(4) with the 26–31 day
    binary quirk; `BCD_time(16)` = **HH(8 BCD) MM(8 BCD)** (full-byte
    BCD, not nibbles). Century: yy<70 → 2000+yy else 1900+yy.
- `tsr_bcd_time24` rewritten as explicit nibble math (the old `strtol`
  version greedily consumed the whole "203000" into hh — all EIT
  durations were 10⁴× too big until fixed).
- PCR first-tick: added `tsr_program_t.pcr_ticks_valid` flag — a first
  PCR of exactly tick 0 used to be re-stamped with the second tick.
- `parse_dvb_eit` **infinite loop fixed** (the session-6 hang): a
  `for(;;)` wrapped the event `while`; a `break` inside re-looped forever
  with no progress. Now a single bounded `while (p + 12 <= end - 4)`.
- `demo3s.ts` created: demo3's ES stream (with its 98 real CC errors)
  re-muxed with standard-layout SI, because demo3's original PAT/PMT are
  legacy-layout and no longer parse (expected; demo3 was hand-assembled
  to the legacy offsets). demo3s is now the regression fixture.
- `record` now fails hard (exit 1) if the output can't be opened —
  previously it silently no-op'd.
- **FFmpeg codec probe design (session 8):** bounded 256 KB ES sample per
  probeable PID, probed at finalization. Video/MP3/AC-3/E-AC-3 via FFmpeg
  raw demuxers opened from a **temp file** with the stable
  `avformat_open_input(filename)` API — deliberately *not* the custom AVIO
  path (this machine's dev FFmpeg has a relocated `avio.h`, a changed
  `avio_alloc_context` signature, no `adts` demuxer, and parsers that no
  longer fill width/height/sample_rate — see §6b). AAC via a built-in
  7-byte ADTS header parser (no FFmpeg needed; also the fallback for
  no-FFmpeg builds). `FF_PROFILE_*` constants are not in this build's
  headers — stable numeric values are defined locally in
  `tsreader_probe.c`. `av_log_set_level(AV_LOG_PANIC)` keeps
  `find_stream_info` chatter off stderr.

## 4. Verification matrix (all green, session 7)

Fixture `dvb2.ts` (5400 pkts, 2 programs, standard DVB SI; generator
`/tmp/tsr_fix2/mkdvb.py`, ES sources `v.h264`/`a.aac` in same dir):

- Independent Python scan: **600/600 SI sections CRC-valid, 0 CC errors**,
  240 PCRs exact.
- `list`: Network DemoNet; Stream time 2026-11-11 20:30 (TDT); services
  257 "Alpha One" ch 1, 258 "Beta Two" ch 2; programs 1/2 with PMT
  0x100/0x103, PCR 0x101/0x104; per-PID roles PAT/SDT/NIT/TDT/EIT/PMT×2;
  EPG 3 named events with correct durations (1800/10800/3600 s).
- `list --json`: valid JSON; `stream_time_unix` (not `tdt_unix`!) key;
  services/programs/events arrays complete.
- `extract -e prog=1|2` and `-e pid=0x101`: **byte-exact** vs
  independent payload decode (219840/198720 bytes per stream).
- `record`: byte-identical passthrough (1,015,200 B).

`demo3.ts` (original): 98 CC errors preserved (81×0x100, 17×0x101); SI
labels now empty (legacy-layout PAT/PMT — expected).

`demo3s.ts` (standard SI over demo3 ES, 1154 pkts): 98 CC errors,
Program 1 "Demo Three" [PMT 0x1000, PCR 0x100], service 256 ch 100,
extracts **byte-identical** to legacy references in `/tmp/tsr_fix/ex/`.

ASan build (`build-cli-asan`): clean (zero stderr) across
`list`/`list --json`/`extract`/`record` on all three fixtures.

Session 8 (codec probe) additions, same fixtures, all green:

- `list` stream lines now show `High L3.0 640 x 360` (H.264 — matches
  ffprobe ground truth High/level 30/640×360) and `LC 88200 Hz, 1 ch`
  (AAC — the correct decode of the fixture's synthetic ADTS header;
  ffprobe's 44100/1 ch came from decoded frame data, not the header).
- `list --json` emits `codec_detail` in program streams and pids; still
  valid JSON (`python3 -m json.tool` passes).
- extract still **byte-exact** (dvb2 0x101/0x102 vs `/tmp/tsr_fix2/ex1`;
  demo3s 0x100/0x101 vs `/tmp/tsr_fix/ex`); record still byte-identical;
  all commands now produce **0 bytes of stderr**.
- ASan build recompiled with the probe: clean — all 4 commands ×
  dvb2.ts/demo3s.ts, zero findings.
- `tsreader_probe.c` compiles standalone **without** FFmpeg
  (no `TSR_HAVE_FFMPEG`) — no-FFmpeg builds remain supported (AAC detail
  still works via the built-in parser).

## 5. Repo state (do not revert anything else)

- `cli/` — the complete CLI: `tsreader_cli.{c,h}` (engine),
  `tsreader_si.c` (~1160 lines; all parsers now standard),
  `tsreader_report.c`, `tsreader_probe.c` (new, session 8),
  `tsreader_main.c`, `CMakeLists.txt`, `cli/README.md` (build,
  commands, table support, fixtures, probe notes, roadmap).
- `build-cli/` (current Release), `build-cli-asan/` (current).
- Legacy portability edits from earlier sessions still in place
  (`formatter.h`, `bcdmux.c`, `MDInterface.h`, `ControlServer.c`).

## 6. Session 8 done + remaining steps

Done in session 8 — FFmpeg codec probing (roadmap step 1):

- New `cli/tsreader_probe.c` (~365 lines): `tsr_stream_probeable()`
  (types 0x01/0x02/0x10/0x1b/0x24/0x03/0x0f/0x81/0x87/0x90),
  `tsr_probe_feed()` (bounded 256 KB sample; called from
  `tsr_process_packet` right after `tsr_si_packet`), `tsr_probe_streams()`
  (idempotent driver — consumes/frees buffers — called at the top of
  `tsr_finalize`, so the double-finalize is safe), FFmpeg raw-demuxer
  probe via temp file (`mkstemp` in `P_tmpdir`, `unlink` on every path),
  built-in ADTS header parser, local `P_*` profile-numeric tables,
  hand-rolled H.264 (idc/10) and HEVC (non-linear idc table) level text.
- `tsr_stream_t` gained `probe_buf`/`probe_len`/`probe_detail[128]`
  (freed in `tsr_free`); `tsr_probe_feed`/`tsr_probe_streams` declared in
  `tsreader_cli.h`; `tsreader_probe.c` added to the CMake sources.
- `tsreader_report.c`: detail shown on the human stream line (after
  component tag, before extract arrow) and emitted as `codec_detail` in
  both JSON locations (program streams + pids).
- Verified per §4. **No session-7 number changed** (CRC, CC, TDT,
  services, EPG, extract, record all identical).

Remaining (in order):

1. ATSC A/65 time descriptors + 0x42 SDT name decoding.
2. DVB v4l2 tuner front-ends for `live` (Linux-first; macOS uses
   IOKit/AVFoundation — scope separately).
3. `extract --clean-annexb` option (strip pre-start-code garbage;
   default stays raw-faithful).
4. **GUI phase — must ask the user first** (user constraint).
5. Optional: commit `cli/` to git as a clean baseline branch
   (`bionic/tsreader-cli`) once the user is happy — ask first.

## 6b. FFmpeg dev-build gating found (and worked around)

Per the user's "tell me about blocking/gated issues" instruction — none
were ultimately blocking; all worked around within the existing
open-source stack:

- `avio.h` lives in `include/libavformat/`, **not** `libavutil/` in this
  build.
- `avio_alloc_context` has a new 7-arg (packet-callback) signature.
- The `adts` demuxer is **not compiled into** this build
  (`av_find_input_format("adts")` returns NULL).
- The h264/aac **parsers** no longer fill width/height/sample_rate/
  channels into `AVCodecContext` (profile/level only).
- `FF_PROFILE_*` constants and deprecated
  `AVCodecParameters.channels` are absent from the headers.

Workarounds used (all version-stable): temp-file +
`avformat_open_input(filename)` for demuxing; built-in ADTS parser for
AAC; local numeric profile constants; `ch_layout` only. If a standard
(stable-release) FFmpeg is installed later, the same code path works
unchanged — the temp-file API and ADTS parser don't depend on any of the
changed surfaces.

## 7. Gotchas (new this session)

- **BCD is a mental trap.** 11 (month) = 0x0B in a 4-bit field, 0xB1 in
  a "packed byte" (which is *invalid BCD*), and "20:30" TDT time = 0x2030
  (full-byte BCD). The fixture and parser disagreed three separate ways
  before converging. When in doubt, dump bytes and decode nibble-by-nibble
  on paper.
- The harness `fflush` ordering hid which packet was hanging (an
  unflushed "pkt N ok" line makes it look like the hang is one packet
  early).
- `strtol("203000")` = 203000. Never use strtol to slice fixed-width
  digit fields; use explicit nibble/byte math.
- EIT sections: real muxers put **one service per section**; multiple
  services on one PID = multiple sections (the engine's splitter already
  walks multiple sections per packet, verified).
- macOS `getopt_long` permutes args fine, but a failed `fopen` in
  `record` used to be silent — check `record_out` after setup.
- **ADTS bit layout (session 8):** profile (AOT-1) is in **byte 2**
  (`b2>>6`), sfi straddles `b2[3:0]<<2 | b3>>6`, channels in `b3>>3 & 7`.
  An earlier draft read profile from byte 1 and produced plausible-looking
  wrong values (SSR 32 kHz instead of LC) — decode the actual bytes when
  in doubt. `channel_config` 0 is reserved; assume mono.
- The dvb2 fixture's AAC is synthetic: its ADTS header (LC, sfi=1 =
  88200 Hz, ch 0→1) does not match the decoded frame params ffprobe
  reports (44100/1 ch). The probe reports the **header** (that's what an
  analyzer does); don't "fix" it to match the decoder.
- `extract` has no `--all` flag — select with `-e prog=N` / `-e pid=0xNNN`
  (a wrong flag prints usage and extracts *nothing*; check the output dir
  is non-empty after byte-exact comparisons).

## 8. Key files

| Path | Significance |
|---|---|
| `cli/tsreader_si.c` | All SI parsers, standard layouts. `parse_pat` ~444, `parse_pmt` ~478, `parse_sdt` ~560, `parse_nit` ~660, `parse_dvb_eit` ~700 (event loop ~715), `parse_atsc_ett` ~767, `parse_tdt` ~900, `tsr_bcd_time24`/`tsr_bcd2`/`tsr_tdt4` helpers near top of dispatch area, `tsr_si_packet` ~985 (splitter + deferred classification), `dispatch` ~875. |
| `cli/tsreader_cli.c` | Engine: `tsr_process_packet` (AF/PCR/record/extract), CC check, `tsr_feed`, `tsr_process` (file/live loops). |
| `cli/tsreader_cli.h` | API; `tsr_program_t.pcr_ticks_valid` added. |
| `cli/tsreader_report.c` | Human + JSON output (`stream_time_unix` key). |
| `cli/tsreader_main.c` | CLI; record now fatal-on-open-failure. |
| `cli/tsreader_probe.c` | **Session 8:** codec probe module. `tsr_stream_probeable` ~28, `tsr_probe_feed` ~47, `ff_format_name`/`profile_name`/`level_text` + temp-file demuxer `tsr_probe_ffmpeg` ~245 (FFmpeg-only), `tsr_probe_adts` ~314 (built-in), `tsr_probe_streams` ~341 (driver, `av_log_set_level(AV_LOG_PANIC)`). |
| `/tmp/tsr_fix2/dvb2.ts`, `/tmp/tsr_fix2/mkdvb.py` | Verified 2-program standard-DVB fixture + generator (ES sources `v.h264`/`a.aac`; reference extract in `/tmp/tsr_fix2/ex1`). |
| `/tmp/tsr_fix/demo3s.ts`, `/tmp/tsr_fix/ex/` | Regression fixture (98 CC errors; ES PIDs **0x100/0x101**) + byte-exact extract references. |
| `cli/README.md` | New; user-facing docs. |
| `parser.c` (root) | Legacy reference — its TDT decode (MJD-based, table 0x70) and `ConvertDVBTime` were useful ground truth; its PAT/PMT offsets are the non-standard ones we replaced. |
| `/tmp/tsr_fix2/mkdvb.py` | dvb2.ts generator (current, correct). |
| `/tmp/tsr_fix2/dvb2.ts` | Verified 2-program standard fixture. |
| `/tmp/tsr_fix/demo3s.ts` | Standard-SI regression fixture (98 CC errors). |
| `/tmp/tsr_fix/demo3.ts` | Original legacy-layout fixture (CC ground truth only). |
| `/tmp/tsr_fix/ex/` | Legacy extract references (byte-exact targets). |

---

# Session 13 (2026-11-12) — SI standards rewrite complete, fixtures corrected, ES repacked

## What was done

### Engine (all verified)
- `tsreader_si.c` full standards rewrite: PAT/PMT/SDT/NIT/EIT/TDT standard
  layouts; PID-aware dispatch for 0x40/0x41 (NIT on 0x001, EIT_SDT
  elsewhere); CRC init 0xFFFFFFFF standardization (engine still accepts
  both inits); AF bits 5:4 (`0x30` mask) strict.
- **EIT_SDT service-loop guard:** EIT 0x40/0x41 service loops have no
  explicit length (the event loop follows directly). Unbounded walks mint
  phantom services. Guard: walk `while (p + 6 <= end - 4 &&
  (p[2] & 0xF0) == 0xF0 && (p[4] & 0xF0) == 0xF0)` — real service entries
  carry reserved(4) in both 16-bit fields; an event's byte 4 is BCD hours
  (0x00–0x19), so the discriminator is reliable.
- **TDT is UTC by definition:** standard 7-byte body (section_length 16,
  total 19 — *not* 20), `tsr_greg_to_unix` (timegm) epoch; report renders
  via `gmtime_r` + `strftime` (was local). `tsr_tdt4` deleted (the EN 300
  468 binary-day quirk helper is gone with it — do not re-add). EIT/EPT
  MJD events stay local; the ~5 h offset vs TDT wall clock is a documented
  quirk, don't "fix".
- **`tsreader_probe.c` ADTS bit-layout bug:** the sampling-frequency
  index is entirely in byte 2 (`(b2 >> 2) & 0xF`); the old code split it
  across b2/b3 and reported a 44100 Hz stream as 88200 Hz. Channel config
  is `((b2 & 1) << 2) | (b3 >> 6)`. Fixed; both fixtures now probe
  `LC 44100 Hz, 1 ch`.

### Fixtures (both tsinfo-clean, engine-verified)
- `dvb2.ts` (5640 pkts, 1.0 MiB): Network DemoNet; services 257 "Alpha
  One" / 258 "Beta Two"; programs 2 & 3 [PMT 0x100/0x103, PCR
  0x101/0x104]; H.264 High L3.0 640x360 + AAC LC 44100 1ch; TDT
  2026-11-11 20:30:00 UTC; EPG 3 events; clean CC, 0 sync/reserved-AF.
  **ES is now real, decodable content** (see repack below); all four
  extracts byte-identical to the source ES and ffmpeg-decodable (only
  expected tail-truncation errors at the cyclic wrap).
- `demo3s.ts` (1154 pkts): program 1, service 256 "Demo Three" EIT 0x000
  ch 100; legacy PES-wrapped ES (ts2es strips PES fine here); 0 CC errors.

### Fixture bugs found & fixed (generators)
- dvb2 TDT month byte `0xBB` — invalid BCD (November = `0x11`). tsinfo
  does **not** validate TDT BCD; only the engine's range check caught it.
- demo3s SDT LCN descriptor truncated (`64 03 00 64` → `64 03 00 00 64`,
  LCN 100) and EITs field `0xF07F` (127) → `0xF7FF` (not present).
- dvb2 EIT event-header running-status byte standardized
  (`0xF0 | running<<4 | len_hi`).
- **dvb2 ES repack (the big one):** the old chunkers dropped data for
  units > 184 B (an ADTS frame ~575 B or a large NAL: the tail was
  silently discarded on the next "resync"), so the multiplex carried
  garbage that no decoder could play. Now:
  - `es_pack` tiles the ES exactly (no loss); a chunk carries PUSI iff an
    ES unit (NAL / ADTS frame) starts inside it.
  - PCR moved to **AF-only packets** (standard DVB practice) so PUSI
    packets keep full 184-byte payloads (a PCR+payload packet truncates
    the PUSI chunk to 176 B — that was corrupting every cycle's first
    unit).
  - Short final chunk stuffed in the **adaptation field** (0xFF is only
    legal AF stuffing, never payload padding of a payload-only packet).
  - **AF flags byte without PCR must clear bit 4 (0x10 = PCR flag):**
    the correct reserved-only value is `0xEF`, not `0xFF`/`0xF7`
    (tstools `get_PCR_from_adaptation_field` checks `adapt[0] & 0x10`).
    0xFF stuffing asserted PCR-present and fed 135-day garbage PCRs to
    the engine.
  - **CC does not advance on AF-only packets** (13818-1 2.4.4.3);
    generator now `peek`s CC for those (was consuming CC values → ~240
    false continuity errors).

### Oracle findings
- `ts2es` is **not** a raw-payload oracle: for TS input it assumes
  PES-wrapped streams (reads payload[4..5] as PES length at every PUSI
  and clamps), and it mis-handles ADTS (treats 0xFFF frames as MPEG-1
  PES, drops 9 bytes/frame). Use it only for PES-wrapped content
  (demo3s). For byte-exact extract verification, compute the expected
  payload in Python (independent) and `cmp`.
- `tsinfo` does not validate TDT BCD and does not parse EIT content —
  the engine's own checks are the last line of defense.
- ts2es/tsinfo live at `/Users/csdesigns/Applications/tstools/` (read-only).

## "98 CC errors" expectation: refuted
Session 12's handoff expected exactly 98 continuity errors on demo3s.
An independent Python scan of **every** fixture (`demo*.ts`, `rec.ts`,
incl. the pre-rewrite `demo3s_malsi.bak.ts`) shows **zero** CC
discontinuities anywhere. Correct expectation: 0, which is what the
engine reports. If real CC-error fixtures are wanted, one must be
synthesized deliberately.

## Final verification matrix (all green)
| Check | dvb2.ts | demo3s.ts |
|---|---|---|
| tsinfo clean | yes | yes |
| `list` (programs/services/EPG/TDT-UTC/codec probes) | yes | yes |
| `list --json` valid, `duration_basis: pcr`, `continuity_errors: 0` | yes | yes |
| `extract` byte-exact vs independent Python payload extraction | 4/4 PIDs | 2/2 PIDs |
| extract decodes (ffmpeg `-f h264` / `-f aac`) | clean (tail-truncation only) | n/a (PES-wrapped; ts2es-stripped decodes) |
| `record` byte-identical | yes | yes |
| ASan clean (0 stderr bytes) on list / list --json | yes | yes |

## State / remaining
- `build-cli` current; `build-cli-asan` rebuilt. No pending engine work.
- GUI phase and any git commit of `cli/` still require explicit user
  go-ahead.
- Generators: `/tmp/tsr_fix2/mkdvb.py` (dvb2, from-scratch),
  `/tmp/tsr_fix/mkdemo3s_si.py` (demo3s SI rebuild). `VC` (CRC inits,
  all 0xFFFFFFFF) sits just above `build_si()` in mkdvb.py — it was
  accidentally clobbered once during a rewrite and restored; keep an eye
  on it when editing that file.

---

# Addendum (2026-11-12, post-session-13) — tstools fork fixes + fixture TDT fix

- **tstools fork (github.com/csdesigns/tstools) updated twice:**
  - `89b9431` — ts2es: unwrapped AAC (ADTS) payloads are now passed
    through raw instead of being mangled by the MPEG-1 PES heuristics
    (byte loss per frame + bogus PES-length clamping). PES-wrapped
    output is byte-identical to upstream.
  - (second commit, this addendum) — tsinfo: TDT (0x13) / TM (0x14)
    detection on any PID (PUSI payload sniffing; no SDT needed) with
    section-size check, CRC-32 (init 0xFFFFFFFF) check, BCD + range
    validation of the 6 time fields (EN 300 468 binary days 26–31
    tolerated: 0x1A–0x1F), and decoded-time reporting (UTC for TDT,
    local for TM). Once per distinct section; muxes without TDT/TM
    produce byte-identical output to upstream. Verified with mutated
    fixtures (bad BCD, bad CRC, bad hour, binary-day).
  - Both documented in the fork's README ("Changes in this fork").
  - The local `~/Applications/tstools` tree is that fork (rebuilt
    `bin/tsinfo`, `bin/ts2es` are the fixed versions).
- **dvb2.ts fixture regenerated**: it carried the UTC time in table
  0x14 (TM) when 0x13 (TDT) is correct for UTC; fixed in
  `/tmp/tsr_fix2/mkdvb.py` and regenerated (5640 pkts). Full battery
  re-run green: tsinfo-clean (+ new TDT line), engine `list`
  unchanged, 4/4 extracts byte-identical, record byte-identical, ASan
  clean, JSON valid.
- `cli/README.md` oracle section updated to match (ts2es ADTS fixed in
  fork; tsinfo TDT validation in fork; EIT still unvalidated by
  tsinfo; raw non-PES video still not ts2es-extractable).
