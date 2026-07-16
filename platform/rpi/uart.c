/* platform/rpi/uart.c — BCM2835 PL011 UART0 (real serial output).
 *
 * The first real peripheral driver of the Pi port: the ARM PL011 UART at
 * 0x2020_1000. Replaces semihosting for OUTPUT — the C library's write()
 * is routed here (see _write below), so printf goes out the serial line.
 *
 * We still link newlib rdimon for startup + clean QEMU exit + the heap;
 * a real-hardware build swaps rdimon's crt0/_exit for a boot.S + halt
 * loop (the UART itself needs no semihosting).
 *
 * QEMU: raspi0 models the PL011 as serial0, so `-nographic` routes it to
 * stdio. Baud/clock values below matter on real hardware only (QEMU has
 * no UART timing model).
 *
 * Public domain (CC0). No warranty.
 */

#include <stdint.h>

#define PERIPH_BASE  0x20000000u
#define GPIO_BASE    (PERIPH_BASE + 0x200000u)
#define UART0_BASE   (PERIPH_BASE + 0x201000u)

#define REG(a)       (*(volatile uint32_t *)(a))
#define GPFSEL1      REG(GPIO_BASE + 0x04)
#define GPPUD        REG(GPIO_BASE + 0x94)
#define GPPUDCLK0    REG(GPIO_BASE + 0x98)

#define UART0_DR     REG(UART0_BASE + 0x00)
#define UART0_FR     REG(UART0_BASE + 0x18)
#define UART0_IBRD   REG(UART0_BASE + 0x24)
#define UART0_FBRD   REG(UART0_BASE + 0x28)
#define UART0_LCRH   REG(UART0_BASE + 0x2C)
#define UART0_CR     REG(UART0_BASE + 0x30)
#define UART0_IMSC   REG(UART0_BASE + 0x38)
#define UART0_ICR    REG(UART0_BASE + 0x44)

#define FR_TXFF      (1u << 5)   /* TX FIFO full */

static void short_delay(int n) { while (n-- > 0) __asm__ volatile("nop"); }

void uart_init(void) {
    UART0_CR = 0;                                  /* disable while configuring */

    /* Route GPIO14 (TXD0) / GPIO15 (RXD0) to ALT0 (=0b100). GPFSEL1 holds
     * FSEL10..19; FSEL14 = bits[14:12], FSEL15 = bits[17:15]. */
    uint32_t sel = GPFSEL1;
    sel &= ~((7u << 12) | (7u << 15));
    sel |=  ((4u << 12) | (4u << 15));
    GPFSEL1 = sel;

    /* Disable pull-up/down on 14,15 (the classic GPPUD/GPPUDCLK dance). */
    GPPUD = 0;                short_delay(150);
    GPPUDCLK0 = (1u << 14) | (1u << 15); short_delay(150);
    GPPUDCLK0 = 0;

    UART0_ICR = 0x7FF;                             /* clear pending interrupts */

    /* 115200 baud @ 48 MHz UART clock: 48e6/(16*115200) = 26.04 ->
     * IBRD=26, FBRD=round(0.04*64)=3. (QEMU ignores this.) */
    UART0_IBRD = 26;
    UART0_FBRD = 3;

    UART0_LCRH = (3u << 5) | (1u << 4);            /* 8 bits, FIFO enable */
    UART0_IMSC = 0;                                /* mask all interrupts (poll) */
    UART0_CR   = (1u << 0) | (1u << 8) | (1u << 9);/* UARTEN | TXE | RXE */
}

void uart_putc(char c) {
    while (UART0_FR & FR_TXFF) { }                 /* wait for TX space */
    UART0_DR = (uint32_t)(unsigned char)c;
}

void uart_puts(const char *s) {
    for (; *s; s++) {
        if (*s == '\n') uart_putc('\r');           /* CR-LF for terminals */
        uart_putc(*s);
    }
}

/* Newlib syscall override: send all stdout/stderr writes to the UART.
 * Objects win over library archive members, so this replaces rdimon's
 * semihosting _write while its _exit / _sbrk stay in use. */
int _write(int fd, char *buf, int len) {
    (void)fd;
    for (int i = 0; i < len; i++) {
        if (buf[i] == '\n') uart_putc('\r');
        uart_putc(buf[i]);
    }
    return len;
}
