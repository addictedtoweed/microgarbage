# Contributing

Notes for working on microgarbage. Short and practical.

## Warnings policy

The build treats **warnings as errors** (`-Werror`) by default. A
clean build is a hard requirement, not a nicety — a warning is a bug
waiting to happen, so the compiler enforces zero warnings rather than
relying on anyone to read the output.

When a warning appears, handle it in this order of preference:

1. **Fix the code.** Almost always the right answer. Make the warning
   genuinely not apply — correct the type, the format specifier, the
   missing initializer, the signature. The code ends up better, not
   just quieter.

   Example: the native-Windows `%zu` format warnings were fixed by
   selecting mingw's C99 stdio (`-D__USE_MINGW_ANSI_STDIO=1`), not by
   suppressing — the specifiers were already correct.

2. **Suppress at the narrowest scope — a single line — if a fix is
   genuinely impractical.** Wrap just the offending line(s):

   ```c
   #pragma GCC diagnostic push
   #pragma GCC diagnostic ignored "-Wsomething"
   /* WHY this is unavoidable here — always explain. */
   ... the one line ...
   #pragma GCC diagnostic pop
   ```

3. **Suppress per file** only if line-level won't work — e.g.
   third-party/vendor code we don't own (FatFs is the likely case).
   Prefer a targeted `-Wno-...` on just that translation unit in the
   build script over editing vendor source. Document why.

4. **Never** add a blanket project-wide `-Wno-...`. That hides real
   bugs everywhere to silence one. If you're reaching for this, stop
   and go back to option 1.

Every suppression carries a comment explaining *why* it's
unavoidable. A bare `#pragma ignored` with no reason is itself a
smell — the next reader must know it was a considered decision, not
laziness.

### Opting out of -Werror

A newer or stricter toolchain can flag something we don't see (this
genuinely happens — compilers add warnings over time). To get a
building tree on such a toolchain:

- POSIX / Cygwin (`build.sh`, via `vm_objs.sh`): `WERROR=0 ./build.sh`
- Native Windows (`build-win.sh`): `WERROR=0 ./build-win.sh`
- Native Windows (`build-win.ps1`): `.\build-win.ps1 -NoWerror`

If you hit this, please open an issue with the warning text and your
compiler version — that's a real portability signal worth fixing at
the source.

## Build layout (orientation)

- `src/vm/` — the RV32IMC VM core, scheduler, syscall handlers.
  Platform-agnostic; no OS calls.
- `src/host/` — the platform layer (`platform_posix.c`,
  `platform_win.c`, `platform_stub.c`) implementing
  `include/vm/host_platform.h`: time, sleep, the stop hook. One file
  per platform, selected by the build. Porting to a new target
  (e.g. STM32) starts from `platform_stub.c`.
- `examples/` — runnable hosts; `05_shell` is the main one.
- `third_party/fatfs/` — vendored FatFs (don't edit; wrap instead).

## Testing

Build the example ELFs first, then run the suites — several VM tests
load real guest ELFs from `examples/*/build/`, so a fresh checkout
shows spurious failures until those are built. See `README.md`.
