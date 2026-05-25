# Example 05: Shell

An interactive file-system shell running as a VM guest, with
file operations backed by a trashfs RAM disk (`/td0`) and a
read-only host-directory passthrough (`/host`).

## What this demonstrates

- **The full file-syscall ABI** (`openat`, `read`, `write`,
  `close`, `lseek`, `mkdirat`, `unlinkat`, `readdir`) — every
  call the guest makes routes through `vm_host_fs.c` into
  trashfs (or the host-directory passthrough).
- **Guest-side path resolution**: the shell tracks its own
  current working directory (since the VM has no native CWD
  concept) and prefixes relative paths before calling into the
  host.
- **Raw-mode terminal input with history** — the shell runs
  in raw mode, owns its own line editing, and supports
  up/down history recall, Ctrl-L (clear screen), Ctrl-U (clear
  line), Ctrl-C (cancel line), and Ctrl-D (exit).
- **A real "application" running as a guest** — not a demo
  loop. Most of the code is shell logic, not VM glue.
- **Optional TCP / pty stdio routing** — `--tcp=<port>` listens
  on a localhost port that PuTTY (Raw/Telnet) or `nc` connects
  to; it's repeatable for multiple concurrent sessions, each
  with its own shell VM. `--pty` does the same over a POSIX
  pseudoterminal (Linux/Cygwin). Both decouple VM pacing from
  the launching terminal's scheduling.

## Prerequisites

None beyond the standard toolchain. The filesystem is the native,
public-domain `trashfs` (`src/storage/trashfs.c`), built straight
from this repo — there is no external library to download.

## Build and run

```
./build.sh run
```

You'll see something like:

```
05_shell: compiling host...
05_shell: compiling guest (RV32IMC)...
05_shell: built. To run:
    .../examples/05_shell/build/host

  Type 'help' once inside the shell for a command list.

05_shell: ===== running =====
VM shell — type 'help' for commands
/$
```

The host pre-creates `/home`, `/tmp`, and a `/readme.txt` so
`ls` shows something on first launch.

## Commands

| Command            | What it does                              |
|--------------------|-------------------------------------------|
| `ls [path]`        | List directory contents                   |
| `cd <path>`        | Change directory                          |
| `pwd`              | Print working directory                   |
| `mkdir <path>`     | Create directory                          |
| `rmdir <path>`     | Remove (empty) directory                  |
| `rm <path>`        | Remove file                               |
| `touch <path>`     | Create empty file                         |
| `cat <path>`       | Print file contents                       |
| `write <p> <text>` | Write text to file (truncating)           |
| `run <path>`       | Load and execute an ELF as a child VM     |
| `help`             | Show command list                         |
| `exit`             | Leave the shell                           |

Paths can be absolute (`/foo/bar`) or relative (`bar` ⇒
`<cwd>/bar`). `cd ..` works (one level up). `cd .` is a no-op
(stays in cwd).

## The `/host` mount and `run`

The host process exposes a real directory on the developer's
machine to the shell as `/host/...`. By default this is
`./host_files/` (auto-created if missing). You can drop any
file in there and read it from inside the shell via `/host/foo`.

The `run` command takes an ELF path and spawns it as a child
VM. The child runs synchronously — the shell blocks until the
child halts. Child output appears on the shell's terminal,
interleaved with the prompts.

Three sample spawnable ELFs are built into `./host_files/`:

- `hello.elf` — prints `hi from spawned VM` and exits
- `count.elf` — counts 1 to 10, one per line, and exits
- `snake.elf` — interactive snake game (WASD or hjkl to move,
  `q` to quit). Demonstrates raw-mode TTY, ANSI cursor control,
  and the kernel-managed auto-reload timer for an 8 FPS game
  loop. On exit it cleans up the screen and restores cooked
  mode automatically.

Example session:

```
[/]
$ run /host/hello.elf
hi from spawned VM

[/]
$ run /host/count.elf
1
2
3
4
5
6
7
8
9
10

[/]
$
```

ELFs always load with `VM_BACKING_COPY_RAM` — code and rodata
are copied into fresh RAM from the bump arena, not run XIP from
the source file. This is required because the source storage
(trashfs blocks, or host-fs bytes outside our address space) is
not necessarily contiguous in memory. Plan for roughly 20–30 KB
of RAM per spawned VM (code + rodata + 16 KB data region).

