# Transports

> Status: Round U in progress. U.1–U.4 done, U.5 (per-VM TUI state)
> and U.6 (multi-session orchestration) are next.

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
flag in the transport's static state. In multi-session mode (U.5/U.6)
this becomes per-session.

## Available transports

### stdio (default)

Process stdin/stdout/stderr. No `--flag` needed; just run the binary.
This is the fallback when no transport is installed — `handle_read`,
`handle_write`, etc. use their legacy paths.

Limitations: no transport-level raw mode; relies on tcgetattr/tcsetattr
inside `vm_host_stdio.c`.

### `--pipe=<name>` — Windows/Cygwin named pipe

Bidirectional `\\.\pipe\<name>` byte stream. PuTTY connects as
**Serial** (Connection type), pipe path in the Serial line field,
any speed.

- Available: Windows/Cygwin builds
- `is_terminal`: true
- `set_raw`: no-op (pipes have no line discipline)
- `flush`: no-op (kernel buffer is unbuffered at app layer)

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

Listens on the given port, accepts the first connection, that
socket is the transport. Cross-platform: BSD sockets on POSIX,
WinSock2 on native Windows.

- Available: all platforms (POSIX + native Windows via WinSock)
- `is_terminal`: true
- `set_raw`: no-op (TCP has no line discipline; the client owns
  its own terminal mode)
- `flush`: no-op
- Sets `TCP_NODELAY` for low-latency interactive use
- Sets `SO_REUSEADDR` for quick restarts

Build: native Windows requires `-lws2_32`. The Linux `build.sh`
auto-detects MSYS/MINGW environments and links it.

## Comparison

| Transport | Platforms       | Network? | TTY semantics | Use case                       |
|-----------|-----------------|----------|---------------|--------------------------------|
| stdio     | all             | no       | inherited     | default; running from a shell  |
| pipe      | Windows/Cygwin  | local    | none          | PuTTY/serial-like UX on Windows |
| pty       | POSIX           | no       | real          | screen/minicom on Linux        |
| tcp       | all             | yes      | none          | remote shells, demo of N hosts |

## Adding a new transport

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

## Multi-session (round U.5 / U.6)

Today there is **one active transport** at a time. The TUI service
has globals (canvas, pen state, input parser) that are likewise
singletons. Round U.5 makes those globals per-session (keyed by
VM id). Round U.6 lets the host accept multiple transport
instances simultaneously, spawning a fresh shell VM per connection.

The vtable doesn't change. What changes:

- `vm_host_set_transport` becomes `vm_host_set_transport_for_vm(vm_id, t)`
  with the old single-arg form preserved as backward-compat
- TUI globals (`g_canvas`, `g_pen_*`, `g_in_state`, ...) move into
  a `VmTuiSession` struct, one per VM that has called `SYS_TUI_INIT`
- The input parser's "byte arrived" path looks up which session
  the bytes came from (by which transport delivered them) and
  feeds that session's parser
- Owner-lock (`g_owner_vm` returning `-EBUSY`) is relaxed: each
  session has its own owner

User-facing wins after U.5/U.6:
- Multiple PuTTY connections to one host process, each with its
  own shell, history, and TUI surface
- Demo case: one Pi running `host --tcp=5678 --tcp=5679 --tcp=5680`
  serves three independent shell sessions
- MCU case: one STM32 with UART + USB CDC + Ethernet TCP simultaneously
  serves three concurrent users

## Round U progress

```
U.1  ✅  TUI output hook (quick fix; superseded by U.2)
U.2  ✅  Unified transport interface
U.3  ✅  POSIX pty transport
U.4  ✅  TCP socket transport
U.5  ⏳  Per-VM TUI canvas + input parser state
U.6      Multi-session orchestration
```
