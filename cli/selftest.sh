#!/usr/bin/env bash
# =============================================================================
# selftest.sh — verification matrix for the tsreader CLI
#
# Rebuilds the CLI from the sources in this directory and runs the checks
# used to validate the port:
#
#   list      exits 0 on the input
#   json      `list --json` produces parseable JSON
#   oracle TDT  the UTC time reported by tstools `tsinfo` appears in `list`
#   extract   ES output byte-identical to tstools `ts2es` for every
#             PMT-declared elementary PID
#   record    output byte-identical to the input (pass-through round trip)
#
# Every check is run twice: a Release build, and an AddressSanitizer build
# where stderr must additionally be completely empty.
#
# The "oracle" is a tstools build (tsinfo + ts2es) — for this port, the
# csdesigns/tstools fork with the TDT validation and ADTS ts2es fixes.
# Auto-detected from $ORACLE, $TSTOOLS, ~/Applications/tstools/bin,
# ~/tstools/bin or PATH.
#
# Usage:
#   cli/selftest.sh [options] [<input.ts> ...]
#
# With no inputs, runs the in-repo fixtures in testdata/ (their
# <input>.truth/ ground-truth sidecars are picked up automatically).
#
# Options:
#   --build-dir DIR  shared scratch/build dir (default: fresh mktemp -d)
#   --keep           keep the scratch dir (builds + intermediates) on success
#   --skip-build     use prebuilt binaries already in --build-dir/{rel,asan}
#   --no-asan        skip the AddressSanitizer build/run
#   --no-extract     skip the extract-vs-oracle check
#   --no-record      skip the record round-trip check
#   --oracle DIR     dir containing tsinfo and ts2es
#   --no-oracle      skip all oracle differential checks
#   --hdhr URL       record an HDHomeRun source (host[:port]/tune-url, e.g.
#                    192.168.1.16/auto/v5.1) for HDHR_SECS seconds, then run
#                    the full matrix on the capture (replaces the file list)
#   --hdhr-secs N    capture duration for --hdhr (default 10)
#   -h, --help       this text
#
# Exit status: 0 = all checks passed (WARN/SKIP allowed), 1 = any FAIL.
# The scratch dir is kept on failure for inspection.
#
# Shell: POSIX-ish bash, compatible with macOS /bin/bash 3.2.
# =============================================================================
set -u

LC_ALL=C
export LC_ALL

SRCDIR=$(cd "$(dirname "$0")" && pwd)

BUILD_DIR=""
KEEP=0
SKIP_BUILD=0
DO_ASAN=1
DO_EXTRACT=1
DO_RECORD=1
ORACLE_DIR=""
USE_ORACLE=1
HDHR_URL=""
HDHR_SECS=10
FILES=()

usage() {
  sed -n '/^# Usage:/,/^#   -h, --help/p' "$0" | sed 's/^# \{0,1\}//'
}

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir)  BUILD_DIR=$2; shift 2 ;;
    --keep)       KEEP=1; shift ;;
    --skip-build) SKIP_BUILD=1; shift ;;
    --no-asan)    DO_ASAN=0; shift ;;
    --no-extract) DO_EXTRACT=0; shift ;;
    --no-record)  DO_RECORD=0; shift ;;
    --oracle)     ORACLE_DIR=$2; shift 2 ;;
    --no-oracle)  USE_ORACLE=0; shift ;;
    --hdhr)       HDHR_URL=$2; shift 2 ;;
    --hdhr-secs)  HDHR_SECS=$2; shift 2 ;;
    -h|--help)    usage; exit 0 ;;
    -*)  echo "unknown option: $1" >&2; exit 2 ;;
    *)   FILES+=("$1"); shift ;;
  esac
done

