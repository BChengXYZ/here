#include "io.h"
#include "power.h"
#include "trap.h"
#include <stdint.h>

#define WATCHDOG_BASE 0x00102000ULL
#define WD_CTRL 0x00
#define WD_TIMEOUT 0x04
#define WD_STATUS 0x08

#define CONTROL_ENABLE (1u << 0)
#define CONTROL_FEED (1u << 1)
#define CONTROL_CLEAR (1u << 2)

#define PLIC_BASE 0x0c000000ULL
#define PLIC_PRIORITY(id) (PLIC_BASE + 4ULL * (id))
#define PLIC_ENABLE(ctx, id) (PLIC_BASE + 0x2000ULL + (ctx) * 0x80ULL + 4ULL * ((id) / 32))
#define PLIC_THRESHOLD(ctx) (PLIC_BASE + 0x200000ULL + (ctx) * 0x1000ULL)
#define PLIC_CLAIM(ctx) (PLIC_BASE + 0x200004ULL + (ctx) * 0x1000ULL)

#define WATCHDOG_IRQ 62
#define M_CONTEXT 0
#define CLINT_MTIME 0x0200bff8ULL

static inline uint32_t mmio_r32(uint64_t addr) {
    return *(volatile uint32_t*)addr;
}

static inline void mmio_w32(uint64_t addr, uint32_t value) {
    *(volatile uint32_t*)addr = value;
}

static inline uint64_t mmio_r64(uint64_t addr) {
    return *(volatile uint64_t*)addr;
}

static volatile uint64_t fired = 0;

static void die(const char* msg) {
    printf("[watchdog] FAIL: %s\n", msg);
    PowerOff();
    for (;;) {
    }
}

void trap_handler(TrapContext* ctx) {
    uint64_t mcause = read_csr_mcause();

    if (mcause == ((1ULL << 63) | 11)) {
        uint32_t irq = mmio_r32(PLIC_CLAIM(M_CONTEXT));
        if (irq == WATCHDOG_IRQ) {
            fired++;
            printf("[watchdog] interrupt #%lld status=%x\n", (long long)fired,
                   mmio_r32(WATCHDOG_BASE + WD_STATUS));
            mmio_w32(WATCHDOG_BASE + WD_CTRL,
                     CONTROL_ENABLE | CONTROL_FEED | CONTROL_CLEAR);
        }
        if (irq != 0) {
            mmio_w32(PLIC_CLAIM(M_CONTEXT), irq);
        }
    } else {
        printf("[watchdog] unexpected trap mcause=%llx mtval=%llx\n",
               (unsigned long long)mcause, (unsigned long long)read_csr_mtval());
        die("unexpected trap");
    }

    __traps_return(ctx);
}

static void wait_for_fire(uint64_t target) {
    uint64_t start = mmio_r64(CLINT_MTIME);
    while (fired < target) {
        if (mmio_r64(CLINT_MTIME) - start > 200000000ULL) {
            die("interrupt timeout");
        }
    }
}

int main(void) {
    TEST_START("watchdog_test");

    trap_init();

    mmio_w32(PLIC_PRIORITY(WATCHDOG_IRQ), 5);
    mmio_w32(PLIC_THRESHOLD(M_CONTEXT), 0);
    uint64_t enable_addr = PLIC_ENABLE(M_CONTEXT, WATCHDOG_IRQ);
    mmio_w32(enable_addr, mmio_r32(enable_addr) | (1u << (WATCHDOG_IRQ % 32)));

    mmio_w32(WATCHDOG_BASE + WD_TIMEOUT, 20);
    mmio_w32(WATCHDOG_BASE + WD_CTRL, CONTROL_ENABLE | CONTROL_FEED);
    printf("[watchdog] armed, timeout=20ms\n");

    wait_for_fire(1);
    printf("[watchdog] first timeout observed\n");

    mmio_w32(WATCHDOG_BASE + WD_CTRL, CONTROL_ENABLE | CONTROL_FEED);
    printf("[watchdog] re-armed\n");

    wait_for_fire(2);
    printf("[watchdog] second timeout observed\n");

    mmio_w32(WATCHDOG_BASE + WD_CTRL, 0);
    pass();
    return 0;
}
