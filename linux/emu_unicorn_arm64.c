/*
 * ARM64 Unicorn Engine backend for iSH — page-level mapping
 */

#include <unicorn/unicorn.h>
#include <unicorn/arm64.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

#include "emu/cpu.h"
#include "emu/mmu.h"
#include "emu/interrupt.h"
#include "emu/arch/arm64/cpu.h"
#include "kernel/memory.h"

static uc_engine *g_uc = NULL;
static uc_hook g_intr_hook = 0;
static uc_hook g_mem_hook = 0;
static bool g_uc_init = false;

void uc_page_map(uint64_t guest_addr, void *host_ptr, uint32_t size, int prot)
{
    if (!g_uc_init) return;
    uc_mem_map_ptr(g_uc, guest_addr, size, prot, host_ptr);
}

void uc_page_unmap(uint64_t guest_addr, uint32_t size)
{
    if (!g_uc_init) return;
    uc_mem_unmap(g_uc, guest_addr, size);
}

static void uc_intr_cb(uc_engine *uc, uint32_t intno, void *user_data)
{
    (void)uc; (void)intno; (void)user_data;
}

static int uc_mem_cb(uc_engine *uc, uc_mem_type type,
                     uint64_t address, uint32_t size,
                     int64_t value, void *user_data)
{
    (void)uc; (void)type; (void)address; (void)size; (void)value; (void)user_data;
    return UC_ERR_READ_UNMAPPED;
}

static void load_regs(uc_engine *uc, struct cpu_state *cpu)
{
    for (int i = 0; i < 31; i++) {
        uint64_t val = cpu->regs[i];
        uc_reg_write(uc, UC_ARM64_REG_X0 + i, &val);
    }
    uint64_t sp = cpu->sp;
    uc_reg_write(uc, UC_ARM64_REG_SP, &sp);
    uint64_t pc = cpu->pc;
    uc_reg_write(uc, UC_ARM64_REG_PC, &pc);
    uint32_t nzcv = cpu->nzcv;
    uc_reg_write(uc, UC_ARM64_REG_NZCV, &nzcv);
    uint64_t tls = cpu->tls_ptr;
    uc_reg_write(uc, UC_ARM64_REG_TPIDR_EL0, &tls);
}

static void save_regs(uc_engine *uc, struct cpu_state *cpu)
{
    for (int i = 0; i < 31; i++) {
        uint64_t val;
        uc_reg_read(uc, UC_ARM64_REG_X0 + i, &val);
        cpu->regs[i] = val;
    }
    uint64_t sp;
    uc_reg_read(uc, UC_ARM64_REG_SP, &sp);
    cpu->sp = sp;
    uint64_t pc;
    uc_reg_read(uc, UC_ARM64_REG_PC, &pc);
    cpu->pc = pc;
    uint32_t nzcv;
    uc_reg_read(uc, UC_ARM64_REG_NZCV, &nzcv);
    cpu->nzcv = nzcv;
    cpu->nf = (uint8_t)((nzcv >> 31) & 1);
    cpu->zf = (uint8_t)((nzcv >> 30) & 1);
    cpu->cf = (uint8_t)((nzcv >> 29) & 1);
    cpu->vf = (uint8_t)((nzcv >> 28) & 1);
    uint64_t tls;
    uc_reg_read(uc, UC_ARM64_REG_TPIDR_EL0, &tls);
    cpu->tls_ptr = tls;
}

__attribute__((constructor))
static void unicorn_early_init(void)
{
    uc_err err = uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &g_uc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "[unicorn] early init failed: %s\n", uc_strerror(err));
        return;
    }

    uc_hook_add(g_uc, &g_intr_hook, UC_HOOK_INTR, uc_intr_cb, 0, 0, UINT64_MAX);
    uc_hook_add(g_uc, &g_mem_hook, UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_FETCH_UNMAPPED, uc_mem_cb, 0, 0, UINT64_MAX);

    g_uc_init = true;
    fprintf(stderr, "[unicorn] early init done\n");
}

int cpu_run_to_interrupt_unicorn(struct cpu_state *cpu, struct tlb *tlb)
{
    (void)tlb;

    if (!g_uc_init) return INT_GPF;

    load_regs(g_uc, cpu);

    uc_err err = uc_emu_start(g_uc, cpu->pc, 0, 0, 0);

    save_regs(g_uc, cpu);

    if (err != UC_ERR_OK && err != UC_ERR_READ_UNMAPPED && err != UC_ERR_WRITE_UNMAPPED && err != UC_ERR_FETCH_UNMAPPED) {
        fprintf(stderr, "[unicorn] uc_emu_start error: %s\n", uc_strerror(err));
        return INT_GPF;
    }

    if (cpu->segfault_addr != 0) {
        cpu->segfault_addr = 0;
        return INT_GPF;
    }

    return INT_SYSCALL;
}

void cpu_poke(struct cpu_state *cpu)
{
    (void)cpu;
}
