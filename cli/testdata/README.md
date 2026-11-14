# Test fixtures for the tsreader CLI

Fixtures used by `cli/selftest.sh`. With no inputs, the two `.ts` fixtures
below run by default.

## Fixtures

| File | What it is |
|---|---|
| `dvb2.ts` | Synthetic multi-program DVB fixture, 5640 packets. Program 2 (PMT 0x100): H.264 on 0x101 (PCR) + raw ADTS AAC on 0x102; program 3 (PMT 0x103): the same streams on 0x104 / 0x105. SI per EN 300 468 / 300 401: PAT 0x000, NIT 0x001, SDT 0x011, TDT 0x013 (UTC 2026-11-11 20:30:00), EIT 0x014. ES payloads are tiled in 184-byte chunks and cycled through 120 packet cycles, so PES start codes and headers straddle TS packet boundaries throughout. |
| `demo3s.ts` | Real-world ~3-second capture: H.264 (0x100, PCR) + AAC-in-PES (0x101). The canary for PES-wrapped ADTS. No TDT, so the selftest's TDT cross-check legitimately skips for it. |
| `mkdvb.py` | Generator for `dvb2.ts`. Deterministic: `python3 mkdvb.py <dir>` reads `v.h264` and `a.aac` from `<dir>` and writes `<dir>/dvb2.ts`. The committed `dvb2.ts` is byte-identical to the generator's output. |
| `v.h264`, `a.aac` | Source elementary streams for `dvb2.ts` (also the demo3s audio ground truth). |

## Ground-truth sidecars

`dvb2.truth/` and `demo3s.truth/` hold reference elementary streams named
`<PID as %03X>.es`. When present, the selftest additionally checks that
each extraction is a byte-exact **prefix** of the corresponding reference
(the muxes cycle the ES, so an extraction is a prefix, not the whole
stream; a reference that exactly matches is also a pass).

- `dvb2.truth/101.es`, `104.es` = `v.h264`. The video PIDs are
  H.222.0 PES-wrapped (one PES per access unit, declared lengths);
  unwrapping starts at the first video byte.
- `dvb2.truth/102.es`, `105.es` = `a.aac` repeated 6 times. These PIDs
  carry **unwrapped (raw) ADTS** cycled through the mux, so the
  reference must be the same tiling.
- `demo3s.truth/100.es`, `101.es` = the source video / audio elementary
  streams; both extractions match these exactly.

## Running

```sh
cli/selftest.sh                 # in-repo fixtures (default)
cli/selftest.sh /path/to/x.ts   # explicit input; the optional
                                # <input>.truth/ sidecar hook applies
                                # to any input
```
