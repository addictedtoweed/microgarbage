/* ============================================================
 *  shell_embedded.h — the baked-in shell.elf image.
 *
 *  shell_elf_data.c is generated at build time by tools/bin2c.c
 *  from the freshly built build/shell.elf. The array is 4-byte
 *  aligned so the host can XIP-execute the guest straight out of
 *  it (RV32 half-word fetch needs >=2-byte alignment).
 *
 *  The host loads this embedded image with VM_BACKING_XIP when no
 *  explicit ELF path is given on the command line, so a distributed
 *  host.exe runs standalone with no external shell.elf. An explicit
 *  path still loads from disk with COPY_RAM (for development).
 * ============================================================ */
#ifndef SHELL_EMBEDDED_H
#define SHELL_EMBEDDED_H

#include <stddef.h>

extern const unsigned char shell_elf[];
extern const size_t        shell_elf_len;

#endif /* SHELL_EMBEDDED_H */
