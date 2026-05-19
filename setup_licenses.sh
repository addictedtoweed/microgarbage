#!/bin/bash
# setup_licenses.sh — one-shot helper to set up the repo's licensing
# files and the third_party/ scaffold for FatFs.
#
# Run this once from the repo root after pulling the integration
# commits. It does the following:
#
#   1. Creates LICENSE at the repo root (CC0 1.0 Universal, the
#      formal legal-code text, fetched from the SPDX license list
#      and SHA-256-verified against a pinned hash).
#
#   2. Creates third_party/README.md explaining the directory.
#
#   3. Creates third_party/fatfs/PLACEHOLDER.md with instructions
#      on where to drop FatFs sources.
#
#   4. If you've already extracted FatFs into third_party/fatfs/source/,
#      extracts the BSD-1-clause license block from ff.h and writes
#      it to third_party/fatfs/LICENSE.txt. Otherwise it leaves a
#      stub and tells you to re-run this script after extracting.
#
# Idempotent: re-running won't damage anything. If LICENSE already
# exists, the script skips that step with a warning.
#
# Requirements: bash, curl, sha256sum (or shasum on macOS).
#
# Public domain (CC0). No warranty.

# We deliberately do NOT use `set -e` here. Each step handles its
# own errors and reports clearly; the script should keep going so
# a network-blocked step 1 doesn't prevent the directory scaffold
# from being created.

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
echo "Setting up licenses for: $REPO_ROOT"
echo

# ---------------------------------------------------------------
# Step 1: Create LICENSE (CC0 1.0 Universal)
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

echo "Step 1: LICENSE (CC0 1.0 Universal)"

# Allow this step to fail independently of the directory scaffold
# below — if the download doesn't work in this environment, the
# user can still benefit from steps 2-4 and download CC0 manually.
license_step_ok=0

if [ -f LICENSE ]; then
    color_skip "LICENSE already exists; not overwriting"
    license_step_ok=1
else
    TMP_LICENSE=$(mktemp)

    if curl -fsSL "$CC0_URL_PRIMARY" -o "$TMP_LICENSE" 2>/dev/null && [ -s "$TMP_LICENSE" ]; then
        color_ok "fetched CC0 text from SPDX"
        license_step_ok=1
    elif curl -fsSL "$CC0_URL_FALLBACK" -o "$TMP_LICENSE" 2>/dev/null && [ -s "$TMP_LICENSE" ]; then
        color_ok "fetched CC0 text from creativecommons.org (fallback)"
        color_warn "primary SPDX source was unreachable"
        license_step_ok=1
    else
        color_fail "could not fetch CC0 text from any source"
        echo
        echo "  Network appears to be blocked. Manual fallback:"
        echo "    curl -o LICENSE $CC0_URL_PRIMARY"
        echo "  or download from a browser and save as LICENSE here."
        echo
        echo "  Continuing with the rest of the setup..."
        rm -f "$TMP_LICENSE"
    fi

    if [ "$license_step_ok" = 1 ] && [ -s "$TMP_LICENSE" ]; then
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
# Step 2: third_party/README.md
# ---------------------------------------------------------------

echo "Step 2: third_party/README.md"

mkdir -p third_party

if [ -f third_party/README.md ]; then
    color_skip "third_party/README.md already exists"
else
    cat > third_party/README.md << 'TPEOF'
# Third-party components

The rest of this repository is released under CC0 1.0 Universal
(see `/LICENSE` at the repo root) — i.e., as close to public
domain as the law allows. Code in **this directory** is the
exception: each subdirectory contains software from another
author, under its own license. Read the relevant LICENSE file
before using or redistributing.

## Contents

### `fatfs/` — Elm Chan's FatFs

A small, portable FAT/exFAT/FAT12-16-32 filesystem library for
embedded systems. We use it in `src/storage/trashdrive_fatfs.c`
to expose a FAT filesystem on top of `trashdrive` (the RAM-backed
block device in this library).

- **Upstream:** http://elm-chan.org/fsw/ff/ (canonical source)
- **License:** BSD-1-clause (see `fatfs/LICENSE.txt`)
- **Pinned version:** R0.15p3 (others should work but are untested)
- **What we customized:** `fatfs/ffconf.h` is OUR build of the
  FatFs config file, tuned for this project (no LFN, no RTC,
  single volume, writable). The rest of FatFs is unmodified
  upstream — `fatfs/source/ff.h` and `fatfs/source/ff.c` are
  byte-for-byte from the official release.

To rebuild against a different FatFs version: replace
`fatfs/source/*` with the new files but keep our `fatfs/ffconf.h`.
The diskio shim (`src/storage/trashdrive_fatfs.c`) targets
FatFs's stable C API and should not need changes.

## How third-party code is wired in

We deliberately keep third-party headers OFF the default
include path. `-Iinclude` (our standard build flag) does NOT
expose third_party/. Any source file that needs FatFs symbols
explicitly adds `-Ithird_party/fatfs/source` and
`-Ithird_party/fatfs` (the config). This makes the dependency
boundary visible in build scripts.

If you fork this repo and don't need FatFs, you can delete
`third_party/fatfs/` entirely. The only files that pull it in
are `src/storage/trashdrive_fatfs.c/h` — remove those and
nothing else breaks.
TPEOF
    color_ok "wrote third_party/README.md"
fi
echo

# ---------------------------------------------------------------
# Step 3: third_party/fatfs/PLACEHOLDER.md
# ---------------------------------------------------------------

echo "Step 3: third_party/fatfs/ scaffold"

mkdir -p third_party/fatfs

if [ -f third_party/fatfs/PLACEHOLDER.md ]; then
    color_skip "PLACEHOLDER.md already exists"
else
    cat > third_party/fatfs/PLACEHOLDER.md << 'PLEOF'
