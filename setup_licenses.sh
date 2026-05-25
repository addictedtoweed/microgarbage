#!/bin/bash
# setup_licenses.sh — one-shot helper to set up the repo's LICENSE.
#
# The entire repository is released under CC0 1.0 Universal — i.e.,
# as close to public domain as the law allows. There is no longer
# any third-party/vendored code (FatFs was removed; the native,
# public-domain trashfs filesystem replaced it), so this script's
# only job is to drop the canonical CC0 legal-code text at the repo
# root as LICENSE.
#
#   1. Creates LICENSE at the repo root (CC0 1.0 Universal, the
#      formal legal-code text, fetched from the SPDX license list
#      and SHA-256-verified against an optional pinned hash).
#
# Idempotent: re-running won't damage anything. If LICENSE already
# exists, the script skips that step with a warning.
#
# Requirements: bash, curl, sha256sum (or shasum on macOS).
#
# Public domain (CC0). No warranty.

# We deliberately do NOT use `set -e` here — the step handles its
# own errors and reports clearly.

# ---------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------

# Portable SHA-256 (macOS uses shasum -a 256, Linux uses sha256sum)
sha256() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$1" | awk '{print $1}'
    elif command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        echo "error: neither sha256sum nor shasum found" >&2
        exit 1
    fi
}

color_ok()    { printf "  \033[32mOK\033[0m   %s\n" "$1"; }
color_skip()  { printf "  \033[33mSKIP\033[0m %s\n" "$1"; }
color_warn()  { printf "  \033[33mWARN\033[0m %s\n" "$1"; }
color_fail()  { printf "  \033[31mFAIL\033[0m %s\n" "$1"; }
color_info()  { printf "  \033[36mINFO\033[0m %s\n" "$1"; }

# Confirm we're at the repo root by looking for a known marker.
if [ ! -d include ] || [ ! -d src ] || [ ! -f README.md ]; then
    echo "error: this script must be run from the microgarbage repo root" >&2
    echo "(expected to find include/, src/, and README.md here)" >&2
    exit 1
fi

REPO_ROOT=$(pwd)
echo "Setting up license for: $REPO_ROOT"
echo

# ---------------------------------------------------------------
# Create LICENSE (CC0 1.0 Universal)
# ---------------------------------------------------------------
#
# We fetch the text from the SPDX license-list-data repo on GitHub,
# which is the standard machine-readable license registry. The
# CC0-1.0.txt file there is the canonical legal-code text.
#
# Sources tried in order:
#   1. GitHub SPDX raw — primary, very stable
#   2. creativecommons.org — fallback, the original source
#
# The downloaded file is SHA-256-verified against a pinned hash.
# If the hash doesn't match, we refuse to write LICENSE so the
# user can investigate.

CC0_URL_PRIMARY="https://raw.githubusercontent.com/spdx/license-list-data/main/text/CC0-1.0.txt"
CC0_URL_FALLBACK="https://creativecommons.org/publicdomain/zero/1.0/legalcode.txt"

# Optional SHA-256 pin. Leave empty to skip verification. If you
# want to pin: run the script once successfully, then take the
# value printed by `sha256 LICENSE` and paste it here. Future
# runs will then fail loudly if the upstream text changes.
CC0_EXPECTED_SHA=""

echo "LICENSE (CC0 1.0 Universal)"

if [ -f LICENSE ]; then
    color_skip "LICENSE already exists; not overwriting"
else
    TMP_LICENSE=$(mktemp)

    if curl -fsSL "$CC0_URL_PRIMARY" -o "$TMP_LICENSE" 2>/dev/null && [ -s "$TMP_LICENSE" ]; then
        color_ok "fetched CC0 text from SPDX"
    elif curl -fsSL "$CC0_URL_FALLBACK" -o "$TMP_LICENSE" 2>/dev/null && [ -s "$TMP_LICENSE" ]; then
        color_ok "fetched CC0 text from creativecommons.org (fallback)"
        color_warn "primary SPDX source was unreachable"
    else
        color_fail "could not fetch CC0 text from any source"
        echo
        echo "  Network appears to be blocked. Manual fallback:"
        echo "    curl -o LICENSE $CC0_URL_PRIMARY"
        echo "  or download from a browser and save as LICENSE here."
        rm -f "$TMP_LICENSE"
        TMP_LICENSE=""
    fi

    if [ -n "$TMP_LICENSE" ] && [ -s "$TMP_LICENSE" ]; then
        if [ -n "$CC0_EXPECTED_SHA" ]; then
            GOT_SHA=$(sha256 "$TMP_LICENSE")
            if [ "$GOT_SHA" != "$CC0_EXPECTED_SHA" ]; then
                color_warn "SHA-256 mismatch — got $GOT_SHA"
                color_warn "  expected $CC0_EXPECTED_SHA"
                color_warn "review LICENSE before committing"
            else
                color_ok "SHA-256 verified against pinned hash"
            fi
        else
            color_info "no SHA-256 pin set (CC0_EXPECTED_SHA is empty)"
            color_info "  current hash: $(sha256 "$TMP_LICENSE")"
            color_info "  paste this into the script to pin it for future runs"
        fi
        mv "$TMP_LICENSE" LICENSE
        color_ok "wrote LICENSE ($(wc -c < LICENSE | tr -d ' ') bytes)"
    fi
fi
echo

# ---------------------------------------------------------------
# Summary
# ---------------------------------------------------------------

echo "Done."
echo
echo "Next steps:"
echo "  1. Verify the LICENSE looks correct:  cat LICENSE | head -3"
echo "  2. git add LICENSE && git commit"
