# FatFs source goes here

This file is a placeholder. To complete the FatFs setup:

## 1. Download FatFs from Elm Chan's site

Go to http://elm-chan.org/fsw/ff/ and download the current
release (R0.16, archive likely `ff16.zip` or similar).

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

## 4. Check the FFCONF_DEF version number

FatFs's `ff.c` has a compile-time check that the config file's
revision ID matches the source's. Our `ffconf.h` has the value
for R0.15p3 hard-coded (86631). If you're using R0.16 or a later
release, you need to update it.

How to find the right value:

```sh
grep "^#define FF_DEFINED" third_party/fatfs/source/ff.h
```

This will print something like `#define FF_DEFINED 80286` (the
exact number depends on the version). Take that number and put
it into `third_party/fatfs/ffconf.h`:

```sh
# in third_party/fatfs/ffconf.h, change the line:
#define FFCONF_DEF  86631
# to match whatever ff.h declared, e.g.:
#define FFCONF_DEF  80286
```

If the numbers disagree, ff.c will fail to compile with a clear
`#error Wrong configuration file (ffconf.h)`.

You may also want to diff R0.16's stock `ffconf.h` (the one
inside the FatFs zip — don't extract it into the repo) against
ours, in case any new options were added. New options usually
have safe defaults so this is informational, not required.

## 5. Re-run setup_licenses.sh

After extraction, re-run the setup script. It will detect
`source/ff.h` and write the FatFs license block to LICENSE.txt
automatically.

## 6. Delete this file (optional)

Once everything is in place, you can `git rm PLACEHOLDER.md` —
it's just here to mark the empty directory.
