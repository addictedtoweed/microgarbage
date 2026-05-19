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
- **Pinned version:** R0.16 (R0.15p3 was the previous; others should work but only these two have been verified)
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
