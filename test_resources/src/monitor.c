#include "io.h"
#include "power.h"
#include "syscall.h"
#include "trap.h"
#include <stdint.h>

void* memcpy(void* dst, const void* src, unsigned long n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    for (unsigned long i = 0; i < n; i++) {
        d[i] = s[i];
    }
    return dst;
}

void* memset(void* dst, int c, unsigned long n) {
    unsigned char* d = (unsigned char*)dst;
    for (unsigned long i = 0; i < n; i++) {
        d[i] = (unsigned char)c;
    }
    return dst;
}

void* memmove(void* dst, const void* src, unsigned long n) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    if (d < s) {
        for (unsigned long i = 0; i < n; i++) {
            d[i] = s[i];
        }
    } else {
        for (unsigned long i = n; i > 0; i--) {
            d[i - 1] = s[i - 1];
        }
    }
    return dst;
}

#define NUM_TASKS 4
#define USER_STACK_SIZE 0x2000
#define TRAP_STACK_SIZE 0x8000
#define TIMER_INTERVAL 20000ULL
#define CLINT_BASE 0x02000000ULL
#define CLINT_MTIME (CLINT_BASE + 0xbff8)
#define CLINT_MTIMECMP (CLINT_BASE + 0x4000)

#define SYS_WRITE_CHAR 1
#define SYS_WRITE_STR 2
#define SYS_YIELD 3
#define SYS_SLEEP 4
#define SYS_EXIT 5
#define SYS_SPAWN 6

enum {
    TASK_DORMANT,
    TASK_READY,
    TASK_RUNNING,
    TASK_SLEEPING,
    TASK_EXITED,
};

typedef struct {
    TrapContext ctx;
    void (*entry)(void);
    uint8_t* stack_top;
    uint64_t sleep_until;
    int state;
} Task;

static Task tasks[NUM_TASKS];
static uint8_t user_stacks[NUM_TASKS][USER_STACK_SIZE] __attribute__((aligned(16)));
static uint8_t trap_stack[TRAP_STACK_SIZE] __attribute__((aligned(16)));
static TrapContext* trap_frame =
    (TrapContext*)(trap_stack + TRAP_STACK_SIZE - sizeof(TrapContext));

static int current = -1;
static uint64_t ticks = 0;
static uint64_t switches = 0;
static uint64_t exceptions = 0;
static uint64_t last_cause = 0;
static uint64_t last_mepc = 0;
static uint64_t last_mtval = 0;

static inline uint64_t mmio_read64(uint64_t addr) {
    return *(volatile uint64_t*)addr;
}

static inline void mmio_write64(uint64_t addr, uint64_t value) {
    *(volatile uint64_t*)addr = value;
}

static void rearm_timer(void) {
    mmio_write64(CLINT_MTIMECMP, mmio_read64(CLINT_MTIME) + TIMER_INTERVAL);
}

static void zero_ctx(TrapContext* ctx) {
    uint64_t* p = (uint64_t*)ctx;
    for (unsigned i = 0; i < sizeof(TrapContext) / sizeof(uint64_t); i++) {
        p[i] = 0;
    }
}

static void init_task_ctx(int id, void (*entry)(void)) {
    Task* task = &tasks[id];
    zero_ctx(&task->ctx);

    uint64_t mstatus = read_csr_mstatus();
    mstatus &= ~((3ULL << 11) | (1ULL << 3) | (1ULL << 17));
    mstatus |= (1ULL << 7);

    task->ctx.mstatus = mstatus;
    task->ctx.mepc = (uint64_t)entry;
    task->ctx.x[2] = (uint64_t)task->stack_top;
    task->ctx.x[10] = (uint64_t)id;
    task->state = TASK_READY;
}

static int pick_next(int from) {
    for (int i = 1; i <= NUM_TASKS; i++) {
        int idx = (from + i) % NUM_TASKS;
        if (tasks[idx].state == TASK_READY) {
            return idx;
        }
    }
    return -1;
}

static int any_ready(void) {
    for (int i = 0; i < NUM_TASKS; i++) {
        if (tasks[i].state == TASK_READY) {
            return i;
        }
    }
    return -1;
}

