# Examples

Working VM embeddings, ordered roughly by complexity. Each
example is fully self-contained — its own host source, its own
guest source(s), and a `build.sh` that links the host against
the VM library in this repo.

## Prerequisites

- A C compiler. `cc`, `gcc`, or `clang` all work. Set `CC` to
  override.
- For rebuilding guest `.elf` files: `gcc-riscv64-unknown-elf`
  (Ubuntu: `sudo apt install gcc-riscv64-unknown-elf`). If the
  cross-compiler is missing, the build script will reuse any
  existing pre-built `.elf` rather than failing.

## Running them

Each example builds independently:

```
cd 01_hello
./build.sh run
```

Or build only:

```
./build.sh
./build/host
```

Or clean:

```
./build.sh clean
```

## The examples

| Dir         | What it shows                                              |
|-------------|------------------------------------------------------------|
| `01_hello`  | Minimal single-guest embedding. SYS_WRITE, SYS_EXIT.       |
| `02_counter`| Cooperative scheduling with SYS_YIELD; manual scheduler loop. |
| `03_mailbox`| Two guests talking via SYS_SEND/SYS_RECV. The "VM OS" pattern. |
| `04_keydump`| Raw-mode terminal input via SYS_READ. Foundation for TUI work. |
| `05_shell`  | Interactive file-system shell. Demonstrates the file ECALL group (openat/read/write/close/lseek/mkdirat/unlinkat/readdir) against a FatFs volume mounted on a trashdrive. **Requires FatFs** (see `third_party/fatfs/PLACEHOLDER.md`). |

## Shared bits

- `common/guest.ld` — linker script every guest uses. Defines
  the four address regions and where each section goes.
- `common/vm_objs.sh` — sourced by each `build.sh`; defines
  `$REPO_ROOT`, `$CC`, `$CFLAGS`, the list of VM library source
  files (`$VM_CORE_SRCS`), and guest-toolchain settings.

If you copy an example as a starting point for your own
embedding: keep `../common/` accessible from your `build.sh`, or
inline the relevant bits if you're moving the example out of
the repo.

## Output directories

Each `build.sh` creates a `build/` subdirectory next to itself:

```
01_hello/
├── build/              # generated; .gitignore excludes
│   ├── host
│   └── guest.elf
├── build.sh
├── host.c
├── guest.c
└── README.md
```

The repo's `.gitignore` should include `examples/*/build/`.
