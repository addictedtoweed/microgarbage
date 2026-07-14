# 10_hwio_sim — hardware-I/O ecalls on the simulation backend

Proves the portable hardware-I/O ABI (GPIO / I2C / SPI / ADC / PWM,
`SYS_*` 1250–1269) end to end on a PC, with no real hardware.

The guest (`guest.c`) issues transactions through the portable
`examples/common/guest/hwio.h` wrappers. The host (`host.c`) installs
the handlers with `vm_host_install_hwio()` and links the **desktop
simulation backend** (`src/host/platform_hwio_sim.c`) — a fake 64-pin
GPIO bank plus a fake LM75-style I2C temperature sensor at address
`0x48`. The path exercised is:

```
guest hwio_* -> SYS_* ecall -> vm_translate_read/write -> host_hwio_* -> back
```

The same guest `.elf` would run unchanged on the Pi dev kit against the
real `platform_rpi.c` backend — only the shim changes. That is the
"develop against real sensors on the Pi, ship the same binary to the
micro" story from `docs/rpi-port.md`.

## Build & run

```sh
./build.sh run
```

Requires a host C compiler and a RISC-V cross-compiler (rv32imc). This
host links two sources that are intentionally NOT in `VM_CORE_SRCS`
(so the core stays untouched for other builds):
`src/vm/vm_host_hwio.c` and `src/host/platform_hwio_sim.c`.

## Expected output

```
== hwio sim demo ==
gpio17         = 1
gpio17 toggled = 0
temp @0x48     = 25.5 C
i2c @0x50 absent : rc=-5 (expect nonzero)
adc ch0        = 512
== done ==
```

The MCU/desktop memory note: this host uses roomy 1 MB pools because
the default slab bucket layout outgrew `01_hello`'s old 64 KB. Real
targets tune `slab_config` + pool sizes to fit.
