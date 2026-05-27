/* main.c — your microgarbage guest app. Edit me!
 *
 * This compiles to an RV32IMC ELF that runs as a guest under the
 * microgarbage VM. The SDK runtime (src/vm_runtime.c) provides _start,
 * which zeroes BSS, wires up stdio, and calls your main(). You reach
 * the host through the headers below.
 *
 * Build:   ./build.sh            (or build.ps1 on Windows PowerShell)
 *          needs only a RISC-V cross compiler.
 * Run:     copy the resulting app.elf where your host can load it
 *          (e.g. a microgarbage shell's /host dir), then `run app.elf`.
 *
 * Public domain (CC0). No warranty.
 */
#include "vm_runtime.h"   /* host hooks: SYS_*, _vm_sysN, exit, spawn, time, rand */
#include <stdio.h>        /* mini-libc: printf/puts/fopen... routed to the host  */

/* Other host hooks available (uncomment + see build.sh MODULES for tools):
 *   #include "tui.h"                       extended-char TUI + PuTTY mouse
 *   #include "audio.h"                      mixer / SFX / music / FFT meter
 *   #include "fs.h"                          files + directories (open/read/readdir/mkdir)
 *   #include "containers/ring_buffer.h"     fixed-capacity ring (caller storage)
 *   #include "math/fixed_point.h"           q15/q31 fixed-point helpers
 */

int main(void) {
    printf("hello from a microgarbage guest!\n");
    return 0;
}
