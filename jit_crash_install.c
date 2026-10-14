// Crash-handler installation for the iOS app build.
//
// iSH's standalone CLI installs its crash_handler in main() before running
// the kernel. The iOS app boots the kernel through KernelBootC and never
// runs main(), so the JIT recovery handler was never installed there: the
// app's only SIGSEGV handler was a diagnostic backtracer that intercepted
// the recoverable JIT stale-TLB faults and killed the process (or, when it
// returned, looped on the re-fault and hung).
//
// This file replicates main.c's crash_handler plus the sigaction
// registration and exposes install_jit_crash_handler() for KernelBootC to
// call at boot.

#include <execinfo.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>

#include "asbestos/frame.h"
#include "cpu-offsets.h"

// Engine state (global, not TLS: the app runs one shared engine with a
// global current task, so a process-wide in_jit flag is correct here).
extern volatile sig_atomic_t in_jit;
extern volatile uint64_t jit_saved_pc;
extern volatile uint64_t jit_last_host_fault;
extern volatile uint64_t jit_last_x7;
extern volatile uint64_t jit_last_x10;
extern volatile int jit_crash_count;
#ifdef GUEST_ARM64
// Assembly trampoline: returns INT_JIT_CRASH via fiber_exit (defined in entry.S)
extern void jit_crash_trampoline(void);
#endif

#define CRASH_CPU_pc                 offsetof(struct cpu_state, pc)
#define CRASH_CPU_segfault_addr      offsetof(struct cpu_state, segfault_addr)
#define CRASH_CPU_segfault_was_write offsetof(struct cpu_state, segfault_was_write)
#define CRASH_LOCAL_jit_exit_sp      offsetof(struct fiber_frame, jit_exit_sp)

static_assert(CRASH_CPU_pc                 == CPU_pc,                 "cpu-offsets drift: pc");
static_assert(CRASH_CPU_segfault_addr      == CPU_segfault_addr,      "cpu-offsets drift: segfault_addr");
static_assert(CRASH_CPU_segfault_was_write == CPU_segfault_was_write, "cpu-offsets drift: segfault_was_write");
static_assert(CRASH_LOCAL_jit_exit_sp      == LOCAL_jit_exit_sp,      "cpu-offsets drift: jit_exit_sp");

static void jit_crash_handler(int sig, siginfo_t *info, void *ctx) {
#if defined(__aarch64__) && defined(GUEST_ARM64)
    // [T-ish-sigbus-guard] Only treat synchronous memory faults (si_code > 0)
    // with valid ucontext as JIT crashes. Async SIGBUS (e.g. from a dying
    // filesystem or bus error delivered to a non-JIT thread) must fall through
    // to the non-JIT dump path instead of corrupting a healthy thread's
    // ucontext.
    int synchronous_memory_fault =
        (sig == SIGSEGV || sig == SIGBUS) && info != NULL && info->si_code > 0 && ctx != NULL;
    // If we're inside JIT code and got SIGSEGV/SIGBUS, recover by redirecting
    // execution to jit_crash_trampoline via ucontext PC manipulation.
    // This avoids the overhead of _setjmp on every block entry.
    if (synchronous_memory_fault && in_jit) {
        ucontext_t *uc = (ucontext_t *)ctx;

        // _cpu is in x1 — pointer to cpu_state within fiber_frame
        uint64_t cpu_ptr = uc->uc_mcontext->__ss.__x[1];

        // Reconstruct guest segfault_addr from registers.
        // x7 = _addr (host pointer = data_minus_addr + guest_addr)
        // x10 may hold data_minus_addr from TLB lookup (but only on TLB HIT path)
        uint64_t x7 = uc->uc_mcontext->__ss.__x[7];
        uint64_t x10 = uc->uc_mcontext->__ss.__x[10];
        uint64_t guest_addr = (x7 - x10) & 0xffffffffffffULL;

        // Store diagnostic info for handle_interrupt to read
        jit_last_host_fault = (uint64_t)info->si_addr;
        jit_last_x7 = x7;
        jit_last_x10 = x10;
        jit_crash_count++;

        // Determine read/write from host ESR. Bit 6 (WnR): 0=read, 1=write.
        uint64_t esr = uc->uc_mcontext->__es.__esr;
        int was_write = (esr & 0x40) != 0;

        // Write crash info directly to cpu_state via _cpu pointer.
        // Each store must match the field's real width: segfault_was_write is a
        // `bool`, and the previous `*(int *)` store wrote 4 bytes over it and
        // the padding before `trapno`. The generated gadgets use `strb` for the
        // same field, so 1 byte is the correct width on both sides.
        *(uint64_t *)(cpu_ptr + CRASH_CPU_segfault_addr) = guest_addr;
        *(bool *)(cpu_ptr + CRASH_CPU_segfault_was_write) = (bool)was_write;
        // Restore guest PC to block start for re-execution
        *(uint64_t *)(cpu_ptr + CRASH_CPU_pc) = (uint64_t)jit_saved_pc;

        // Restore SP to the value saved by fiber_enter, so fiber_exit
        // can correctly pop the callee-saved register frame.
        uint64_t exit_sp = *(uint64_t *)(cpu_ptr + CRASH_LOCAL_jit_exit_sp);
        uc->uc_mcontext->__ss.__sp = exit_sp;

        // Redirect execution to crash trampoline (returns INT_JIT_CRASH)
        uc->uc_mcontext->__ss.__pc = (uint64_t)jit_crash_trampoline;

        // Unblock signal so it can fire again on next crash
        sigset_t unblock;
        sigemptyset(&unblock);
        sigaddset(&unblock, sig);
        sigprocmask(SIG_UNBLOCK, &unblock, NULL);

        // Signal handler returns; execution resumes at jit_crash_trampoline
        return;
    }
#endif

    // Non-JIT crash: dump state and exit
    char buf[512];
    int len;
    ucontext_t *uc = (ucontext_t *)ctx;
    len = snprintf(buf, sizeof(buf), "\n=== HOST CRASH: signal %d ===\nfault addr: %p\n", sig, info->si_addr);
    write(STDERR_FILENO, buf, len);
#ifdef __aarch64__
    len = snprintf(buf, sizeof(buf),
        "pc:  0x%llx\nlr:  0x%llx\nsp:  0x%llx\n"
        "x0:  0x%llx\nx1:  0x%llx\nx2:  0x%llx\n"
        "x7:  0x%llx\nx28: 0x%llx\n",
        uc->uc_mcontext->__ss.__pc, uc->uc_mcontext->__ss.__lr,
        uc->uc_mcontext->__ss.__sp,
        uc->uc_mcontext->__ss.__x[0], uc->uc_mcontext->__ss.__x[1],
        uc->uc_mcontext->__ss.__x[2],
        uc->uc_mcontext->__ss.__x[7], uc->uc_mcontext->__ss.__x[28]);
    write(STDERR_FILENO, buf, len);
#endif
    void *bt[20];
    int n = backtrace(bt, 20);
    backtrace_symbols_fd(bt, n, STDERR_FILENO);
    _exit(139);
}

void install_jit_crash_handler(void) {
    static char altstack[SIGSTKSZ];
    stack_t ss = {.ss_sp = altstack, .ss_size = SIGSTKSZ};
    sigaltstack(&ss, NULL);
    struct sigaction sa = {0};
    sa.sa_sigaction = jit_crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
}
