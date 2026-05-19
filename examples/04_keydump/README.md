# Example 04: Keydump

A guest that prints each byte it reads from host stdin as a
bracketed hex value. Lets you see exactly what your terminal
sends for arrow keys, function keys, mouse clicks, paste
boundaries, etc.

## What this demonstrates

- **`SYS_READ`**: non-blocking host stdin input. Returns 0 if no
  bytes are ready, distinguishing "wait" from "EOF" (which
  returns `-EIO`).
- **Raw terminal mode** via `vm_host_install_stdio_ex` with
  `raw_mode = true`. Disables ICANON (line buffering), ECHO
  (local echo), and ISIG (signal generation), so each keystroke
  arrives immediately as bytes — including the multi-byte escape
  sequences for arrow keys and function keys.
- **Automatic terminal restoration**: the original termios is
  saved at install time and restored at process exit via
  `atexit`. The user's shell isn't left in a wedged state if the
  guest crashes or is killed normally.
- **Polling pattern**: the guest's `_start` does `sys_read →
  sys_yield → sys_read` forever. This is the canonical
  game-loop / TUI pattern.

## Build and run

```
./build.sh
./build/host
```

You'll see a banner and then a flashing cursor waiting for input.
Press some keys — try arrow keys, Home, End, F1, etc. Press `q`
to quit:

```
keydump: press 'q' to quit
[1B][5B][41][1B][5B][42][1B][5B][44][1B][5B][43][71]
keydump: 'q' pressed, exiting
```

Reading that output:
- `[1B][5B][41]` = ESC `[` A = **Up arrow**
- `[1B][5B][42]` = ESC `[` B = **Down arrow**
- `[1B][5B][44]` = ESC `[` D = **Left arrow**
- `[1B][5B][43]` = ESC `[` C = **Right arrow**
- `[71]` = `q`

These are the standard xterm sequences, which PuTTY also produces.

## Important caveat: Ctrl-C doesn't work in raw mode

With raw mode enabled, pressing Ctrl-C does NOT generate SIGINT
— the byte `0x03` is delivered to the guest like any other key.
To exit this example, press `q` (the guest exits and the host
follows).

If you somehow wedge your terminal (e.g., from a crash before the
atexit hook runs), the `reset` command from another shell will
restore it. Or close and reopen the terminal.

## Things to try

Try pressing these and see what bytes come through:

- **Shift+Arrow keys**: `ESC [ 1 ; 2 A` (etc.) — modifier-encoded
- **Function keys F1–F4**: `ESC O P/Q/R/S`
- **F5 onwards**: `ESC [ 15 ~`, `ESC [ 17 ~`, etc.
- **Home / End**: `ESC [ H` / `ESC [ F` in xterm mode
- **Backspace / Delete**: 0x7F or 0x08 (terminal-dependent)
- **Tab**: 0x09
- **Enter**: 0x0D (CR) in raw mode (NOT translated to LF)

This is the byte stream a guest-side terminal-event parser would
consume. A future `vm_term.h` guest library would convert these
into structured events (key + modifier + name).
