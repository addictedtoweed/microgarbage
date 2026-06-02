#!/usr/bin/env bash
# ============================================================
#  build.sh — rebuild kernel.s + mgapi.dll from MSYS in one shot
#
#  When you edit snes/kernel.s or anything under src/mgapi/, you need
#  TWO rebuilds in this exact order:
#
#    1. snes/build.ps1  -> snes/build/snes_boot.bin   (assembles kernel)
#    2. build-mgapi.ps1 -> build/mgapi/mgapi.dll      (embeds the .bin)
#
#  Then tools/run-bsnes.sh auto-copies the fresh DLL into bsnes/out/
#  and launches bsnes. This script chains those steps so a single
#  invocation gets you back to a running bsnes against your edits.
#
#  Usage:
#    ./tools/build.sh                       # rebuild SNES + mgapi
#    ./tools/build.sh --launch              # also launch bsnes after
#    ./tools/build.sh --smoke               # build the 65816 SELECT
#                                           #   DEMO ROM instead of the
#                                           #   kernel (legacy menu)
#    ./tools/build.sh --no-snes             # skip the SNES build
#    ./tools/build.sh --no-mgapi            # skip the mgapi.dll build
#
#  Public domain (CC0). No warranty.
# ============================================================
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

step() { printf 'build: %s\n' "$*"; }
die()  { printf 'build: ERROR -- %s\n' "$*" >&2; exit 1; }

# ---- 1. Args ------------------------------------------------
SMOKE=0
LAUNCH=0
NO_SNES=0
NO_MGAPI=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --smoke)    SMOKE=1;    shift ;;
        --launch)   LAUNCH=1;   shift ;;
        --no-snes)  NO_SNES=1;  shift ;;
        --no-mgapi) NO_MGAPI=1; shift ;;
        -h|--help)  sed -n '3,28p' "$0"; exit 0 ;;
        *) die "unknown arg: $1 (use -h for usage)" ;;
    esac
done

# ---- 2. PATH for ca65 / ld65 -------------------------------
# snes/build.ps1 expects the cc65 suite on PATH. MSYS PATH usually
# doesn't include it (cc65 ships as a Windows installer drop). Probe
# common locations and prepend whichever is found, in Windows form
# so powershell.exe sees it correctly.
CC65_BIN=""
for d in /c/cc65/bin "/c/Program Files/cc65/bin" "/c/Program Files (x86)/cc65/bin"; do
    if [[ -f "$d/ca65.exe" ]]; then CC65_BIN="$d"; break; fi
done
if [[ -n "$CC65_BIN" ]]; then
    PATH="$(cygpath -w "$CC65_BIN");$PATH"
    step "cc65 bin: $CC65_BIN"
else
    step "warning: cc65 not found in common paths -- snes/build.ps1 will fail if it's not on PATH already"
fi

# ---- 3. TMP fix --------------------------------------------
# Set TMP / TEMP to the 8.3 short form of the user's Windows temp
# directory so the build chain doesn't trip over spaces in the path.
# Some of the windows tools spawned downstream choke on
# "C:\Users\IP Freely\AppData\Local\Temp" but are happy with
# "C:\Users\IPFREE~1\AppData\Local\Temp".
#
# Earlier versions of this script shelled out to `cmd.exe /c "echo
# %TEMP%"` to discover the long path, but that hangs in some MSYS
# configurations (the pipe back to bash doesn't close). $USERPROFILE
# is set directly by MSYS and round-trips through cygpath cleanly.
if [[ -n "${USERPROFILE:-}" ]]; then
    TMP_LONG="$USERPROFILE\\AppData\\Local\\Temp"
    TMP_SHORT="$(cygpath -s -w "$(cygpath -u "$TMP_LONG")" 2>/dev/null || echo "$TMP_LONG")"
    export TMP="$TMP_SHORT"
    export TEMP="$TMP_SHORT"
fi

# ---- 4. Build SNES side ------------------------------------
SBOOT="$REPO_ROOT/snes/build/snes_boot.bin"
SSFC="$REPO_ROOT/snes/build/snes_smoke.sfc"
if (( ! NO_SNES )); then
    SNES_PS="$(cygpath -w "$REPO_ROOT/snes/build.ps1")"
    if (( SMOKE )); then
        step "snes/build.ps1 -Smoke  (legacy 65816 SELECT DEMO ROM)"
        powershell.exe -NoProfile -ExecutionPolicy Bypass \
            -File "$SNES_PS" -Smoke \
            || die "snes/build.ps1 -Smoke failed"
        [[ -f "$SSFC" ]] || die "snes/build.ps1 produced no snes_smoke.sfc"
    else
        step "snes/build.ps1  (runtime kernel: boot.s + kernel.s)"
        powershell.exe -NoProfile -ExecutionPolicy Bypass \
            -File "$SNES_PS" \
            || die "snes/build.ps1 failed"
        [[ -f "$SBOOT" ]] || die "snes/build.ps1 produced no snes_boot.bin"
    fi
fi

# ---- 5. Build mgapi.dll (re-embeds the just-built SNES ROM)
DLL="$REPO_ROOT/build/mgapi/mgapi.dll"
if (( ! NO_MGAPI )); then
    MG_PS="$(cygpath -w "$REPO_ROOT/build-mgapi.ps1")"
    step "build-mgapi.ps1"
    powershell.exe -NoProfile -ExecutionPolicy Bypass \
        -File "$MG_PS" \
        || die "build-mgapi.ps1 failed"
    [[ -f "$DLL" ]] || die "build-mgapi.ps1 produced no mgapi.dll"
fi

# ---- 6. Summary --------------------------------------------
step "artifacts:"
[[ -f "$SBOOT" ]] && printf '  %s\n' "$(ls -la "$SBOOT" | awk '{print $6,$7,$8,"  ",$NF}')"
[[ -f "$SSFC"  ]] && printf '  %s\n' "$(ls -la "$SSFC"  | awk '{print $6,$7,$8,"  ",$NF}')"
[[ -f "$DLL"   ]] && printf '  %s\n' "$(ls -la "$DLL"   | awk '{print $6,$7,$8,"  ",$NF}')"

# ---- 7. Optional launch -----------------------------------
if (( LAUNCH )); then
    step "launching bsnes..."
    if (( SMOKE )); then
        exec "$REPO_ROOT/tools/run-bsnes.sh" --smoke
    else
        exec "$REPO_ROOT/tools/run-bsnes.sh"
    fi
fi
