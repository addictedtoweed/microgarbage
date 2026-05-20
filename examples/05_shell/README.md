# Example 05: Shell

An interactive file-system shell running as a VM guest, with
file operations backed by a FatFs volume on a trashdrive.

## What this demonstrates

- **The full file-syscall ABI** (`openat`, `read`, `write`,
  `close`, `lseek`, `mkdirat`, `unlinkat`, `readdir`) — every
  call the guest makes routes through `vm_host_fs.c` into
  FatFs into trashdrive.
- **Guest-side path resolution**: the shell tracks its own
  current working directory (since the VM has no native CWD
  concept) and prefixes relative paths before calling into the
  host.
- **Cooked-mode terminal input** — opposite of `04_keydump`.
  The terminal handles line editing; the guest reads complete
  lines from stdin via `SYS_READ` (which arrives one or more
  bytes at a time and assembles a line in a buffer).
- **A real "application" running as a guest** — not a demo
  loop. Most of the code is shell logic, not VM glue.

## Prerequisites

This example needs **FatFs** to be extracted under
`third_party/fatfs/source/`. See `third_party/fatfs/PLACEHOLDER.md`
for download instructions. Without FatFs, the host won't link
(it needs `f_open`, `f_read`, etc. from FatFs) and `build.sh`
will fail with a clear message.

The other examples (01–04) don't need FatFs and will continue
to build without it.

## Build and run

```
./build.sh run
```

You'll see something like:

```
05_shell: compiling host (with FatFs)...
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
(FatFs sectors, or host-fs bytes outside our address space) is
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
/home$ write docs/note.txt Hello FatFs from a VM
/home$ cat docs/note.txt
Hello FatFs from a VM
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

- **No history / arrow keys**: the shell uses cooked mode for
  simplicity. To add history you'd switch to raw mode (like
  `04_keydump`) and implement an input-edit loop with
  cursor-movement and history buffers.
- **No pipes / redirection**: the `write` command exists as a
  redirection substitute. Real pipes would need a way to route
  one VM's stdout into another's stdin.
- **No background jobs**: `run` is always synchronous. A
  hypothetical `run &` would need scheduler changes so the
  parent VM keeps running while the child does too.
- **No completion / globbing**: no `*` expansion.
- **Volume persistence**: the trashdrive lives in host RAM; each
  run starts with a freshly-formatted volume. To persist across
  runs you'd skip `f_mkfs` and let FatFs auto-detect the
  existing layout — and use file-backed storage instead of RAM.

## Files

- `host.c` — sets up trashdrive + FatFs + VmSystem + stdio/fs
  bridges, loads `shell.elf`, runs scheduler. Parses `--host-fs=`
  CLI args and configures the `/host` mount.
- `shell.c` — the guest shell (~600 lines, no libc)
- `host_files_src/` — sources for the sample spawnable guests
  (`hello.c`, `count.c`). `build.sh` compiles each into a
  matching `.elf` under `host_files/`.
- `host_files/` — auto-populated by `build.sh` with the sample
  ELFs. This directory is what the shell sees as `/host`.
- `build.sh` — builds the host, the shell guest, and each
  spawnable. Checks for FatFs presence first.
