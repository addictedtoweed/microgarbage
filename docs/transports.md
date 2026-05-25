# Transports

> Status: Round U complete. Pluggable transports (stdio, pty, TCP)
> with per-VM routing and multi-session orchestration. One host
> process can run N independent shell sessions, each on its own
> transport with its own canvas and input.

A **transport** is the host-side mechanism by which a guest's
console-style I/O reaches the user. The base abstraction is a
byte-stream: read N bytes non-blocking, write N bytes, flush,
optionally toggle raw mode.

Today every console-style I/O path funnels through one indirection:

```
SYS_READ(fd=0)     ─┐
SYS_WRITE(fd=1,2)  ─┤
SYS_FFLUSH         ─┤
TUI canvas output  ─┼──→  VmHostTransport vtable  ──→  bytes
TUI input parser   ─┤
set raw mode       ─┘
```

When no transport is installed, the host falls back to the legacy
stdio paths (process stdin/stdout/stderr). Existing single-process
demos see no difference.

## The vtable

```c
typedef struct VmHostTransport {
    /* Read up to `cap` bytes non-blocking.
     *   > 0  bytes read
     *   = 0  no data right now
     *   < 0  -errno on error
     * MUST NOT block. */
    int  (*read_nonblock)(VmHostTransport *t, void *buf, unsigned cap);

    /* Write `n` bytes best-effort.
     *   >= 0  bytes written
     *   < 0   -errno on error
     * MAY block briefly for flow control; SHOULD NOT block long. */
    int  (*write)(VmHostTransport *t, const void *buf, unsigned n);

    /* Flush any buffered output. 0 on success, -errno on error.
     * No-op for transports without app-level buffering. */
    int  (*flush)(VmHostTransport *t);

    /* Toggle raw mode on the underlying surface. May be a no-op
     * for transports without termios-style line discipline. */
    int  (*set_raw)(VmHostTransport *t, bool enable);

    /* Optional teardown. May be NULL. */
    void (*close)(VmHostTransport *t);

    /* True if the far end is a real terminal that interprets
     * ANSI escapes + cursor positioning. False for file/null sinks. */
    bool is_terminal;

    /* Transport-private state. */
    void *ctx;
} VmHostTransport;
```

The host activates a transport via `vm_host_set_transport(t)`. Passing
`NULL` restores the legacy fallback.

## Universal write policy: LF→CRLF translation

Every transport's `write` method does the same thing on `'\n'`:

- A `'\n'` byte **not** preceded by `'\r'` is rewritten as `'\r\n'`.
- A `'\n'` byte **already** preceded by `'\r'` passes through unchanged.

This single rule handles both shell cooked output (`puts("hello\n")`)
and TUI canvas escapes (which emit explicit `'\r\n'` already) without
needing the caller to know which kind of byte it's writing.