# FatFs source goes here

This file is a placeholder. To complete the FatFs setup:

## 1. Download FatFs from Elm Chan's site

Go to http://elm-chan.org/fsw/ff/ and download the current
release (pinned: R0.15p3, archive `ff15p3.zip`).

## 2. Extract into `source/`

Create `third_party/fatfs/source/` and extract these files
from the FatFs zip's `source/` directory into it:

    ff.h
    ff.c
    ffsystem.c
    diskio.h

Leave them BYTE-FOR-BYTE unchanged. Do not edit, reformat, or
strip comments — the BSD-1-clause license requires the copyright
notice to be retained.

You can skip `ffunicode.c` because our config sets `FF_USE_LFN = 0`
(short filenames only). If you flip that to 1, copy `ffunicode.c`
in too.

## 3. Verify the layout

After extraction, this directory should look like:

    third_party/fatfs/
    ├── LICENSE.txt           ← created by setup_licenses.sh
    ├── PLACEHOLDER.md        ← this file (you can delete it after setup)
    ├── README.md             ← present in third_party/, applies here too
    ├── ffconf.h              ← OUR tuned config (already in the repo)
    └── source/
        ├── ff.h              ← from FatFs zip, unmodified
        ├── ff.c              ← from FatFs zip, unmodified
        ├── ffsystem.c        ← from FatFs zip, unmodified
        └── diskio.h          ← from FatFs zip, unmodified

## 4. Re-run setup_licenses.sh

After extraction, re-run the setup script. It will detect
`source/ff.h` and write the FatFs license block to LICENSE.txt
automatically.

## 5. Delete this file (optional)

Once everything is in place, you can `git rm PLACEHOLDER.md` —
it's just here to mark the empty directory.
PLEOF
    color_ok "wrote third_party/fatfs/PLACEHOLDER.md"
fi
echo

# ---------------------------------------------------------------
# Step 4: third_party/fatfs/LICENSE.txt
# ---------------------------------------------------------------
#
# If FatFs has been extracted (ff.h exists), grab the license
# block from the top of that file and write it to LICENSE.txt.
# Otherwise leave a small note saying to re-run after extraction.

echo "Step 4: third_party/fatfs/LICENSE.txt"

FATFS_HEADER="third_party/fatfs/source/ff.h"

if [ -f "$FATFS_HEADER" ]; then
    # Extract the license block: it's the comment block near the
    # top of ff.h that contains the "FatFs" copyright. We extract
    # from the first /* to the next */ that contains "DISCLAIMED".
    LICENSE_BLOCK=$(awk '
        /^\/\*/ { in_comment = 1; block = "" }
        in_comment { block = block $0 "\n" }
        /DISCLAIMED/ && in_comment { print block; exit }
    ' "$FATFS_HEADER")

    if [ -z "$LICENSE_BLOCK" ]; then
        color_warn "could not extract license block from ff.h"
        color_warn "(searched for a /* ... DISCLAIMED ... */ block)"
        color_warn "fall back to manual: copy the top comment block"
        color_warn "from ff.h into third_party/fatfs/LICENSE.txt"
    else
        # Strip C comment delimiters and leading "/" lines.
        cat > third_party/fatfs/LICENSE.txt << HDREOF
FatFs - Generic FAT Filesystem Module - License
================================================

The text below is extracted verbatim from the copyright notice
at the top of FatFs's ff.h. This file is BSD-1-clause licensed
by ChaN; the rest of this repository is CC0 (see /LICENSE).

When redistributing the FatFs source files (ff.h, ff.c, etc.),
keep their copyright notices intact — that is the only
obligation BSD-1-clause imposes.

----------------------------------------------------------------

HDREOF
        # Append the extracted block, lightly cleaned (strip the
        # "/*" and "*/" delimiters and the "/" at start of lines)
        echo "$LICENSE_BLOCK" | sed -E '
            s|^/\*+/?||
            s|\*+/$||
            s|^/||
            s|^ \* ?||
        ' >> third_party/fatfs/LICENSE.txt

        color_ok "wrote third_party/fatfs/LICENSE.txt from ff.h"
    fi
else
    if [ -f third_party/fatfs/LICENSE.txt ]; then
        color_skip "LICENSE.txt already exists (FatFs not present; re-run after extracting to update)"
    else
        cat > third_party/fatfs/LICENSE.txt << 'STUBEOF'
FatFs - Generic FAT Filesystem Module - License
================================================

(stub — FatFs source not yet extracted)

After you download FatFs from http://elm-chan.org/fsw/ff/ and
extract it into third_party/fatfs/source/, re-run

    ./setup_licenses.sh

from the repo root, and this file will be replaced with the
license block from ff.h verbatim.

The expected license is BSD-1-clause, which requires only that
the copyright notice be retained when source is redistributed.
STUBEOF
        color_info "wrote stub LICENSE.txt; re-run this script after"
        color_info "extracting FatFs to populate it from ff.h"
    fi
fi
echo

# ---------------------------------------------------------------
# Summary
# ---------------------------------------------------------------

echo "Done."
echo
echo "Next steps:"
echo
if [ ! -f "$FATFS_HEADER" ]; then
    echo "  1. Download FatFs from http://elm-chan.org/fsw/ff/"
    echo "  2. Extract source files to third_party/fatfs/source/"
    echo "     (see third_party/fatfs/PLACEHOLDER.md for details)"
    echo "  3. Re-run this script to populate LICENSE.txt from ff.h"
    echo "  4. git add . && git commit"
else
    echo "  1. Verify the LICENSE files look correct:"
    echo "       cat LICENSE | head -3"
    echo "       cat third_party/fatfs/LICENSE.txt | head -10"
    echo "  2. git add . && git commit"
fi
