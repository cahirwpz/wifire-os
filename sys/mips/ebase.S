#include <mips/asm.h>
#include <mips/m32c0.h>
#include <mips/pcpu.h>
#include <mips/regdef.h>
#include <mips/pmap.h>
#include <mips/vm_param.h>

#include "assym.h"

        # Don't allow the assembler to reorder instructions.
        .set	noreorder
        # Forbid the assembler from using $at register.
        .set	noat
        # Enable FPU instructions use since we need to save & restore FP context
        .set    hardfloat

#define SAVE_REG_CFI(reg, offset, base)                                        \
        sw reg, (CTX_##offset)(base);                                          \
        .cfi_rel_offset reg, (CTX_##offset)

#define SAVE_REG(reg, offset, base) sw reg, (CTX_##offset)(base)

#define LOAD_REG(reg, offset, base) lw reg, (CTX_##offset)(base)

#define LOAD_FPU_REG(reg, offset, base) lwc1 reg, (FPU_CTX_##offset)(base)

#define SAVE_CPU_CTX(reg)                                                      \
        SAVE_REG_CFI(AT, AT, reg);                                             \
        SAVE_REG_CFI(v0, V0, reg);                                             \
        SAVE_REG_CFI(v1, V1, reg);                                             \
        SAVE_REG_CFI(a0, A0, reg);                                             \
        SAVE_REG_CFI(a1, A1, reg);                                             \
        SAVE_REG_CFI(a2, A2, reg);                                             \
        SAVE_REG_CFI(a3, A3, reg);                                             \
        SAVE_REG_CFI(t0, T0, reg);                                             \
        SAVE_REG_CFI(t1, T1, reg);                                             \
        SAVE_REG_CFI(t2, T2, reg);                                             \
        SAVE_REG_CFI(t3, T3, reg);                                             \
        SAVE_REG_CFI(t4, T4, reg);                                             \
        SAVE_REG_CFI(t5, T5, reg);                                             \
        SAVE_REG_CFI(t6, T6, reg);                                             \
        SAVE_REG_CFI(t7, T7, reg);                                             \
        SAVE_REG_CFI(s0, S0, reg);                                             \
        SAVE_REG_CFI(s1, S1, reg);                                             \
        SAVE_REG_CFI(s2, S2, reg);                                             \
        SAVE_REG_CFI(s3, S3, reg);                                             \
        SAVE_REG_CFI(s4, S4, reg);                                             \
        SAVE_REG_CFI(s5, S5, reg);                                             \
        SAVE_REG_CFI(s6, S6, reg);                                             \
        SAVE_REG_CFI(s7, S7, reg);                                             \
        SAVE_REG_CFI(t8, T8, reg);                                             \
        SAVE_REG_CFI(t9, T9, reg);                                             \
        SAVE_REG_CFI(gp, GP, reg);                                             \
        SAVE_REG_CFI(fp, FP, reg);                                             \
        /* Save value of user-space RA just before syscall. */                 \
        SAVE_REG(ra, RA, reg);                                                 \
        .cfi_rel_offset ra, CTX_RA;                                            \
        mfc0 ra, C0_EPC;                                                       \
        mflo t0;                                                               \
        mfhi t1;                                                               \
        SAVE_REG(t0, LO, reg);                                                 \
        SAVE_REG(t1, HI, reg);

#define LOAD_CPU_CTX()                                                         \
        LOAD_REG(t0, HI, sp);                                                  \
        LOAD_REG(t1, LO, sp);                                                  \
        mthi t0;                                                               \
        mtlo t1;                                                               \
        LOAD_REG(ra, RA, sp);                                                  \
        LOAD_REG(fp, FP, sp);                                                  \
        LOAD_REG(gp, GP, sp);                                                  \
        LOAD_REG(t9, T9, sp);                                                  \
        LOAD_REG(t8, T8, sp);                                                  \
        LOAD_REG(s7, S7, sp);                                                  \
        LOAD_REG(s6, S6, sp);                                                  \
        LOAD_REG(s5, S5, sp);                                                  \
        LOAD_REG(s4, S4, sp);                                                  \
        LOAD_REG(s3, S3, sp);                                                  \
        LOAD_REG(s2, S2, sp);                                                  \
        LOAD_REG(s1, S1, sp);                                                  \
        LOAD_REG(s0, S0, sp);                                                  \
        LOAD_REG(t7, T7, sp);                                                  \
        LOAD_REG(t6, T6, sp);                                                  \
        LOAD_REG(t5, T5, sp);                                                  \
        LOAD_REG(t4, T4, sp);                                                  \
        LOAD_REG(t3, T3, sp);                                                  \
        LOAD_REG(t2, T2, sp);                                                  \
        LOAD_REG(t1, T1, sp);                                                  \
        LOAD_REG(t0, T0, sp);                                                  \
        LOAD_REG(a3, A3, sp);                                                  \
        LOAD_REG(a2, A2, sp);                                                  \
        LOAD_REG(a1, A1, sp);                                                  \
        LOAD_REG(a0, A0, sp);                                                  \
        LOAD_REG(v1, V1, sp);                                                  \
        LOAD_REG(v0, V0, sp);                                                  \
        LOAD_REG(AT, AT, sp);                                                  \
        LOAD_REG(sp, SP, sp)

#define LOAD_FPU_CTX()                                                         \
        LOAD_FPU_REG($f0, F0, sp);                                             \
        LOAD_FPU_REG($f1, F1, sp);                                             \
        LOAD_FPU_REG($f2, F2, sp);                                             \
        LOAD_FPU_REG($f3, F3, sp);                                             \
        LOAD_FPU_REG($f4, F4, sp);                                             \
        LOAD_FPU_REG($f5, F5, sp);                                             \
        LOAD_FPU_REG($f6, F6, sp);                                             \
        LOAD_FPU_REG($f7, F7, sp);                                             \
        LOAD_FPU_REG($f8, F8, sp);                                             \
        LOAD_FPU_REG($f9, F9, sp);                                             \
        LOAD_FPU_REG($f10, F10, sp);                                           \
        LOAD_FPU_REG($f11, F11, sp);                                           \
        LOAD_FPU_REG($f12, F12, sp);                                           \
        LOAD_FPU_REG($f13, F13, sp);                                           \
        LOAD_FPU_REG($f14, F14, sp);                                           \
        LOAD_FPU_REG($f15, F15, sp);                                           \
        LOAD_FPU_REG($f16, F16, sp);                                           \
        LOAD_FPU_REG($f17, F17, sp);                                           \
        LOAD_FPU_REG($f18, F18, sp);                                           \
        LOAD_FPU_REG($f19, F19, sp);                                           \
        LOAD_FPU_REG($f20, F20, sp);                                           \
        LOAD_FPU_REG($f21, F21, sp);                                           \
        LOAD_FPU_REG($f22, F22, sp);                                           \
        LOAD_FPU_REG($f23, F23, sp);                                           \
        LOAD_FPU_REG($f24, F24, sp);                                           \
        LOAD_FPU_REG($f25, F25, sp);                                           \
        LOAD_FPU_REG($f26, F26, sp);                                           \
        LOAD_FPU_REG($f27, F27, sp);                                           \
        LOAD_FPU_REG($f28, F28, sp);                                           \
        LOAD_FPU_REG($f29, F29, sp);                                           \
        LOAD_FPU_REG($f30, F30, sp);                                           \
        LOAD_FPU_REG($f31, F31, sp);                                           \
        LOAD_FPU_REG($31, FSR, sp)

#define LOAD_PCPU_KSEG0(reg)                                                   \
        LA reg, MIPS_KSEG2_TO_KSEG0(_pcpu_data)

# Disable interrupts and extract interrupt mask into reg.
#define DISABLE_INT(reg)                                                       \
        di      reg;                                                           \
        ehb;                                                                   \
        ext     reg, reg, SR_IMASK_SHIFT, SR_IMASK_BITS

# Remove given bits from the status register.
#define STATUS_CLEAR(mask)                                                     \
        mfc0    k0, C0_STATUS;                                                 \
        li      k1, ~(mask);                                                   \
        and     k0, k1;                                                        \
        mtc0    k0, C0_STATUS

# Add given bits to the status register.
#define STATUS_SET(mask)                                                       \
        mfc0    k0, C0_STATUS;                                                 \
        li      k1, mask;                                                      \
        or      k0, k1;                                                        \
        mtc0    k0, C0_STATUS

#define PCPU_SAVE(reg)                                                         \
        mfc0    k1, C0_##reg;                                                  \
        sw      k1, PCPU_##reg(k0)

#define PCPU_RESTORE(reg)                                                      \
        lw      k1, PCPU_##reg(k0);                                            \
        mtc0    k1, C0_##reg

#define PCPU_TO_FRAME(reg)                                                     \
        lw      t1, PCPU_##reg(t0);                                            \
        SAVE_REG(t1, reg, sp)

        .global _ebase

        .section .ebase, "ax"

_ebase:

        .org    0x0

# TLB miss handler must avoid generating TLB miss during PT search.
# 
# If you use TLBR instruction remember that it overwrites EntryHi!
#
# Please verify EHB (clear execution hazard) instruction placement against
# chapter 2.8 of "MIPS32® 24KETM Processor Core Family Software User’s Manual".
# EHB has to be put between a producer and a consumer - preferably just before
# the consumer, not just after the producer.

# MIPS kseg0 plays role of direct map from other architectures,
# i.e. it exposes 1:1 mapping between effective and physical addresses.
#
# Page directory entries are not stored in TLB but are processed by TLB refill
# handler, hence they contain kseg0 addresses instead of physical one.

# useg: [0000.0000, 8000.0000) -> ffff.e000 UPD (ASID = x)
# kseg: [c000.0000, ffff.e000) -> ffff.f000 KPD (GLOBAL)

# |uuuuuuuu________|________....kkkk|
# ^                ^
# fffe.0000        ffff.0000

# useg: [000x.xxxx, 800x.xxxx) -> 0000.0000 - 0000.07ff -> 0000 - 07fc
# kseg: [c00x.xxxx, fffx.xxxx) -> ffff.fc00 - ffff.ffff -> 1c00 - 1ffc

SLEAF(tlb_refill)
        # Read PDE associated with bad virtual address.
        # Highest bit of the address switches between UPD_BASE & KPD_BASE,
        # so it's copied into 12th position with arithmetic shift.
        mfc0    k1, C0_BADVADDR
        sra     k1, 20
        andi    k1, 0x1ffc
        lw      k1, -0x2000(k1)         # [k1] PDE of bad virtual address

        # If PDE isn't valid behave as if TLBL/TLBS exception was triggered.
        andi    k0, k1, ENTRYLO0_V_MASK
        beqz    k0, exc_enter
        /* branch delay slot */

        # Calculate page table address from PDE
        srl     k1, 6
        sll     k0, k1, 12              # [k0] PTE address in kseg0
        # ... and index of even entry corresponding to bad virtual address.
        mfc0    k1, C0_BADVADDR
        srl     k1, 13                  # [k1] PTE index (lower 10 bits)
        ins     k0, k1, 3, 9

        # Simply grab the PTEs and load them into the TLB.
        lw      k1, 0(k0)
        mtc0    k1, C0_ENTRYLO0
        lw      k1, 4(k0)
        mtc0    k1, C0_ENTRYLO1
        ehb
        tlbwr
        eret
END(tlb_refill)

        .org    0x100
        .local  cache_error

SLEAF(cache_error)
1:      j       1b
        nop
END(cache_error)

        .org    0x180
        .local  general_exception

SLEAF(general_exception)
        j       exc_enter
        nop
END(general_exception)

        .org    0x200
        .local  irq

SLEAF(irq)
        j       exc_enter
        nop
END(irq)

        .org    0x500
        .global user_exc_leave
        .global kern_exc_leave

# [$k0] must be set to value of C0_STATUS
exc_enter:
        mfc0    k0, C0_STATUS
        andi    k0, SR_KSU_MASK         # Did exception occur in kernel mode?
        beqz    k0, kern_exc_enter
        nop

# IMPORTANT:
# - While exception level is active, the code may only access local pcpu
#   structure! FTTB It's accessed through kseg0.
# - It is UNSAFE to use k0/k1 registers when TLB miss handler may be triggered!
# - Status, cause, epc & badvaddr registers cannot be saved directly to kernel
#   stack (kseg2) as it may trigger TLB handler which would overwrite them!

SNESTED(user_exc_enter, MCONTEXT_SIZE, ra)
        .cfi_signal_frame

        # Fetch PCPU address
        LOAD_PCPU_KSEG0(k0)

        # Store sp, status, cause, epc, badvaddr registers in PCPU
        sw      sp, PCPU_SP(k0)
        PCPU_SAVE(STATUS)
        PCPU_SAVE(CAUSE)
        PCPU_SAVE(EPC)
        PCPU_SAVE(BADVADDR)

        # sp now contains kernel stack pointer.
        lw      sp, PCPU_KSP(k0)

        # Define a rule for computing CFA as: sp + 0
        .cfi_def_cfa sp, 0

        # We cannot access stack (KSEG2) when exception level is enabled
        # Let's enter kernel mode, drop exception level, disable interrupts
        STATUS_CLEAR(SR_KSU_MASK|SR_EXL|SR_IE)

        # Save context on the kernel stack
        SAVE_CPU_CTX(sp)

        # Copy sp, status, cause, epc, badvaddr from PCPU to stack frame
        LOAD_PCPU_KSEG0(t0)
        PCPU_TO_FRAME(SP)
        .cfi_rel_offset sp, CTX_SP
        PCPU_TO_FRAME(SR)
        PCPU_TO_FRAME(CAUSE)
        PCPU_TO_FRAME(PC)
        .cfi_return_column t1
        .cfi_rel_offset t1, CTX_PC
        PCPU_TO_FRAME(BADVADDR)

        # Load kernel global pointer.
        la      gp, _gp

        # Turn off FPU
        STATUS_CLEAR(SR_CU1)

        # Fetch thread control block pointer to s0 for later use.
        LOAD_PCPU(s0)
        lw      s0, PCPU_CURTHREAD(s0)

        # No exeception frame so set td_kframe to NULL.
        sw      $0, TD_KFRAME(s0)

        # Call C interrupt handler routine.
        la      t0, mips_exc_handler
        jalr    t0
        move    a0, sp                  # (delay) 1st arg

user_exc_leave:
        DISABLE_INT(t1)

        # Set current stack pointer to user exception frame.
        # This is crucial on first entry to user-space for this thread.
        LOAD_PCPU_KSEG0(s0)
        lw      s1, PCPU_CURTHREAD(s0)
        lw      sp, TD_UCTX(s1)

        # Update kernel stack pointer to be used on kernel reentry.
        sw      sp, PCPU_KSP(s0)

        # Copy pc register from stack frame to PCPU
        LOAD_REG(t0, PC, sp)
        sw      t0, PCPU_PC(s0)

        # Load status register from exception frame, update it with current
        # interrupt mask and exception mode, and store it within PCPU
        LOAD_REG(t0, SR, sp)
        ins     t0, t1, SR_IMASK_SHIFT, SR_IMASK_BITS
        ori     t0, SR_EXL
        sw      t0, PCPU_SR(s0)

        # If user FPU context is not saved, don't restore its registers.
        lw      t0, TD_PFLAGS(s1)
        and     t1, t0, TDP_FPUCTXSAVED
        beqz    t1, skip_fpu_restore
        nop

        # Clear TD_FPUCTXSAVED flag.
        xori    t0, t0, TDP_FPUCTXSAVED
        sw      t0, TD_PFLAGS(s1)

        # Enable FPU and load FPU context
        STATUS_SET(SR_CU1)
        LOAD_FPU_CTX()

skip_fpu_restore:
        # Load context from exception frame on stack, sp will get overwritten.
        LOAD_CPU_CTX()

        # Restore status and epc registers from PCPU
        LOAD_PCPU_KSEG0(k0)
        PCPU_RESTORE(STATUS)
        PCPU_RESTORE(EPC)

        # This label is useful for debugging.
        sync
user_return:
        eret
END(user_exc_enter)

SNESTED(kern_exc_enter, CTX_SIZE, ra)
        .cfi_signal_frame
        # Fetch PCPU address
        LOAD_PCPU_KSEG0(k0)

        # Store sp, status, cause, epc, badvaddr registers in PCPU
        sw      sp, PCPU_SP(k0)
        PCPU_SAVE(STATUS)
        PCPU_SAVE(CAUSE)
        PCPU_SAVE(EPC)
        PCPU_SAVE(BADVADDR)

        # If the exception is a TLB load/store exception (exc. code 2 or 3),
        # check for kernel stack overflow.
        mfc0 k1, C0_CAUSE
        # Extract exc. code, discarding least significant bit.
        ext k1, k1, CR_X_SHIFT+1, CR_X_BITS
        subu k1, 1
        # k1 is 0 now iff exc. code was 2 or 3.
        bnez k1, skip_overflow_check
        nop
        # Check if badvaddr is in the page preceding the kernel stack
        lw k1, PCPU_CURTHREAD(k0)
        lw k1, (TD_KSTACK+STK_BASE)(k1)
        srl k1, PAGE_SHIFT
        subu k1, 1
        mfc0 k0, C0_BADVADDR
        srl k0, PAGE_SHIFT
        bne k0, k1, skip_overflow_check
        nop
        # Kernel stack overflow detected: switch to panic stack
        LOAD_PCPU_KSEG0(k0)
        lw sp, PCPU_PANIC_SP(k0)
skip_overflow_check:
        # Allocate stack frame (of CTX_SIZE bytes) and save context.
        subu    sp, CTX_SIZE # required by gdb to recognize frame
        .cfi_def_cfa sp, 0

        # We cannot access stack (KSEG2) when exception level is enabled
        # Let's drop exception level and disable interrupts.
        STATUS_CLEAR(SR_EXL|SR_IE)

        # Save context on the kernel stack
        SAVE_CPU_CTX(sp)

        # Copy sp, status, cause, epc, badvaddr from PCPU to stack frame
        LOAD_PCPU_KSEG0(t0)
        PCPU_TO_FRAME(SP)
        .cfi_rel_offset sp, CTX_SP
        PCPU_TO_FRAME(SR)
        PCPU_TO_FRAME(CAUSE)
        PCPU_TO_FRAME(PC)
        .cfi_return_column t1
        .cfi_rel_offset t1, CTX_PC
        PCPU_TO_FRAME(BADVADDR)

        # Load kernel global pointer.
        la      gp, _gp

        # Fetch thread control block pointer to s0 for later use.
        LOAD_PCPU(t0)
        lw      s0, PCPU_CURTHREAD(t0)

        # Save exception frame pointer into td_kframe.
        sw      sp, TD_KFRAME(s0)

        # Call C interrupt handler routine.
        la      t0, mips_exc_handler
        jalr    t0
        move    a0, sp                  # (delay) 1st arg

kern_exc_leave:
        DISABLE_INT(t1)

        LOAD_PCPU_KSEG0(k0)

        # Load status register from exception frame, update it with current
        # interrupt mask and exception mode, and store it within PCPU
        LOAD_REG(t0, SR, sp)
        ins     t0, t1, SR_IMASK_SHIFT, SR_IMASK_BITS
        ori     t0, SR_EXL
        sw      t0, PCPU_SR(k0)

        # Copy pc register from exception frame to PCPU
        LOAD_REG(t0, PC, sp)
        sw      t0, PCPU_PC(k0)

        # Load context from exception frame on stack, sp will get overwritten.
        LOAD_CPU_CTX()

        # Restore status and epc registers from PCPU
        LOAD_PCPU_KSEG0(k0)
        PCPU_RESTORE(STATUS)
        PCPU_RESTORE(EPC)

        sync
        eret
END(kern_exc_enter)

# vim: sw=8 ts=8 et