if [ ${#FILES[@]} -eq 0 ]; then
  # No inputs: fall back to the in-repo fixtures (testdata/).
  for f in "$SRCDIR/testdata/dvb2.ts" "$SRCDIR/testdata/demo3s.ts"; do
    [ -r "$f" ] && FILES+=("$f")
  done
  if [ ${#FILES[@]} -eq 0 ]; then
    usage
    echo >&2
    echo "error: no input .ts file given and no fixtures in $SRCDIR/testdata/" >&2
    exit 2
  fi
fi

# -----------------------------------------------------------------------------
# scratch area
# -----------------------------------------------------------------------------
if [ -z "$BUILD_DIR" ]; then
  BUILD_DIR=$(mktemp -d "${TMPDIR:-/tmp}/tsr-selftest.XXXXXX") || exit 1
fi
mkdir -p "$BUILD_DIR" || exit 1

ncpu=$( (sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4) | tr -d '[:space:]' )

# -----------------------------------------------------------------------------
# build
# -----------------------------------------------------------------------------
build_release() {
  cmake -S "$SRCDIR" -B "$BUILD_DIR/rel" -DCMAKE_BUILD_TYPE=Release \
      > "$BUILD_DIR/cfg-rel.log" 2>&1 \
    || { echo "FAIL cmake configure (release) — see $BUILD_DIR/cfg-rel.log"; exit 1; }
  cmake --build "$BUILD_DIR/rel" -j"$ncpu" \
      > "$BUILD_DIR/build-rel.log" 2>&1 \
    || { echo "FAIL build (release) — see $BUILD_DIR/build-rel.log"; exit 1; }
}

build_asan() {
  cmake -S "$SRCDIR" -B "$BUILD_DIR/asan" -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
      > "$BUILD_DIR/cfg-asan.log" 2>&1 \
    || { echo "note: cmake configure (asan) failed — skipping asan pass"; DO_ASAN=0; return; }
  cmake --build "$BUILD_DIR/asan" -j"$ncpu" \
      > "$BUILD_DIR/build-asan.log" 2>&1 \
    || { echo "note: asan build failed — skipping asan pass"; DO_ASAN=0; return; }
}

if [ "$SKIP_BUILD" -eq 1 ]; then
  if [ ! -x "$BUILD_DIR/rel/tsreader" ]; then
    echo "FAIL --skip-build: $BUILD_DIR/rel/tsreader not found"
    exit 1
  fi
  if [ "$DO_ASAN" -eq 1 ] && [ ! -x "$BUILD_DIR/asan/tsreader" ]; then
    echo "note: no asan binary in --build-dir — skipping asan pass"
    DO_ASAN=0
  fi
else
  build_release
  if [ "$DO_ASAN" -eq 1 ]; then build_asan; fi
fi

REL="$BUILD_DIR/rel/tsreader"
ASAN="$BUILD_DIR/asan/tsreader"

# -----------------------------------------------------------------------------
# oracle detection
# -----------------------------------------------------------------------------
TSINFO=""
TS2ES=""
detect_oracle() {
  if [ "$USE_ORACLE" -eq 0 ]; then return; fi
  local d
  local candidates=()
  [ -n "$ORACLE_DIR" ] && candidates+=("$ORACLE_DIR")
  [ -n "${ORACLE:-}" ] && candidates+=("$ORACLE")
  [ -n "${TSTOOLS:-}" ] && candidates+=("$TSTOOLS/bin")
  candidates+=("$HOME/Applications/tstools/bin" "$HOME/tstools/bin")
  for d in "${candidates[@]}"; do
    [ -x "$d/tsinfo" ] && TSINFO="$d/tsinfo"
    [ -x "$d/ts2es" ] && TS2ES="$d/ts2es"
  done
  [ -n "$TSINFO" ] || TSINFO=$(command -v tsinfo 2>/dev/null || true)
  [ -n "$TS2ES" ] || TS2ES=$(command -v ts2es 2>/dev/null || true)
  if [ -z "$TSINFO" ] && [ -z "$TS2ES" ]; then
    echo "note: no tstools oracle found (--oracle DIR or TSTOOLS) — oracle checks skipped"
    USE_ORACLE=0
  fi
}
detect_oracle

# -----------------------------------------------------------------------------
# reporting
# -----------------------------------------------------------------------------
NPASS=0; NFAIL=0; NWARN=0; NSKIP=0
report() { # $1=status $2=file $3=check $4=detail(optional)
  case "$1" in
    PASS) NPASS=$((NPASS+1)) ;;
    FAIL) NFAIL=$((NFAIL+1)) ;;
    WARN) NWARN=$((NWARN+1)) ;;
    SKIP) NSKIP=$((NSKIP+1)) ;;
  esac
  printf '[%s] %s :: %s\n' "$1" "$2" "$3"
  if [ -n "${4:-}" ]; then printf '       %s\n' "$4"; fi
}

echo "tsreader selftest"
echo "  sources : $SRCDIR"
echo "  scratch : $BUILD_DIR"
if [ "$USE_ORACLE" -eq 1 ]; then
  echo "  oracle  : tsinfo=${TSINFO:-missing} ts2es=${TS2ES:-missing}"
