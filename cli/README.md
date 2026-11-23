# tsreader — modern POSIX CLI

A modern, headless MPEG-2 transport stream analyzer for macOS and Linux,
rebuilt from the legacy Win32 TSReader engine. It is intentionally **not**
a 1:1 port: legacy proprietary plugin parsing has been replaced by
standard DVB/ATSC table layouts and an open-source-friendly architecture
(FFmpeg can be linked for codec probing; none of the original binary
plugins are used).

## Build

```sh
cmake -S cli -B build-cli
cmake --build build-cli
./build-cli/tsreader list input.ts
```

Optional ASan build (used for CI-style verification):

```sh
cmake -S cli -B build-cli-asan -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
cmake --build build-cli-asan
```

The project is self-contained C11; it does not touch the legacy MSVC
`CMakeLists.txt` at the repository root.

## Commands

```
tsreader list [--json] [--no-epg] [--tables] input.ts
tsreader extract -e prog=N [-e pid=0xNNN ...] -o DIR/ input.ts
tsreader record -o out.ts input.ts
tsreader live [-u [addr:port]] [-t [addr:port]] [-i iface]
tsreader list  -H host[:port]/tune-url [--duration N]
tsreader record -H host[:port]/tune-url --duration N -o out.ts
```

- `list` — mux overview: network name (NIT), stream time (TDT), services
  with LCNs (SDT), programs with PMT/PCR PIDs, per-PID roles and
  bitrates, codec details (when FFmpeg is available, see below),
  continuity/sync error counts, and an EPG (EIT). `--json`
  emits a machine-readable form (`codec_detail` per stream); `--tables`
  dumps every parsed SI section (hex) for debugging.
- `extract` — writes raw elementary-stream payloads for the selected
  programs/PIDs (byte-faithful: adaptation fields stripped, no
  re-encoding, no start-code cleanup).
