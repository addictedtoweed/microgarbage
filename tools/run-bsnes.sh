#!/usr/bin/env bash
# ============================================================
#  run-bsnes.sh -- start bsnes-plus on a microgarbage SNES ROM.
#
#  MSYS/MinGW companion to run-bsnes.ps1. Same defaults, same flags
#  in long-option form. The bsnes binary is still a Windows .exe in
#  the MSYS world, so we just invoke it through MSYS path
#  conventions and let CMD-style argv pass through.
#
#  Usage:
#    ./tools/run-bsnes.sh                       # boot kernel (demos via PuTTY)
#    ./tools/run-bsnes.sh --smoke               # legacy 65816 SELECT DEMO menu
#    ./tools/run-bsnes.sh --bsnes-dir /c/bsnes  # explicit bsnes-plus location
#    ./tools/run-bsnes.sh --rom path/to.sfc     # arbitrary ROM file
#    ./tools/run-bsnes.sh --no-build            # skip snes/build.ps1
#    ./tools/run-bsnes.sh --trace               # MG_DMA_TRACE=1 -> mgdma.log
#    ./tools/run-bsnes.sh --trace --trace-log foo.log    # custom log path
#
#  Resolution order for bsnes (first match wins):
#    1. --bsnes-dir argument
#    2. $BSNES_HOME env var
#    3. <repo>/bsnes-plus, <repo>/../bsnes-plus, /c/Program Files/bsnes-plus
#    4. bsnes.exe / bsnes-plus.exe on PATH
#
#  Public domain (CC0). No warranty.
# ============================================================
set -euo pipefail

step()  { printf 'run-bsnes: %s\n'        "$*"; }
die()   { printf 'run-bsnes: ERROR -- %s\n' "$*" >&2; exit 1; }

# Repo root = parent of the directory holding this script.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
step "repo: $REPO_ROOT"

# ---- 1. Parse args ------------------------------------------
BSNES_DIR=""
ROM=""
SMOKE=0
NO_BUILD=0
TRACE=0
CAPTURE=0
TRACE_LOG="mgdma.log"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --bsnes-dir) BSNES_DIR="$2"; shift 2 ;;
        --rom)       ROM="$2";       shift 2 ;;
        --smoke)     SMOKE=1;        shift   ;;
        --boot)      SMOKE=0;        shift   ;;  # backward-compat alias for the default
        --no-build)  NO_BUILD=1;     shift   ;;
        --trace)     TRACE=1;        shift   ;;
        --trace-log) TRACE_LOG="$2"; shift 2 ;;
        # Capture the child's stderr to a file WITHOUT the MG_DMA_TRACE
        # firehose — for lightweight diagnostics (e.g. the once/sec
        # "fmv-audio:" lines) where the per-frame DMA trace would both
        # bury the signal and perturb the timing being measured.
        --log)       TRACE_LOG="$2"; CAPTURE=1; shift 2 ;;
        -h|--help)
            sed -n '3,20p' "$0"; exit 0 ;;
        *) die "unknown arg: $1 (use -h for usage)" ;;
    esac
done

# DMA-slot trace: gated on --trace. Bsnes is a GUI process and its
# stderr lands nowhere by default; --trace exports MG_DMA_TRACE=1 and
# redirects the detached child's stderr to $TRACE_LOG so the per-frame
# stage/flush/emit lines from copro_mg_state.c are captureable.
if (( TRACE )); then
    export MG_DMA_TRACE=1
else
    unset MG_DMA_TRACE
fi

# Default mgapi rom_select to "boot" (the runtime kernel that picks up
# the cart-window staging from running demos). --smoke flips to the
# self-contained 65816 SELECT DEMO menu for legacy bring-up tests.
# Both are honored by mgapi.dll via $MGAPI_ROM_SELECT, regardless of
# whatever cfg.rom_select the bsnes mapper was compiled with.
if (( SMOKE )); then
    export MGAPI_ROM_SELECT="smoke"