else
  echo "  oracle  : disabled"
fi
if [ -n "$HDHR_URL" ]; then
  echo "  hdhr    : $HDHR_URL (${HDHR_SECS}s capture)"
fi
echo

# -----------------------------------------------------------------------------
# optional live HDHomeRun capture
# -----------------------------------------------------------------------------
if [ -n "$HDHR_URL" ]; then
  CAP="$BUILD_DIR/hdhr_capture.ts"
  if [ ! -x "$REL" ]; then
    report FAIL "hdhr_cap.ts" "hdhr capture" "release build missing"
  else
    "$REL" record -H "$HDHR_URL" --duration "$HDHR_SECS" -o "$CAP" \
      > "$BUILD_DIR/hdhr_rec.log" 2>&1
    rc=$?
    if [ "$rc" -ne 0 ]; then
      report FAIL "hdhr_cap.ts" "hdhr capture" \
        "record rc=$rc (device reachable?) — see $BUILD_DIR/hdhr_rec.log"
      sed -n '1,8p' "$BUILD_DIR/hdhr_rec.log" | sed 's/^/       /'
    else
      csize=$( [ -s "$CAP" ] && wc -c < "$CAP" | tr -d ' ' )
      [ -n "$csize" ] || csize=0
      if [ "$csize" -gt 0 ] && [ $((csize % 188)) -eq 0 ]; then
        report PASS "hdhr_cap.ts" "hdhr capture" "$csize B from $HDHR_URL"
        FILES=("$CAP")
      else
        report FAIL "hdhr_cap.ts" "hdhr capture" \
          "bad capture size=${csize} (not a positive multiple of 188)"
      fi
    fi
  fi
  echo
fi

# -----------------------------------------------------------------------------
# per-file checks
# -----------------------------------------------------------------------------
# Globals set by run_file, used by run_pass: FILE, NAME, W, PIDLIST
FILE=""; NAME=""; W=""
PIDLIST=()