static void wake_sleepers(void) {
    for (int i = 0; i < NUM_TASKS; i++) {
        if (tasks[i].state == TASK_SLEEPING && tasks[i].sleep_until <= ticks) {
            tasks[i].state = TASK_READY;
        }
    }
}

static void finish(void) {
    printf("\n[monitor] all tasks finished\n");
    printf("[monitor] timer ticks=%lld switches=%lld exceptions=%lld\n",
           (long long)ticks, (long long)switches, (long long)exceptions);
    printf("[monitor] last trap: mcause=%llx mepc=%llx mtval=%llx\n",
           (unsigned long long)last_cause, (unsigned long long)last_mepc,
           (unsigned long long)last_mtval);
    pass();
}

static void idle_wait(void) {
    for (;;) {
        if (any_ready() >= 0) {
            return;
        }

        int sleeping = 0;
        uint64_t min_wake = ~0ULL;
        for (int i = 0; i < NUM_TASKS; i++) {
            if (tasks[i].state == TASK_SLEEPING) {
                sleeping = 1;
                if (tasks[i].sleep_until < min_wake) {
                    min_wake = tasks[i].sleep_until;
                }
            }
        }
        if (!sleeping) {
            return;
        }

        uint64_t delta = min_wake > ticks ? min_wake - ticks : 1;
        uint64_t target = mmio_read64(CLINT_MTIME) + delta * TIMER_INTERVAL;
        while (mmio_read64(CLINT_MTIME) < target) {
        }
        ticks = min_wake;
        wake_sleepers();
        rearm_timer();
    }
}

static void schedule(TrapContext* ctx) {
    if (current >= 0 && tasks[current].state == TASK_RUNNING) {
        tasks[current].state = TASK_READY;
    }

    int next = pick_next(current);
    if (next < 0) {
        idle_wait();
        next = pick_next(current);
    }
    if (next < 0) {
        finish();
    }

    current = next;
    tasks[current].state = TASK_RUNNING;
    *ctx = tasks[current].ctx;
    switches++;
}

static int handle_syscall(TrapContext* ctx, int cur) {
    uint64_t nr = ctx->x[17];
    long a0 = (long)ctx->x[10];
    long ret = 0;

    ctx->mepc += 4;

    switch (nr) {
    case SYS_WRITE_CHAR:
        printf("%c", (char)a0);
        break;
    case SYS_WRITE_STR:
        print_str((const char*)a0);
        break;
    case SYS_YIELD:
        tasks[cur].state = TASK_READY;
        ctx->x[10] = 0;
        return 1;
    case SYS_SLEEP: {
        uint64_t n = (uint64_t)a0;
        if (n == 0) {
            n = 1;
        }
        tasks[cur].state = TASK_SLEEPING;
        tasks[cur].sleep_until = ticks + n;
        ctx->x[10] = 0;
        return 1;
    }
    case SYS_EXIT:
        tasks[cur].state = TASK_EXITED;
        ctx->x[10] = 0;
        return 1;
    case SYS_SPAWN: {
        int id = (int)a0;
        if (id >= 0 && id < NUM_TASKS && tasks[id].state == TASK_DORMANT) {
            init_task_ctx(id, tasks[id].entry);
        } else {
            ret = -1;
        }
        break;
    }
    default:
        printf("[monitor] unknown syscall %lld\n", (long long)nr);
        ret = -1;
        break;
    }

    ctx->x[10] = ret;
    return 0;
}

static void timer_tick(void) {
    ticks++;
    rearm_timer();
    wake_sleepers();
}