else
    export MGAPI_ROM_SELECT="boot"
fi
step "MGAPI_ROM_SELECT=$MGAPI_ROM_SELECT"

# ---- 2. Pick the ROM ----------------------------------------
# Note: the actual cart-bus content is served by mgapi.dll from its
# embedded smoke_rom[] or boot_rom[] arrays (driven by MGAPI_ROM_SELECT
# above), so the file passed here is essentially a trigger for bsnes
# to invoke the mgapi cart class. snes_smoke.sfc is what bsnes
# reliably recognizes as a SNES ROM, so we keep using that.
if [[ -z "$ROM" ]]; then
    ROM="$REPO_ROOT/snes/build/snes_smoke.sfc"
fi
if [[ ! -f "$ROM" ]]; then
    if (( NO_BUILD )); then
        die "ROM not found: $ROM (and --no-build was set)"
    fi
    step "ROM missing; running snes/build.ps1 -Smoke to produce it..."
    powershell.exe -NoProfile -ExecutionPolicy Bypass \
        -File "$(cygpath -w "$REPO_ROOT/snes/build.ps1")" -Smoke
    [[ -f "$ROM" ]] || die "snes/build.ps1 ran but $ROM still missing"
fi
step "ROM: $ROM"

# ---- 3. Locate bsnes ---------------------------------------
# Probe the given dir itself AND the standard from-source layout
# (<bsnes-plus-checkout>/bsnes/out/) so pointing at a checkout root
# works -- the source build drops the .exe in bsnes/out/.
find_bsnes() {
    local dir="$1"
    [[ -z "$dir" ]] && return 1
    for sub in "" "bsnes/out"; do
        local d
        if [[ -n "$sub" ]]; then d="$dir/$sub"; else d="$dir"; fi
        for exe in bsnes.exe bsnes-plus.exe; do
            if [[ -f "$d/$exe" ]]; then
                echo "$d/$exe"
                return 0
            fi
        done
    done
    return 1
}

BSNES_EXE=""
BSNES_HOME=""

if [[ -n "$BSNES_DIR" ]]; then
    BSNES_EXE="$(find_bsnes "$BSNES_DIR")" || die "no bsnes(-plus).exe in $BSNES_DIR (also checked bsnes/out)"
elif [[ -n "${BSNES_HOME:-}" ]]; then
    BSNES_EXE="$(find_bsnes "$BSNES_HOME")" || die "BSNES_HOME=$BSNES_HOME has no bsnes(-plus).exe (also checked bsnes/out)"
else
    # <repo>/../bsnes-plus matches the "bsnes-plus cloned next to
    # microgarbage" layout -- John's setup, probably anyone else's
    # working from the same Source/ directory.
    candidates=(
        "$REPO_ROOT/bsnes-plus"
        "$(dirname "$REPO_ROOT")/bsnes-plus"
        "/c/Program Files/bsnes-plus"
        "/c/Program Files (x86)/bsnes-plus"
    )
    for d in "${candidates[@]}"; do
        if found="$(find_bsnes "$d")"; then
            BSNES_EXE="$found"; break
        fi
    done
    if [[ -z "$BSNES_EXE" ]]; then
        # Last resort: anything on PATH.
        for exe in bsnes.exe bsnes-plus.exe bsnes bsnes-plus; do
            if onpath="$(command -v "$exe" 2>/dev/null)"; then
                BSNES_EXE="$onpath"; break
            fi
        done
    fi
fi

# Working dir is where the .exe lives -- bsnes-plus from-source builds
# load cheats / save-states / config relative to the exe, which is in
# bsnes/out, NOT the checkout root.
[[ -n "$BSNES_EXE" ]] && BSNES_HOME="$(dirname "$BSNES_EXE")"