run_pass() { # $1=label  $2=binary  $3=strict(1 = stderr must be empty)
  tag="$1"; bin="$2"; strict="$3"
  if [ ! -x "$bin" ]; then
    report SKIP "$NAME" "$tag" "no binary"
    return
  fi

  # --- list -----------------------------------------------------------------
  "$bin" list "$FILE" > "$W/${tag}_list.out" 2> "$W/${tag}_list.err"
  rc=$?
  if [ "$rc" -eq 0 ] && { [ "$strict" -eq 0 ] || [ ! -s "$W/${tag}_list.err" ]; }; then
    report PASS "$NAME" "$tag list"
  else
    report FAIL "$NAME" "$tag list" "rc=$rc; stderr follow"
    sed -n '1,12p' "$W/${tag}_list.err" | sed 's/^/       /'
  fi

  # --- json -----------------------------------------------------------------
  if command -v python3 >/dev/null 2>&1; then
    if "$bin" list --json "$FILE" 2>/dev/null | python3 -m json.tool > /dev/null 2>&1; then
      report PASS "$NAME" "$tag json"
    else
      report FAIL "$NAME" "$tag json" "list --json is not valid JSON"
    fi
  else
    report SKIP "$NAME" "$tag json" "no python3"
  fi

  # Known ts2es quirk (documented in README, "Standards oracle"): for raw
  # 0x81 (AC-3/DTS) content, ts2es drops short byte runs around incidental
  # 00 00 01 xx start-code-like sequences and 0xFF filler runs. When the
  # oracle output is a byte subsequence of ours and every removed segment is
  # one of those patterns, that PID is counted as a quirk (WARN overall),
  # not a failure.
  check_quirk() { # $1=ours $2=ref ; prints removed bytes, rc 0 = quirk only
    python3 - "$1" "$2" <<'PYEOF'
import sys
a = open(sys.argv[1], "rb").read()
b = open(sys.argv[2], "rb").read()
if len(b) > len(a):
    sys.exit(1)  # we lack bytes the oracle kept: not the known quirk
i = j = 0
removed = 0
bad = False
while i < len(a) and j < len(b):
    if a[i] == b[j]:
        i += 1; j += 1
        continue
    k = a.find(b[j:j + 16], i)
    if k < 0:
        seg = a[i:]; i = len(a)
    else:
        seg = a[i:k]; i = k
    removed += len(seg)
    head = seg[:8]
    filler = seg.strip(b"\xff") in (b"", b"\x21\x00", b"\x00\x21") and len(seg) < 64
    if b"\x00\x00\x01" not in head and not filler:
        bad = True
        break
if bad or j != len(b):
    sys.exit(1)
print(removed)
sys.exit(0)
PYEOF
  }

  # --- extract vs oracle ------------------------------------------------------
  if [ "$DO_EXTRACT" -eq 1 ]; then
    if [ "$USE_ORACLE" -eq 1 ] && [ -n "$TS2ES" ] && [ ${#PIDLIST[@]} -gt 0 ]; then
      bad=0; quirk=0; quirkb=0; detail=""
      for pid in "${PIDLIST[@]}"; do
        hex=$(printf '0x%x' "$pid")
        d="$W/${tag}_ext_$hex"
        rm -rf "$d"; mkdir -p "$d"
        "$bin" extract "$FILE" -e "pid=$hex" -o "$d" > "$W/${tag}_ext_$hex.log" 2>&1
        # Output files are extract_<codec>_<PID as %03X>.es - match on the
        # PID suffix, not glob order.
        suff="_$(printf '%03X' "$pid").es"
        ours=""
        for f in "$d"/*.es; do
          [ -e "$f" ] || continue
          case "$f" in *"$suff") ours="$f" ;; esac
        done
        ref="$W/ref_$hex"
        osize=$( [ -n "$ours" ] && wc -c < "$ours" | tr -d ' ' )
        [ -n "$osize" ] || osize=0
        rsize=$( [ -s "$ref" ] && wc -c < "$ref" | tr -d ' ' )
        [ -n "$rsize" ] || rsize=0
        if [ -n "$ours" ]; then
          if [ -s "$ref" ] && cmp -s "$ours" "$ref"; then
            :
          elif [ -s "$ref" ] && command -v python3 >/dev/null 2>&1 \
               && qb=$(check_quirk "$ours" "$ref"); then
            quirk=1
            quirkb=$((quirkb + qb))
          else
            bad=1
            detail="${detail:+$detail; }pid $hex (${osize} B): differs from ts2es (${rsize} B) or oracle found no data"
          fi
        elif [ -s "$ref" ]; then
          bad=1
          detail="${detail:+$detail; }pid $hex: produced no output, ts2es produced ${rsize} B"
        fi
        # Optional ground-truth prefix check (sidecar <input>.truth/).
        if [ -n "$ours" ] && [ -d "$TRUTHDIR" ] && [ -f "$TRUTHDIR/$(printf '%03X' "$pid").es" ] \
           && command -v python3 >/dev/null 2>&1; then
          if ! python3 -c '
import sys
a = open(sys.argv[1], "rb").read()
b = open(sys.argv[2], "rb").read()
sys.exit(0 if b.startswith(a) else 1)
' "$ours" "$TRUTHDIR/$(printf '%03X' "$pid").es" 2>/dev/null; then
            bad=1
            detail="${detail:+$detail; }pid $hex: not a byte-exact prefix of ground truth"
          fi
        fi
      done
      if [ "$bad" -eq 0 ] && [ "$quirk" -eq 0 ]; then
        report PASS "$NAME" "$tag extract" "byte-identical to ts2es (${#PIDLIST[@]} PIDs)"
      elif [ "$bad" -eq 0 ]; then
        report WARN "$NAME" "$tag extract" \
          "matches ts2es except ${quirkb} B of known 0x81 quirk (oracle drops start-code/FILLER runs)"
      else
        report FAIL "$NAME" "$tag extract" "$detail"
      fi
    else
      report SKIP "$NAME" "$tag extract" \
        "oracle/ts2es unavailable or no PMT streams found"
    fi
  fi

  # --- record round trip ------------------------------------------------------
  if [ "$DO_RECORD" -eq 1 ]; then
    rm -f "$W/${tag}_rec.ts"
    "$bin" record "$FILE" -o "$W/${tag}_rec.ts" \
      > "$W/${tag}_rec.log" 2> "$W/${tag}_rec.err"
    rc=$?
    if [ "$rc" -eq 0 ] && cmp -s "$FILE" "$W/${tag}_rec.ts"; then
      report PASS "$NAME" "$tag record" "round-trip byte-identical"
    else
      report FAIL "$NAME" "$tag record" "rc=$rc; output differs from input"
      sed -n '1,8p' "$W/${tag}_rec.err" | sed 's/^/       /'
    fi
  fi
}

run_file() {
  i="$1"; file="$2"
  FILE="$file"
  NAME=$(basename "$file")
  W="$BUILD_DIR/work/$i-$NAME"
  mkdir -p "$W"
  PIDLIST=()
  # Optional ground truth: a sidecar dir named <input>.truth/ holding
  # <PID as %03X>.es reference streams. When present, each extraction is
  # additionally checked to be an exact prefix of its reference.
  TRUTHDIR="$(dirname "$file")/$(basename "$file" .ts).truth"

  if [ ! -r "$file" ]; then
    report FAIL "$NAME" "input" "not readable"
    return
  fi

  # --- oracle references (once per file, not per binary) ----------------------
  if [ "$USE_ORACLE" -eq 1 ]; then
    if [ -n "$TS2ES" ] && [ "$DO_EXTRACT" -eq 1 ] \
       && command -v python3 >/dev/null 2>&1; then
      "$REL" list --json "$file" > "$W/list.json" 2> "$W/list.json.err"
      if python3 -c '
import json, sys
d = json.load(open(sys.argv[1]))
seen = []
for p in d.get("programs", []):
    for s in p.get("streams", []):
        pid = s.get("pid")
        if pid is not None and pid not in seen:
            seen.append(pid)
for pid in seen:
    print(pid)
' "$W/list.json" > "$W/pids.txt" 2>/dev/null; then
        while IFS= read -r pid; do
          [ -n "$pid" ] && PIDLIST+=("$pid")
        done < "$W/pids.txt"
      else
        echo "note: $NAME — could not parse list --json for PID enumeration"
      fi
      for pid in ${PIDLIST[@]+"${PIDLIST[@]}"}; do
        hex=$(printf '0x%x' "$pid")
        "$TS2ES" -pid "$hex" -stdin -stdout < "$file" \
          > "$W/ref_$hex" 2> "$W/ref_${hex}.err"
      done
    elif [ "$DO_EXTRACT" -eq 1 ]; then
      echo "note: $NAME — extract check needs ts2es + python3; skipped"
    fi

    if [ -n "$TSINFO" ]; then
      "$TSINFO" "$file" > "$W/tsinfo.txt" 2>&1
      # BRE, not -E: some sed builds misbehave with -E capture groups.
      tdt=$(sed -n 's/^TDT time (.*): \(.*\) UTC$/\1/p' \
            "$W/tsinfo.txt" | head -n 1)
      [ -n "$tdt" ] && printf '%s\n' "$tdt" > "$W/oracle_tdt"
    fi
  fi

  # --- release pass -----------------------------------------------------------
  run_pass rel "$REL" 0

  # --- oracle TDT cross-check (release output) --------------------------------
  if [ "$USE_ORACLE" -eq 1 ] && [ -n "$TSINFO" ]; then
    if [ -f "$W/oracle_tdt" ]; then
      tdt=$(cat "$W/oracle_tdt")
      if [ -f "$W/rel_list.out" ] && grep -qF "$tdt" "$W/rel_list.out"; then
        report PASS "$NAME" "oracle TDT" "$tdt (matches tsinfo)"
      else
        report WARN "$NAME" "oracle TDT" \
          "tsinfo first-TDT '$tdt' not found in list output (tsinfo shows the first TDT, list may show a later one) — inspect: $W/rel_list.out"
      fi
    else
      report SKIP "$NAME" "oracle TDT" "no TDT in input"
    fi
  fi

  # --- asan pass ----------------------------------------------------------------
  if [ "$DO_ASAN" -eq 1 ]; then
    run_pass asan "$ASAN" 1
  fi
}

idx=0
for f in "${FILES[@]}"; do
  idx=$((idx+1))
  run_file "$idx" "$f"
done

# -----------------------------------------------------------------------------
# summary
# -----------------------------------------------------------------------------
echo
echo "================ SUMMARY ================"
printf 'pass=%d  warn=%d  skip=%d  fail=%d\n' "$NPASS" "$NWARN" "$NSKIP" "$NFAIL"
if [ "$NFAIL" -gt 0 ]; then
  echo "RESULT: FAIL"
  echo "scratch kept for inspection: $BUILD_DIR"
  exit 1
fi
echo "RESULT: ALL PASS"
if [ "$KEEP" -eq 0 ]; then
  rm -rf "$BUILD_DIR"
else
  echo "scratch kept: $BUILD_DIR"
fi
exit 0