void trap_handler(TrapContext* ctx) {
    uint64_t cause = read_csr_mcause();
    int cur = current;
    int switch_needed = 0;

    if (cause & (1ULL << 63)) {
        uint64_t code = cause & 0xff;
        if (code == 7) {
            timer_tick();
            switch_needed = 1;
        } else {
            printf("[monitor] unexpected interrupt %lld\n", (long long)code);
        }
    } else {
        uint64_t code = cause & 0xff;
        if (code == 8) {
            switch_needed = handle_syscall(ctx, cur);
        } else if (code == 2) {
            exceptions++;
            last_cause = cause;
            last_mepc = ctx->mepc;
            last_mtval = read_csr_mtval();
            printf("[monitor] illegal instruction: mepc=%llx mtval=%llx (skipped)\n",
                   (unsigned long long)last_mepc, (unsigned long long)last_mtval);
            ctx->mepc += 4;
        } else if (code == 5 || code == 7) {
            exceptions++;
            last_cause = cause;
            last_mepc = ctx->mepc;
            last_mtval = read_csr_mtval();
            printf("[monitor] %s fault: mepc=%llx mtval=%llx (task %d killed)\n",
                   code == 5 ? "load" : "store",
                   (unsigned long long)last_mepc, (unsigned long long)last_mtval, cur);
            tasks[cur].state = TASK_EXITED;
            switch_needed = 1;
        } else {
            exceptions++;
            last_cause = cause;
            last_mepc = ctx->mepc;
            last_mtval = read_csr_mtval();
            printf("[monitor] unexpected exception %lld: mepc=%llx mtval=%llx "
                   "(task %d killed)\n",
                   (long long)code, (unsigned long long)last_mepc,
                   (unsigned long long)last_mtval, cur);
            tasks[cur].state = TASK_EXITED;
            switch_needed = 1;
        }
    }

    if (cur >= 0) {
        tasks[cur].ctx = *ctx;
    }
    if (switch_needed) {
        schedule(ctx);
    }
    __traps_return(ctx);
}

static long sys_write_char(char c) {
    return __syscall1(SYS_WRITE_CHAR, (long)c);
}

static long sys_write_str(const char* s) {
    return __syscall1(SYS_WRITE_STR, (long)s);
}

static long sys_yield(void) {
    return __syscall0(SYS_YIELD);
}

static long sys_sleep(long n) {
    return __syscall1(SYS_SLEEP, n);
}

static long sys_exit(void) {
    return __syscall0(SYS_EXIT);
}

static long sys_spawn(long id) {
    return __syscall1(SYS_SPAWN, id);
}

static void task_a(void) {
    sys_write_str("A: hello\n");
    for (int i = 0; i < 4; i++) {
        sys_write_char('A');
        for (volatile int j = 0; j < 150000; j++) {
        }
        sys_yield();
    }

    sys_write_str("\nA: spawning D\n");
    sys_spawn(3);
    sys_write_str("A: sleeping 1 tick\n");
    sys_sleep(1);
    sys_write_str("A: woke up, done\n");
    sys_exit();
}

static void task_b(void) {
    sys_write_str("B: before illegal instruction\n");
    asm volatile(".4byte 0xffffffff");
    sys_write_str("B: after illegal instruction\n");
    sys_exit();
}

static void task_c(void) {
    sys_write_str("C: before load fault\n");
    volatile unsigned long long* p = (volatile unsigned long long*)0x11110000ULL;
    (void)*p;
    sys_write_str("C: should not print\n");
    sys_exit();
}

static void task_d(void) {
    sys_write_str("D: spawned task running\n");
    sys_exit();
}

int main(void) {
    TEST_START("monitor");

    tasks[0].entry = task_a;
    tasks[0].stack_top = user_stacks[0] + USER_STACK_SIZE;
    tasks[1].entry = task_b;
    tasks[1].stack_top = user_stacks[1] + USER_STACK_SIZE;
    tasks[2].entry = task_c;
    tasks[2].stack_top = user_stacks[2] + USER_STACK_SIZE;
    tasks[3].entry = task_d;
    tasks[3].stack_top = user_stacks[3] + USER_STACK_SIZE;

    for (int i = 0; i < NUM_TASKS; i++) {
        init_task_ctx(i, tasks[i].entry);
    }
    tasks[3].state = TASK_DORMANT;

    write_csr_mtvec((uint64_t)__traps_entry);
    write_csr_mscratch((uint64_t)(trap_stack + TRAP_STACK_SIZE));
    write_csr_mie(1ULL << 7);
    rearm_timer();

    printf("[monitor] starting 3 U-mode tasks (D dormant)\n");
    current = 0;
    tasks[0].state = TASK_RUNNING;
    *trap_frame = tasks[0].ctx;
    __traps_return(trap_frame);

    return 0;
}