- `record` — raw passthrough copy of the transport stream.
- `live` — UDP multicast / TCP ingress (for capture front-ends).
- `-H host[:port]/tune-url` — live **HDHomeRun** (SiC) input; works with any
  command (default port 5004; see [HDHomeRun](#hdhomerun-sic-input)).

## Supported SI tables (standard layouts)

| Table | IDs | Notes |
|---|---|---|
| PAT | 0x00 | TSI + ONID, standard loop at byte 10 |
| PMT | 0x02 | program_number @ 6, PCR/ES loops; registry names for stream types |
| SDT | 0x11 / 0xFF | service names (0x48), LCN (0x64), EIT PIDs |
| ATSC A/65 SDT | 0x42 | recognized/ counted; name decoding is a follow-up |
| NIT | 0x40 / 0x41 | network name (0x40) + provider (0x48), network loop |
| EIT (DVB) | 0x46 / 0x47 | **one service per section** (per EN 300 468); short/long event (0x4D/0x54), now (0x80) / next (0x82) flags; MJD + BCD times |
| ETT (A/65) | 0x70–0x75 | program names + description descriptors |
| TDT/TM | 0x13 / 0x14 | BCD date/time, **UTC by definition** → `stream_time_unix` (rendered UTC) |

Notes on decoding choices:

- Section CRC-32 (MSB-first, poly 0x04C11DB7) is checked with both
  initial values (0x00000000 / 0xFFFFFFFF), since real multiplexers use
  either. Sustained CRC failure on a PID demotes it from SI (this is how
  H.264 streams on non-PAT/PMT PIDs are correctly not misclassified).
- Continuity-counter errors are counted per PID; the check is gated on
  payload packets so pure adaptation packets do not false-positive on
  15→0 wraps.
- PCR (90 kHz) tracking drives duration/bitrate when present; otherwise
  the report falls back to wall clock and says so.
- TDT times are UTC by definition; the report renders the TDT wall
  clock with `gmtime_r` and stamps `stream_time_unix`. EIT/EPT (MJD +
  BCD) event times are rendered in local time — the resulting offset
  between the two is a property of the content, not a bug.
- The built-in ADTS header reader (no FFmpeg needed) follows the
  13818-7 fixed-header layout: the sampling-frequency index is entirely
  in byte 2 (`(b2 >> 2) & 0xF`) and channel config spans b2/b3.

### Standards oracle (tstools)

Development verification cross-checks against [tstools](
https://code.videolan.org/videolan/tstools) (`tsinfo`, `ts2es`,
`esreport`), kept read-only at `~/Applications/tstools`. Known oracle
limits found in practice:

- `tsinfo` does not parse EIT content at all. Our fork
  (csdesigns/tstools, 2026-11-12) adds TDT/TM detection with BCD +
  range + CRC validation and time reporting; upstream builds remain
  silent on invalid TDTs, so engine-side range checks are still the
  last line of defense for SI-time correctness.
- `ts2es` assumes PES-wrapped streams for TS input (it reads the bytes
  after PUSI as a PES length and clamps output to it). Unwrapped AAC
  (ADTS) used to be mangled by that logic (byte loss per frame +
  bogus PES-length clamping); our fork (csdesigns/tstools, 
  2026-11-12) now passes ADTS payloads through raw, and its README
  documents the change. Raw (non-PES) *video* such as Annex B H.264
  is still not extracted faithfully by any build — for such
  multiplexes, verify extract with an independent byte-level payload
  extraction.
- Raw 0x81 (AC-3/DTS) content: ts2es drops short byte runs around
  incidental `00 00 01 xx` start-code-like sequences inside the raw
  audio bitstream (and pure `0xFF` filler runs); the engine keeps the
  payload byte-faithful, so the two can differ by exactly those runs on
  affected multiplexes (observed in the wild: 124 × 7 B
  `00 00 01 BD` chunks on a 20 s ATSC capture). `selftest.sh`
  classifies this: when the oracle output is a byte subsequence of the
  engine's and every removed segment is one of the known quirk patterns,
  the extract check reports WARN (not FAIL); any other divergence fails.
- The engine's extract is raw payload bytes (PES where the mux carries
  PES, raw Annex B / ADTS where it doesn't) — byte-exact by design.

## Verification fixtures

Development fixtures (kept in `/tmp`, regenerated by the scripts
documented in the session handoff notes):

- `dvb2.ts` — 2-program standard-DVB multiplex (PAT/NIT/PMT/SDT/TDT/EIT,
  H.264 High + AAC LC, 5640 packets): fully standards-compliant SI
  (tsinfo-clean), real decodable ES (PUSI at unit boundaries, PCR on
  AF-only packets), clean continuity. All SI, EPG, extract
  (byte-exact vs an independent extraction; outputs decode with
  FFmpeg) and record (byte-identical passthrough) checks pass.
- `demo3.ts` / `demo3s.ts` — legacy 1-program multiplex; `demo3s.ts`
  carries the same PES-wrapped ES with standard-layout SI and is the
  regression fixture for SI parsing + extraction. No fixture in the set
  contains continuity discontinuities (an earlier handoff's "98 CC
  errors" expectation was refuted by an independent scan); a
  deliberate CC-error fixture should be synthesized if that regression
  is wanted.

ASan builds run clean across `list`, `list --json`, `extract`, and
`record` on all fixtures.

## Codec probing (optional, FFmpeg)

When FFmpeg is found at configure time (Homebrew prefix, then pkg-config),
each ES stream line in `list` shows a codec detail (and `--json` emits
`codec_detail`), e.g. `High L3.0 640 x 360` or `LC 96000 Hz, 2 ch`.

- The engine keeps a bounded 256 KB payload sample per probeable ES
  (H.264, HEVC, MPEG-2, MPEG-4, MP3, AAC, AC-3, E-AC-3) and probes it at
  finalization:
  - Video / MP3 / AC-3 / E-AC-3 go through FFmpeg raw demuxers
    (`h264`/`hevc`/`mpegvideo`/`mpeg4`/`mp3`/`ac3`/`eac3`) opened from a
    temp file with the stable `avformat_open_input(filename)` API —
    uniform across FFmpeg versions, unlike the AVIO/decoder APIs which
    have churned signatures (and, in some builds, relocated headers).
  - AAC (ADTS) is parsed by a small built-in ADTS header reader that
    needs no FFmpeg at all — it also covers builds whose `adts`
    demuxer is not compiled in, and builds without FFmpeg entirely.
- Probing is strictly best-effort: a failed/missing probe leaves the
  detail empty and never affects analysis, `extract`, or `record`.
- CMake prints which mode it chose
  (`tsreader: FFmpeg found — enabling codec probe` / `… not found —
  codec probe disabled`); builds without FFmpeg remain fully supported.

## HDHomeRun (SiC) input

`-H host[:port]/tune-url` receives a live stream straight from an
HDHomeRun device (default port 5004), natively — no relay:

```sh
tsreader list   -H 192.168.1.16/auto/v5.1 --duration 10
tsreader record -H 192.168.1.16/atsc://12/12.1 --duration 30 -o rec.ts
tsreader extract -H 192.168.1.16/qam:65 -e prog=2 -o out/
tsreader list -H http://192.168.1.16:5004/auto/v5.1   # http:// prefix optional
```

- Uses the standard SiC HTTP API. Two firmware generations are handled,
  tried in order:
  1. **Direct tune-URL streaming** (older firmware and front-ends):
     `GET /<tune-url>` — the tune URL doubles as the stream URL; the
     first byte of a live stream is the TS sync byte `0x47`, anything
     else aborts this attempt. Closing the connection releases the
     tuner.
  2. **Global API** (newer firmware): `POST /tune` with
     `{"url": "<tune-url>"}` → `GET /transport_stream/<id>`, then
     `DELETE /tuner/<id>` when reading stops.

  Tuner release is best-effort in both paths: a device that drops the
  connection leaves nothing to release. A slow auto-scan does not eat
  into `--duration` — the duration clock starts at the first stream
  byte.
- The tune URL is passed to the device **unchanged**, so all SiC URL
  forms work: `auto/<tuner>` (auto-scan), `atsc://M/M.S`,
  `8vsb:<hz>` / `qam:<n>` (legacy frequency style), `dvbs://…`.
  Auto-scan tunes can take tens of seconds; the tune request allows 60 s.
- A stream that goes silent (e.g. signal loss) for 15 s aborts cleanly
  instead of hanging.
- Optional dependency: libcurl (in the macOS SDK; on Linux install the
  distro's `libcurl` development package). CMake prints which mode was
  chosen (`tsreader: libcurl found — enabling HDHomeRun source`); builds
  without libcurl remain fully supported, and `-H` then reports that the
  binary was built without libcurl.
- IPv6 host literals are not supported (documented limitation).
- `selftest.sh --hdhr <URL>` records N seconds (`--hdhr-secs`, default
  10) and then runs the full verification matrix on the capture.

## Roadmap (not yet implemented)

- ATSC A/65 time descriptors and 0x42 SDT name decoding.
- DVB v4l2 tuner front-ends for `live`.
- Optional `extract --clean-annexb` to strip pre-start-code garbage from
  legacy content (default stays raw-faithful).
- A modern GUI front-end is a separate, later phase.
