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
#    ./tools/run-bsnes.sh                       # smoke ROM, search
#    ./tools/run-bsnes.sh --bsnes-dir /c/bsnes  # explicit location
#    ./tools/run-bsnes.sh --boot                # load snes_boot.bin
#    ./tools/run-bsnes.sh --rom path/to.sfc     # arbitrary ROM
#    ./tools/run-bsnes.sh --no-build            # skip snes/build.ps1
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
BOOT=0
NO_BUILD=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --bsnes-dir) BSNES_DIR="$2"; shift 2 ;;
        --rom)       ROM="$2";       shift 2 ;;
        --boot)      BOOT=1;         shift   ;;
        --no-build)  NO_BUILD=1;     shift   ;;
        -h|--help)
            sed -n '3,18p' "$0"; exit 0 ;;
        *) die "unknown arg: $1 (use -h for usage)" ;;
    esac
done

# ---- 2. Pick the ROM ----------------------------------------
if [[ -z "$ROM" ]]; then
    if (( BOOT )); then ROM="$REPO_ROOT/snes/build/snes_boot.bin"
    else                ROM="$REPO_ROOT/snes/build/snes_smoke.sfc"
    fi
fi
if [[ ! -f "$ROM" ]]; then
    if (( NO_BUILD )); then
        die "ROM not found: $ROM (and --no-build was set)"
    fi
    step "ROM missing; running snes/build.ps1 to produce it..."
    if (( BOOT )); then
        powershell.exe -NoProfile -ExecutionPolicy Bypass \
            -File "$(cygpath -w "$REPO_ROOT/snes/build.ps1")"
    else
        powershell.exe -NoProfile -ExecutionPolicy Bypass \
            -File "$(cygpath -w "$REPO_ROOT/snes/build.ps1")" -Smoke
    fi
    [[ -f "$ROM" ]] || die "snes/build.ps1 ran but $ROM still missing"
fi
step "ROM: $ROM"

# ---- 3. Locate bsnes ---------------------------------------
find_bsnes() {
    local dir="$1"
    [[ -z "$dir" ]] && return 1
    for exe in bsnes.exe bsnes-plus.exe; do
        if [[ -f "$dir/$exe" ]]; then
            echo "$dir/$exe"
            return 0
        fi
    done
    return 1
}

BSNES_EXE=""
BSNES_HOME=""

if [[ -n "$BSNES_DIR" ]]; then
    BSNES_EXE="$(find_bsnes "$BSNES_DIR")" || die "no bsnes(-plus).exe in $BSNES_DIR"
    BSNES_HOME="$BSNES_DIR"
elif [[ -n "${BSNES_HOME:-}" ]]; then
    BSNES_EXE="$(find_bsnes "$BSNES_HOME")" || die "BSNES_HOME=$BSNES_HOME has no bsnes(-plus).exe"
else
    candidates=(
        "$REPO_ROOT/bsnes-plus"
        "$(dirname "$REPO_ROOT")/bsnes-plus"
        "/c/Program Files/bsnes-plus"
        "/c/Program Files (x86)/bsnes-plus"
    )
    for d in "${candidates[@]}"; do
        if found="$(find_bsnes "$d")"; then
            BSNES_EXE="$found"; BSNES_HOME="$d"; break
        fi
    done
    if [[ -z "$BSNES_EXE" ]]; then
        # Last resort: anything on PATH.
        for exe in bsnes.exe bsnes-plus.exe bsnes bsnes-plus; do
            if onpath="$(command -v "$exe" 2>/dev/null)"; then
                BSNES_EXE="$onpath"; BSNES_HOME="$(dirname "$onpath")"; break
            fi
        done
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
step "launching..."
(
    cd "$BSNES_HOME"
    # nohup-style: detach so bash returns immediately. The user
    # presumably wants the bsnes GUI plus their bash prompt back.
    "$BSNES_EXE" "$ROM_W" &
    disown
)
step "done. (close the bsnes window to exit)"