# Refresh mgapi.dll into bsnes-out if the build copy is newer. The
# bsnes-plus cart class LoadLibrary's mgapi.dll from alongside
# bsnes.exe; without this step, every rebuild needs a manual copy or
# bsnes runs against yesterday's DLL and the symptoms look bizarre
# (PuTTY silent, no shell, mgapi banner from the wrong version).
SRC_DLL="$REPO_ROOT/build/mgapi/mgapi.dll"
DST_DLL="$BSNES_HOME/mgapi.dll"
if [[ -f "$SRC_DLL" ]]; then
    if [[ ! -f "$DST_DLL" || "$SRC_DLL" -nt "$DST_DLL" ]]; then
        # cp will fail if bsnes is still running with the DLL loaded
        # (Windows holds an exclusive handle). Surface that loudly --
        # otherwise the user relaunches against yesterday's DLL and the
        # symptoms look identical to a real bug.
        if ! cp "$SRC_DLL" "$DST_DLL" 2>/dev/null; then
            die "cannot refresh $DST_DLL -- another bsnes instance still has it loaded? close it and re-run."
        fi
        step "refreshed $DST_DLL"
    else
        step "mgapi.dll already current"
    fi
fi

if [[ -z "$BSNES_EXE" ]]; then
    cat >&2 <<EOF

run-bsnes: couldn't find bsnes(-plus).exe.
  Tried --bsnes-dir, \$BSNES_HOME, common paths, and PATH.
  Pass --bsnes-dir <path> or set BSNES_HOME, e.g.:
    export BSNES_HOME=/c/bsnes-plus
EOF
    exit 1
fi
step "bsnes: $BSNES_EXE"

# ---- 4. Launch ---------------------------------------------
# Convert paths to Windows form for the .exe. cygpath does the
# right thing whether MSYS surfaces them as /c/... or C:\...
ROM_W="$(cygpath -w "$ROM")"

# Prepend Windows-form Qt-bin to PATH so a from-source bsnes-plus built
# against MSYS2's Qt5 can find Qt5Widgets.dll, libpng, etc. Without
# this the detached child process's DLL search misses the MSYS-style
# PATH entries and bsnes dies with
# "cannot open shared object file: Qt5Widgets.dll".
#
# We probe a few common locations for the actual DLL rather than
# blindly trusting /mingw64/bin -- under Git Bash /mingw64 resolves
# to the Git-shipped runtime which doesn't include Qt5, and the user
# really wants MSYS2's. First hit wins.
QT_BIN=""
for d in /mingw64/bin /c/msys64/mingw64/bin /c/msys64/clang64/bin; do
    if [[ -f "$d/Qt5Widgets.dll" ]]; then QT_BIN="$d"; break; fi
done
if [[ -n "$QT_BIN" ]]; then
    PATH="$(cygpath -w "$QT_BIN");$PATH"
    step "Qt5 bin: $QT_BIN"
fi

step "launching..."
# Resolve the trace log path BEFORE the subshell cd's into BSNES_HOME,
# so a relative --trace-log is anchored to the user's cwd, not bsnes's
# install dir.
if (( TRACE || CAPTURE )); then
    case "$TRACE_LOG" in
        /*|[A-Za-z]:[/\\]*) TRACE_LOG_ABS="$TRACE_LOG" ;;
        *)                  TRACE_LOG_ABS="$PWD/$TRACE_LOG" ;;
    esac
    if (( TRACE )); then step "MG_DMA_TRACE=1 -> stderr to $TRACE_LOG_ABS"
    else                 step "stderr -> $TRACE_LOG_ABS (no DMA trace)"; fi
fi
(
    cd "$BSNES_HOME"
    # nohup-style: detach so bash returns immediately. The user
    # presumably wants the bsnes GUI plus their bash prompt back.
    if (( TRACE || CAPTURE )); then
        "$BSNES_EXE" "$ROM_W" 2> "$TRACE_LOG_ABS" &
    else
        "$BSNES_EXE" "$ROM_W" &
    fi
    disown
)
step "done. (close the bsnes window to exit)"
