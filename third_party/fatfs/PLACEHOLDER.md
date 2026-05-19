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
