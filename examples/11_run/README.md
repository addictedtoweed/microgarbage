# 11_run — universal guest runner (the desktop dev-kit loop)

The local counterpart to loading a guest on the Pi dev kit. `build/run`
is a host that installs the common services — **stdio**, the **platform**
layer (`printf` / `rand` / realtime), and the simulated **hardware-IO**
backend (GPIO/I2C/SPI/ADC/PWM) — with roomy pools, loads an ELF from the
command line, and runs it to exit.

Paired with `tools/rvcc`, this is the whole dev-kit loop:

```sh
./build.sh run                      # builds the runner + hello.elf (via rvcc), runs it
# or by hand:
../../tools/rvcc -o app.elf app.c   # compile a guest (frozen ABI, guest.ld as-is)
./build/run app.elf                 # run it with stdio + printf + hardware sim
```

`build.sh` compiles `hello.c` with **rvcc** (dogfooding the dev-kit
compiler), then runs it. Expected output:

```
hello from rvcc + run!
  2 + 40 = 42
  fib: 0 1 1 2 3 5 8 13
```

The same `app.elf` runs unchanged on the Pi against the real
`platform_rpi.c` backend — only the host shim changes. That is the
"develop on the dev kit, deploy the same binary to the micro" story from
`docs/rpi-port.md`.

## Why a dedicated runner

`printf` is formatted **host-side** via `SYS_FORMAT_AND_WRITE`, which is
registered by `vm_host_install_platform` — not by `install_stdio` alone.
Hosts that install only stdio (01_hello, 10_hwio_sim) can `SYS_WRITE`
raw bytes but a libc guest's `printf` is silent there. This runner
installs the platform layer, so rvcc-built libc guests just work.
