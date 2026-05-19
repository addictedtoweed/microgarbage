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
| `help`             | Show command list                         |
| `exit`             | Leave the shell                           |

Paths can be absolute (`/foo/bar`) or relative (`bar` ⇒
`<cwd>/bar`). `cd ..` works (one level up). `cd .` is a no-op
(stays in cwd).

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
- **No pipes / redirection**: this is a single-VM shell with no
  subprocess concept. The `write` command exists as a
  redirection substitute.
- **No completion / globbing**: no `*` expansion.
- **Volume persistence**: the trashdrive lives in host RAM; each
  run starts with a freshly-formatted volume. To persist across
  runs you'd skip `f_mkfs` and let FatFs auto-detect the
  existing layout — and use file-backed storage instead of RAM.

## Files

- `host.c` — sets up trashdrive + FatFs + VmSystem + stdio/fs
  bridges, loads `shell.elf`, runs scheduler
- `shell.c` — the guest shell (~500 lines, no libc)
- `build.sh` — builds both. Checks for FatFs presence first.