### Host-fs CLI options

```
./build/host [--host-fs=PATH] [--host-fs-rw] [--no-host-fs] [shell.elf]

  --host-fs=PATH     mount PATH as /host (default: ./host_files)
  --host-fs-rw       allow writes to /host (default: read-only)
  --no-host-fs       disable the /host mount entirely
```

The mount is **read-only by default** to keep a buggy or
malicious guest from clobbering real files. Pass `--host-fs-rw`
to opt into write access — useful if you want the shell to be
able to `write` or `touch` files visible to the host.

Path traversal escapes are rejected: `/host/../etc/passwd`
returns `-EPERM`. The mount is sandboxed to its configured root.

## Example session

```
/$ ls
  home/
  tmp/
  readme.txt      120

/$ cat readme.txt
Welcome to the VM shell.
Try: ls, cd /home, mkdir foo, touch bar.txt, cat readme.txt

/$ cd home
/home$ mkdir docs
/home$ touch docs/note.txt
/home$ write docs/note.txt Hello trashfs from a VM
/home$ cat docs/note.txt
Hello trashfs from a VM
/home$ ls docs
  note.txt        22

/home$ rm docs/note.txt
/home$ rmdir docs
/home$ ls
  (empty)

/home$ exit
bye
```

## What's not here

- **No pipes / redirection**: the `write` command exists as a
  redirection substitute. Real pipes would need a way to route
  one VM's stdout into another's stdin.
- **No background jobs**: `run` is always synchronous. A
  hypothetical `run &` would need scheduler changes so the
  parent VM keeps running while the child does too.
- **No completion / globbing**: no `*` expansion.
- **Volume persistence**: the trashfs volume lives in host RAM;
  each run starts with a freshly-formatted volume. To persist
  across runs you'd skip the `trashfs_format` call and mount a
  pre-formatted region backed by a file instead of RAM.

## Routing stdio over TCP (and pty)

By default the host binds to the launching terminal (mintty,
Windows Terminal, the inherited stdin/stdout). That couples the
VM's pacing to the local terminal's scheduling quantum, which can
stutter under load.

The `--tcp=<port>` flag instead listens on a TCP port and routes
all VM stdio (stdin, stdout, stderr) through the connection.
PuTTY (or `nc`) connects to `localhost:<port>` and the two
processes are scheduled independently. The flag is repeatable —
each port is its own listener, and each accepted connection gets
its own shell VM, so several clients can run concurrently against
the one host process (audio, the filesystem, etc. are shared).

To use it:

```
$ ./build/host.exe --tcp=5000
host: listening on TCP port 5000 (connect: nc localhost 5000)
# ...and for two concurrent sessions:
$ ./build/host.exe --tcp=5000 --tcp=5001
```

In PuTTY:

1. Open PuTTY.
2. Connection type: **Raw** (or **Telnet**).
3. Host Name: `localhost`, Port: `5000` (or whichever you passed).
4. Click **Open**.

Or from a shell: `nc localhost 5000`.

The shell banner appears in the client window; from there it works
exactly like running the host directly — `ls`, `cd`,
`run /host/snake.elf`, etc. Closing the client ends that session;
the host keeps running as long as a listener could still produce a
client. Lone `\n` output is translated to `\r\n` (Telnet clients
expect CRLF; raw `nc` doesn't care).

On POSIX hosts (Linux/Cygwin), `--pty` is the local-IPC equivalent:
it allocates a pseudoterminal and prints the slave path to attach a
terminal emulator (`screen /dev/pts/N`). `--pty` and `--tcp` are
mutually exclusive; multiple `--tcp` ports are fine.

## Files

- `host.c` — sets up the trashfs volume + VmSystem + stdio/fs
  bridges, loads `shell.elf`, runs scheduler. Parses CLI args
  including `--host-fs=`, `--tcp=`, and `--pty`.
- `shell.c` — the guest shell with a raw-mode line editor
  including up/down history recall, Ctrl-L, Ctrl-U, etc.
- `host_files_src/` — sources for the sample spawnable guests
  (`hello.c`, `count.c`, `snake.c`). `build.sh` compiles each
  into a matching `.elf` under `host_files/`.
- `host_files/` — auto-populated by `build.sh` with the sample
  ELFs. This directory is what the shell sees as `/host`.
- `build.sh` — builds the host, the shell guest, and each
  spawnable.