The stateful tracking is per-transport-instance via a `prev_was_cr`
flag. For multi-instance TCP it lives in the per-connection `ctx`
(so two sessions don't share LF→CRLF state); for the single-instance
pty transport it's a static.

## Available transports

### stdio (default)

Process stdin/stdout/stderr. No `--flag` needed; just run the binary.
This is the fallback when no transport is installed — `handle_read`,
`handle_write`, etc. use their legacy paths.

Limitations: no transport-level raw mode; relies on tcgetattr/tcsetattr
inside `vm_host_stdio.c`.

### `--pty` — POSIX pseudoterminal

Allocates a master/slave pair via `posix_openpt`. The slave path
(typically `/dev/pts/N`) is printed to stderr. User attaches a
terminal emulator: `screen /dev/pts/N`, `minicom -D /dev/pts/N`, etc.

- Available: Linux, BSD, macOS, Cygwin
- `is_terminal`: true
- `set_raw`: real — `cfmakeraw` + `tcsetattr` on the master fd
  affects the slave's line discipline (POSIX-defined behavior)
- `flush`: no-op

### `--tcp=<port>` — TCP socket

Listens on the given port; each accepted connection becomes a
session. Cross-platform: BSD sockets on POSIX, WinSock2 on native
Windows. Repeatable: pass `--tcp=` multiple times for multiple
simultaneous sessions (see Multi-session below).

- Available: all platforms (POSIX + native Windows via WinSock)
- `is_terminal`: true
- `set_raw`: no-op (TCP has no line discipline; the client owns
  its own terminal mode)
- `flush`: no-op
- Multi-instance: per-connection state (sockets, LF→CRLF) lives
  behind the vtable `ctx`, so N transports coexist
- Sets `TCP_NODELAY` for low-latency interactive use
- Sets `SO_REUSEADDR` for quick restarts

Build: native Windows requires `-lws2_32`. The Linux `build.sh`
auto-detects MSYS/MINGW environments and links it.

#### Stopping the host (Ctrl-C) — native build under mintty

If you build the **native** host with `build-win.sh`/`build-win.ps1`
(a real Windows `.exe`, no `cygwin1.dll`) and then run it inside
**mintty** (the default Cygwin terminal), Ctrl-C may not stop the
host once a TCP session is connected. This is not a host bug — it's
the native-binary-under-pty mismatch:

- The native `.exe` doesn't link `cygwin1.dll`, so it never sees
  Cygwin's POSIX `SIGINT`.
- mintty is a pty, not a Win32 console, so it never generates the
  console `CTRL_C_EVENT` that the host's `SetConsoleCtrlHandler`
  would catch.

With no session connected the host is idle enough that Ctrl-C still
lands; once it's busy in Winsock calls for a live session, the
interrupt is swallowed.

Fixes, in order of preference:

- **Run the native host from `cmd.exe` or PowerShell** (a real
  Windows console). Ctrl-C works in all cases there — and this is
  how it'll run on the eventual native-Windows target anyway.
- Under mintty, launch via `winpty`: `winpty ./host.exe --tcp=5000`.
  `winpty` bridges the pty to a real console for native programs.
- Or stop it with `kill -INT <pid>` from another Cygwin terminal.

The native host prints a one-line NOTE at startup when it detects it
has no real console, as a reminder.

(The Cygwin `build.sh` build — linked against `cygwin1.dll` — does
participate in Cygwin signals, so Ctrl-C under mintty behaves
normally there. The tradeoff is a Cygwin-DLL dependency rather than
a standalone `.exe`.)

#### Connecting with PuTTY

Connection type **Raw** or **Telnet** both work. The host speaks a
raw byte stream, but it absorbs Telnet IAC negotiation, so a Telnet
client (PuTTY's default type) no longer dumps "^C" and stray bytes
at the prompt on connect.

For the cleanest interactive editing, under **Terminal** set:

- **Local echo: Force off**
- **Local line editing: Force off**

If you leave them on (a common Raw/Telnet default), PuTTY echoes
everything *it* sends back to its own screen and buffers a whole
line before sending. Symptoms: Tab shows as `^I`, the games' mouse
reports flood the screen as `^[[<..M` text, and character-at-a-time
line editing / tab completion don't engage. With both forced off,
PuTTY sends each keystroke raw and the shell behaves correctly.

`nc` avoids both issues (no echo, no line buffering, no Telnet) but
renders mouse/color poorly; PuTTY configured as above is the better
interactive client.

## Comparison

| Transport | Platforms       | Network? | TTY semantics | Multi? | Use case                        |
|-----------|-----------------|----------|---------------|--------|---------------------------------|
| stdio     | all             | no       | inherited     | no     | default; running from a shell   |
| pty       | POSIX           | no       | real          | no     | screen/minicom on Linux         |
| tcp       | all             | yes      | none          | yes    | PuTTY/nc; remote + N-session    |

## Adding a new transport

**Fastest path:** copy `examples/05_shell/transport_template.c` to
`transport_<yours>.c`, fill in the five functions (it's a working
loopback you replace piece by piece), call your `setup` from `main()`,
and add the file to the build. The template is ~50 lines of code with
a per-function "change this" comment; the rest of this section is the
same steps in prose.

Five things to implement:

1. **A `setup_*` function** that creates whatever underlying resource
   the transport needs (open the serial port, accept the socket,
   spawn the pty), then calls:

   ```c
   vm_host_install_stdio_ex(sys, &sio);   // installs SYS_READ/WRITE handlers
   vm_host_set_transport(&my_transport);  // points them at our vtable
   ```

2. **Four vtable methods**:
   - `_read(VmHostTransport *t, void *buf, unsigned cap)`
   - `_write(VmHostTransport *t, const void *buf, unsigned n)`
   - `_flush(VmHostTransport *t)` — usually returns 0 immediately
   - `_set_raw(VmHostTransport *t, bool enable)` — usually returns 0

   Plus an optional `_close` if the resource needs explicit cleanup.

3. **A static `VmHostTransport` instance** wiring them up:

   ```c
   static VmHostTransport g_my_transport = {
       .read_nonblock = my_t_read,
       .write         = my_t_write,
       .flush         = my_t_flush,
       .set_raw       = my_t_set_raw,
       .close         = my_t_close,
       .is_terminal   = true,
       .ctx           = NULL,
   };
   ```

4. **CLI parsing** in `examples/05_shell/host.c`:

   ```c
   } else if (strncmp(argv[i], "--myname=", 9) == 0) {
       my_arg = argv[i] + 9;
   ```

   ...plus the help line and the conflict check that ensures only
   one transport is active.

5. **The install branch** in `main()`:

   ```c
   } else if (my_arg) {
       if (!setup_my_transport(&sys, my_arg)) return 1;
   ```

That's it. Existing demos pick up the new transport for free because
they all go through the same vtable.

## Future transports (roadmap)

- **Serial** — `--serial=PATH[,BAUD]` host-side serial port.
  Different driver paths on POSIX (`open` + `tcsetattr`) vs Windows
  (`CreateFile` + `SetCommState`), same vtable. Useful for testing
  the platform talking to a real MCU over USB-to-serial.

- **STM32 UART** — on the eventual MCU build. Same vtable; methods
  call into the HAL UART driver. Looks identical to dev-host serial
  to user.

- **USB CDC ACM** — MCU exposing serial-over-USB. From the user's
  PC it appears as `/dev/ttyACM0` or COM4; from the MCU it's a USB
  CDC class endpoint. Same vtable; methods call into the USB stack's
  bulk endpoints.

- **lwIP TCP** — when the MCU gains 10M Ethernet. Same TCP transport
  source code, just compiled against lwIP's sockets API instead of
  the host's. The shim layer at the top of host.c needs one more
  `#ifdef` branch for lwIP.

## Multi-session

One host process can run **N independent shell sessions**, each on
its own transport, each with its own canvas, pen state, and input
parser. This works today.

### Usage

```
./host --tcp=5678 --tcp=5679 --tcp=5680
```

Three TCP listeners come up. As each client connects (`nc localhost
5678`, etc.), a shell VM is spawned and bound to that connection.
Each session is fully independent — separate cwd, history, and TUI
surface. A TUI program launched in one session (snake, car) renders
only to that session's client.

Up to `MAX_TCP_PORTS` (16) `--tcp=` ports are accepted. `--pty`
remains single-instance and can't be combined with `--tcp`
(multi-instancing it is a future refinement; TCP is the
demo-relevant multi case).

### Lifecycle

The host runs until every port has had a client **and** all spawned
shells have exited. Ports that never get a client keep their
listeners open — Ctrl-C to abort. This matches the demo flow: launch
the host, connect your clients, they each `exit`, the host exits with
the last one. Reconnect-after-exit is a future refinement.

### How it works

- **Per-VM transport routing** (U.6): `vm_host_set_transport_for_vm(vm_id, t)`
  binds a transport to a specific VM. Every I/O site resolves the
  transport via the calling VM. The old single-arg
  `vm_host_set_transport(t)` still works and sets a process default;
  lookup precedence is per-VM → default → legacy stdio.

- **Caller-provided session pool** (U.7a): the host supplies backing
  storage via `vm_host_tui_set_pool(pool, count)`. Each VM that calls
  `SYS_TUI_INIT` is allocated a slot, freed on shutdown/exit. The
  build sizes the pool to its RAM budget — 1-2 sessions in internal
  SRAM on a bare MCU, 16 in external SDRAM or freely on a PC. Per
  session is ~33 KB at the default 30×80 canvas.

- **Multi-instance TCP** (U.7b): each TCP transport's per-connection
  state (sockets, LF→CRLF tracking) lives behind the vtable's `ctx`
  pointer, so N transports coexist. pty stays single-instance.

- **Spawn inheritance** (U.7b): when a shell spawns a child VM (e.g.
  running `snake.elf`), the child inherits the parent's transport
  binding — otherwise the spawned program's canvas output would
  resolve to no transport and vanish.

### MCU mapping

On the eventual STM32 target, the same machinery maps a UART, a USB
CDC ACM endpoint, and an Ethernet TCP socket each to a session — so
one board can serve a local serial console and a remote network shell
at once, sized by a pool that fits the chip's RAM.

## Round U progress

```
U.1   ✅  TUI output hook (quick fix; superseded by U.2)
U.2   ✅  Unified transport interface
U.3   ✅  POSIX pty transport
U.4   ✅  TCP socket transport
U.5a  ✅  Transports design doc
U.5   ✅  Per-VM TUI session struct
U.6   ✅  Per-VM transport routing
U.7a  ✅  Caller-provided session pool + per-VM resolution
U.7b  ✅  Multi-session orchestration
```

Round U is complete.
