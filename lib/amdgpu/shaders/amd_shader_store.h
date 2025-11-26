/*
 * Copyright (C) 2021 Advanced Micro Devices, Inc. All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#ifndef _AMD_SHADER_STORE_H_
#define _AMD_SHADER_STORE_H_

/**
 * Macros
 */

#define SHADER_START ".text\n"

/* Macros for portable v_add_co_u32, v_add_co_ci_u32,
 * and v_cmp_lt_u32.
 */
#define SHADER_MACROS_U32 \
    "   .text\n"\
    "   .macro V_ADD_CO_U32 vdst, src0, vsrc1\n"\
    "       .if (.amdgcn.gfx_generation_number >= 10)\n"\
    "           v_add_co_u32        \\vdst, vcc_lo, \\src0, \\vsrc1\n"\
    "       .elseif (.amdgcn.gfx_generation_number >= 9)\n"\
    "           v_add_co_u32        \\vdst, vcc, \\src0, \\vsrc1\n"\
    "       .else\n"\
    "           v_add_u32           \\vdst, vcc, \\src0, \\vsrc1\n"\
    "       .endif\n"\
    "   .endm\n"\
    "   .macro V_ADD_CO_CI_U32 vdst, src0, vsrc1\n"\
    "       .if (.amdgcn.gfx_generation_number >= 10)\n"\
    "           v_add_co_ci_u32     \\vdst, vcc_lo, \\src0, \\vsrc1, vcc_lo\n"\
    "       .elseif (.amdgcn.gfx_generation_number >= 9)\n"\
    "           v_addc_co_u32       \\vdst, vcc, \\src0, \\vsrc1, vcc\n"\
    "       .else\n"\
    "           v_addc_u32          \\vdst, vcc, \\src0, \\vsrc1, vcc\n"\
    "       .endif\n"\
    "   .endm\n"\
    "   .macro V_CMP_LT_U32 src0, vsrc1\n"\
    "       .if (.amdgcn.gfx_generation_number >= 10)\n"\
    "           v_cmp_lt_u32        vcc_lo, \\src0, \\vsrc1\n"\
    "       .else\n"\
    "           v_cmp_lt_u32        vcc, \\src0, \\vsrc1\n"\
    "       .endif\n"\
    "   .endm\n"\
    "   .macro V_CMP_EQ_U32 src0, vsrc1\n"\
    "       .if (.amdgcn.gfx_generation_number >= 10)\n"\
    "           v_cmp_eq_u32        vcc_lo, \\src0, \\vsrc1\n"\
    "       .else\n"\
    "           v_cmp_eq_u32        vcc, \\src0, \\vsrc1\n"\
    "       .endif\n"\
    "   .endm\n"

/* Macros for portable flat load/store/atomic instructions.
 *
 * gc943 (gfx94x) deprecates glc/slc in favour of nt/sc1/sc0.
 * The below macros when used will always use the nt sc1 sc0
 * modifiers for gfx94x, but also take in arg0 arg1 to specify
 * (for non-gfx94x): glc, slc, or glc slc.
 */
#define SHADER_MACROS_FLAT \
    "   .macro FLAT_LOAD_DWORD_NSS vdst, vaddr arg0 arg1\n"\
    "       .if (.amdgcn.gfx_generation_number == 9 && .amdgcn.gfx_generation_minor >= 4)\n"\
    "           flat_load_dword \\vdst, \\vaddr nt sc1 sc0\n"\
    "       .else\n"\
    "           flat_load_dword \\vdst, \\vaddr \\arg0 \\arg1\n"\
    "       .endif\n"\
    "   .endm\n"\
    "   .macro FLAT_LOAD_DWORDX2_NSS vdst, vaddr arg0 arg1\n"\
    "       .if (.amdgcn.gfx_generation_number == 9 && .amdgcn.gfx_generation_minor >= 4)\n"\
    "           flat_load_dwordx2 \\vdst, \\vaddr nt sc1 sc0\n"\
    "       .else\n"\
    "           flat_load_dwordx2 \\vdst, \\vaddr \\arg0 \\arg1\n"\
    "       .endif\n"\
    "   .endm\n"\
    "   .macro FLAT_STORE_DWORD_NSS vaddr, vsrc arg0 arg1\n"\
    "       .if (.amdgcn.gfx_generation_number == 9 && .amdgcn.gfx_generation_minor >= 4)\n"\
    "           flat_store_dword \\vaddr, \\vsrc nt sc1 sc0\n"\
    "       .else\n"\
    "           flat_store_dword \\vaddr, \\vsrc \\arg0 \\arg1\n"\
    "       .endif\n"\
    "   .endm\n"\
    "   .macro FLAT_ATOMIC_ADD_NSS vdst, vaddr, vsrc arg0 arg1\n"\
    "       .if (.amdgcn.gfx_generation_number == 9 && .amdgcn.gfx_generation_minor >= 4)\n"\
    "           flat_atomic_add \\vdst, \\vaddr, \\vsrc nt sc1 sc0\n"\
    "       .else\n"\
    "           flat_atomic_add \\vdst, \\vaddr, \\vsrc \\arg0 \\arg1\n"\
    "       .endif\n"\
    "   .endm\n"

/**
 * Common
 */

/* Shader: NoopIsa
 * Purpose: Minimal shader that immediately terminates.
 * Inputs: (none)
 * Side Effects: None.
 * Pseudo-code:
 *   void shader_noop(void) {
 *       // Immediately end program
 *       return; // s_endpgm
 *   }
 */
static const char NoopIsa[] =
    SHADER_START
    R"(
        s_endpgm
)";
/* Shader: CopyDwordIsa
 * Purpose: Load one 32-bit value from address in s[0:1] and store to address in s[2:3].
 * Inputs:
 *   s[0:1] - source pointer
 *   s[2:3] - destination pointer
 * Memory Semantics:
 *   Uses flat (global) load/store, optionally with system scope (gfx12+) or glc/slc flags.
 * Pseudo-code:
 *   void shader_copy_dword(uint32_t *dst, const uint32_t *src) {
 *       uint32_t val = *src; // flat load
 *       *dst = val;          // flat store
 *   }
 */
static const char CopyDwordIsa[] =
    SHADER_START
    SHADER_MACROS_FLAT
    R"(
        v_mov_b32 v0, s0
        v_mov_b32 v1, s1
        v_mov_b32 v2, s2
        v_mov_b32 v3, s3
        .if (.amdgcn.gfx_generation_number >= 12)
            FLAT_LOAD_DWORD_NSS v4, v[0:1] scope:SCOPE_SYS
            s_wait_loadcnt 0
            FLAT_STORE_DWORD_NSS v[2:3], v4 scope:SCOPE_SYS
        .else
            FLAT_LOAD_DWORD_NSS v4, v[0:1] glc slc
            s_waitcnt 0
            FLAT_STORE_DWORD_NSS v[2:3], v4 glc slc
        .endif
        s_endpgm
)";

/* Shader: InfiniteLoopIsa
 * Purpose: Busy loop forever (used for hang / timeout tests).
 * Pseudo-code:
 *   void shader_infinite_loop(void) {
 *       for (;;) {
 *           // delay (s_nop)
 *       }
 *   }
 */
static const char InfiniteLoopIsa[] =
    SHADER_START
    R"(
        .text
        LOOP:
        s_nop 0x10
        s_branch LOOP
        s_endpgm
)";

/* Shader: AtomicIncIsa
 * Purpose: Atomically increment a 32-bit counter at address s[0:1]. Returns old value (ignored).
 * Inputs: s[0:1] - pointer to uint32_t counter.
 * Memory Semantics: Uses atomic add (system scope on gfx12). Ensures atomicity across waves.
 * Pseudo-code:
 *   void shader_atomic_inc(uint32_t *counter) {
 *       atomic_fetch_add(counter, 1); // old value unused
 *   }
 */
static const char AtomicIncIsa[] =
    SHADER_START
    SHADER_MACROS_FLAT
    R"(
        v_mov_b32 v0, s0
        v_mov_b32 v1, s1
        .if (.amdgcn.gfx_generation_number >= 12)
            v_mov_b32 v2, 1
            FLAT_ATOMIC_ADD_NSS v3, v[0:1], v2 scope:SCOPE_SYS th:TH_ATOMIC_RETURN
        .elseif (.amdgcn.gfx_generation_number >= 8)
            v_mov_b32 v2, 1
            FLAT_ATOMIC_ADD_NSS v3, v[0:1], v2 glc slc
        .else
            v_mov_b32 v2, -1
            flat_atomic_inc v3, v[0:1], v2 glc slc
        .endif
        s_endpgm
)";

/**
 * KFDMemoryTest
 */

/* Shader: ScratchCopyDwordIsa
 * Purpose: Configure scratch base registers then copy one dword from s[0:1] to s[2:3].
 * Inputs:
 *   s[0:1] - source ptr
 *   s[2:3] - destination ptr
 *   s[4:5] - scratch base address (architecture-dependent setup)
 * Scratch Setup: Programs flat scratch base depending on generation.
 * Pseudo-code:
 *   void shader_scratch_copy(uint32_t *dst, const uint32_t *src, uint64_t scratch_base) {
 *       program_scratch_base(scratch_base);
 *       *dst = *src;
 *   }
 */
static const char ScratchCopyDwordIsa[] =
    SHADER_START
    SHADER_MACROS_FLAT
    R"(
        // Copy the parameters from scalar registers to vector registers
        .if (.amdgcn.gfx_generation_number >= 9)
            v_mov_b32 v0, s0
            v_mov_b32 v1, s1
            v_mov_b32 v2, s2
            v_mov_b32 v3, s3
        .else
            v_mov_b32_e32 v0, s0
            v_mov_b32_e32 v1, s1
            v_mov_b32_e32 v2, s2
            v_mov_b32_e32 v3, s3
        .endif

        // Setup the scratch parameters. This assumes a single 16-reg block
        .if (.amdgcn.gfx_generation_number >= 12)
            s_setreg_b32 hwreg(HW_REG_SCRATCH_BASE_LO), s4
            s_setreg_b32 hwreg(HW_REG_SCRATCH_BASE_HI), s5
        .elseif (.amdgcn.gfx_generation_number >= 10)
            s_setreg_b32 hwreg(HW_REG_FLAT_SCR_LO), s4
            s_setreg_b32 hwreg(HW_REG_FLAT_SCR_HI), s5
        .elseif (.amdgcn.gfx_generation_number == 9)
            s_mov_b32 flat_scratch_lo, s4
            s_mov_b32 flat_scratch_hi, s5
        .else
            s_mov_b32 flat_scratch_lo, 8
            s_mov_b32 flat_scratch_hi, 0
        .endif

        // Copy a dword between the passed addresses
        .if (.amdgcn.gfx_generation_number >= 12)
            FLAT_LOAD_DWORD_NSS v4, v[0:1] scope:SCOPE_SYS
            s_wait_loadcnt 0
            FLAT_STORE_DWORD_NSS v[2:3], v4 scope:SCOPE_SYS
        .else
            FLAT_LOAD_DWORD_NSS v4, v[0:1] slc
            s_waitcnt vmcnt(0) & lgkmcnt(0)
            FLAT_STORE_DWORD_NSS v[2:3], v4 slc
        .endif

        s_endpgm
)";

/* Shader: PollMemoryIsa
 * Purpose: Poll *src until it equals 0x5678, then write 0x5678 to *dst.
 * Inputs:
 *   s[0:1] - src address (flag written by host)
 *   s[2:3] - dst address where result 0x5678 stored
 * Behavior:
 *   Loop: load flag; compare; if not matched continue; else store value and exit.
 * Pseudo-code:
 *   void shader_poll_and_write(uint32_t *dst, const uint32_t *src) {
 *       while (*src != 0x5678) { ; } // busy wait
 *       *dst = 0x5678;
 *   }
 */
static const char PollMemoryIsa[] =
    SHADER_START
    R"(
        // Assume src address in s0, s1, and dst address in s2, s3
        s_movk_i32 s18, 0x5678
        .if (.amdgcn.gfx_generation_number >= 10)
            v_mov_b32 v0, s2
            v_mov_b32 v1, s3
            v_mov_b32 v2, 0x5678
        .endif
        LOOP:
        .if (.amdgcn.gfx_generation_number >= 12)
            s_load_dword s16, s[0:1], 0x0 scope:SCOPE_SYS
        .else
            s_load_dword s16, s[0:1], 0x0 glc
        .endif
        s_cmp_eq_i32 s16, s18
        s_cbranch_scc0   LOOP
        .if (.amdgcn.gfx_generation_number >= 12)
            flat_store_dword v[0:1], v2 scope:SCOPE_SYS
        .elseif (.amdgcn.gfx_generation_number >= 10)
            flat_store_dword v[0:1], v2 slc
        .else
            s_store_dword s18, s[2:3], 0x0 glc
        .endif
        s_endpgm
)";

/* Shader: PollNCMemoryIsa
 * Purpose: Poll a potentially non-coherent memory location (via vector load with scc) until value == 0x5678; then store 0x5678 to dst.
 * Rationale: Scalar memory path may not ensure coherence; uses vector load with scc modifier.
 * Inputs: s[0:1] src flag, s[2:3] dst address (written after success).
 * Pseudo-code:
 *   void shader_poll_nc(uint32_t *dst, const uint32_t *src) {
 *       while (vector_load(src) != 0x5678) ;
 *       *dst = 0x5678; // store with scc path
 *   }
 */
static const char PollNCMemoryIsa[] =
    SHADER_START
    R"(
        // Assume src address in s0, s1, and dst address in s2, s3
        v_mov_b32 v6, 0x5678
        v_mov_b32 v0, s0
        v_mov_b32 v1, s1
        LOOP:
        flat_load_dword v4, v[0:1] scc
        v_cmp_eq_u32 vcc, v4, v6
        s_cbranch_vccz   LOOP
        v_mov_b32 v0, s2
        v_mov_b32 v1, s3
        flat_store_dword v[0:1], v6 scc
        s_endpgm
)";

/* Shader: CopyOnSignalIsa
 * Inputs (buffer at s[0:1], size >= 3 dwords):
 *   DW0 flag: host writes 0xcafe to trigger copy
 *   DW1 value: source value
 *   DW2 output: destination written when flag observed
 * Behavior:
 *   Poll flag until equals 0xcafe then copy DW1 -> DW2.
 * Pseudo-code:
 *   void shader_copy_on_signal(uint32_t *buf) {
 *       while (buf[0] != 0xcafe) { ; }
 *       buf[2] = buf[1];
 *   }
 */
static const char CopyOnSignalIsa[] =
    SHADER_START
    R"(
        // Assume input buffer in s0, s1
        .if (.amdgcn.gfx_generation_number >= 10)
            s_add_u32 s2, s0, 0x8
            s_addc_u32 s3, s1, 0x0
            s_mov_b32 s18, 0xcafe
            v_mov_b32 v0, s0
            v_mov_b32 v1, s1
            v_mov_b32 v4, s2
            v_mov_b32 v5, s3
        .else
            s_mov_b32 s18, 0xcafe
        .endif

        .if (.amdgcn.gfx_generation_number >= 12)

            POLLSIGNAL:
            s_load_dword s16, s[0:1], 0x0 scope:SCOPE_SYS
            s_cmp_eq_i32 s16, s18
            s_cbranch_scc0   POLLSIGNAL

            s_load_dword s17, s[0:1], 0x4 scope:SCOPE_SYS
            s_wait_kmcnt 0

            v_mov_b32 v2, s17
            flat_store_dword v[4:5], v2 scope:SCOPE_SYS
        .else

            POLLSIGNAL:
            s_load_dword s16, s[0:1], 0x0 glc
            s_cmp_eq_i32 s16, s18
            s_cbranch_scc0   POLLSIGNAL

            s_load_dword s17, s[0:1], 0x4 glc
            s_waitcnt vmcnt(0) & lgkmcnt(0)
            .if (.amdgcn.gfx_generation_number >= 10)
                v_mov_b32 v2, s17
                flat_store_dword v[4:5], v2 glc
            .else
                s_store_dword s17, s[0:1], 0x8 glc
            .endif
            s_waitcnt vmcnt(0) & lgkmcnt(0)

        .endif
        s_endpgm
)";

/* Shader: PollAndCopyIsa
 * Purpose: Wait for flag at s[0:1] (DW0) to become 1; then copy DW1 (offset +4) into destination s[2:3].
 * Architecture: GFX9 variants (Aldebaran / Aqua Vanjaram) have cache invalidation & write-back sequence.
 * Flow:
 *   spin on flag -> invalidate buffer -> load payload -> store to dst -> flush.
 * Pseudo-code:
 *   void shader_poll_and_copy(uint32_t *src, uint32_t *dst) {
 *       while (src[0] != 1) ;
 *       uint32_t val = src[1];
 *       *dst = val;
 *   }
 */
static const char PollAndCopyIsa[] =
    SHADER_START
    SHADER_MACROS_FLAT
    R"(
        // Assume src buffer in s[0:1] and dst buffer in s[2:3]
        // Path for Aldebaran, Aqua Vanjaram
        .if (.amdgcn.gfx_generation_number == 9 && (.amdgcn.gfx_generation_minor >= 4 || .amdgcn.gfx_generation_stepping == 10))
            v_mov_b32 v0, s0
            v_mov_b32 v1, s1
            v_mov_b32 v18, 0x1
            LOOP0:
            FLAT_LOAD_DWORD_NSS v16, v[0:1] glc
            s_waitcnt vmcnt(0) & lgkmcnt(0)
            v_cmp_eq_i32 vcc, v16, v18
            s_cbranch_vccz LOOP0
            .if (.amdgcn.gfx_generation_minor >= 4)
                buffer_inv sc1 sc0
            .else
                buffer_invl2
            .endif
            s_load_dword s17, s[0:1], 0x4 glc
            s_waitcnt vmcnt(0) & lgkmcnt(0)
            s_store_dword s17, s[2:3], 0x0 glc
            s_waitcnt vmcnt(0) & lgkmcnt(0)
            buffer_wbl2
        .elseif (.amdgcn.gfx_generation_number == 9)
            s_movk_i32 s18, 0x1
            LOOP1:
            s_load_dword s16, s[0:1], 0x0 glc
            s_cmp_eq_i32 s16, s18
            s_cbranch_scc0 LOOP1
            s_load_dword s17, s[0:1], 0x4 glc
            s_waitcnt vmcnt(0) & lgkmcnt(0)
            s_store_dword s17, s[2:3], 0x0 glc
        .endif
        s_waitcnt vmcnt(0) & lgkmcnt(0)
        s_endpgm
)";

/* Shader: WriteFlagAndValueIsa
 * Purpose: Write value from second buffer (Input1 DW0) into first buffer DW1, then set first buffer DW0 to 1.
 * Inputs:
 *   s[0:1] - buffer A (flag + output value)
 *   s[2:3] - buffer B (contains value to copy at DW0)
 * Sequence (gfx9.4+/stepping10): load value -> store to A[1] -> flush -> store flag A[0]=1.
 * Pseudo-code:
 *   void shader_write_flag_value(uint32_t *bufA, uint32_t *bufB) {
 *       uint32_t val = bufB[0];
 *       bufA[1] = val; // ensure visibility
 *       wb();
 *       bufA[0] = 1;   // signal
 *   }
 */
static const char WriteFlagAndValueIsa[] =
    SHADER_START
    SHADER_MACROS_FLAT
    R"(
        // Assume two inputs buffer in s[0:1] and s[2:3]
        .if (.amdgcn.gfx_generation_number == 9 && (.amdgcn.gfx_generation_minor >= 4 || .amdgcn.gfx_generation_stepping == 10))
            v_mov_b32 v0, s0
            v_mov_b32 v1, s1
            s_load_dword s18, s[2:3], 0x0 glc
            s_waitcnt vmcnt(0) & lgkmcnt(0)
            s_store_dword s18, s[0:1], 0x4 glc
            s_waitcnt vmcnt(0) & lgkmcnt(0)
            buffer_wbl2
            s_waitcnt vmcnt(0) & lgkmcnt(0)
            v_mov_b32 v16, 0x1
            FLAT_STORE_DWORD_NSS v[0:1], v16 glc
        .endif
        s_endpgm
)";

/* Shader: WriteAndSignalIsa
 * Purpose: Fill DW1 with 0xbeef, write MMIO signal (0x1) at s[2:3], then flag DW0 with 0xcafe.
 * Inputs:
 *   s[0:1] - buffer (DW0 flag, DW1 payload)
 *   s[2:3] - mmio base (doorbell / signal area)
 * Flow: write payload -> write mmio signal -> write flag.
 * Pseudo-code:
 *   void shader_write_and_signal(uint32_t *buf, uint32_t *mmio) {
 *       buf[1] = 0xbeef;
 *       *mmio = 1; // doorbell
 *       buf[0] = 0xcafe; // completion flag
 *   }
 */
static const char WriteAndSignalIsa[] =
    SHADER_START
    R"(
        // Assume input buffer in s0, s1
        .if (.amdgcn.gfx_generation_number >= 10)
            s_add_u32 s4, s0, 0x4
            s_addc_u32 s5, s1, 0x0
            v_mov_b32 v0, s0
            v_mov_b32 v1, s1
            v_mov_b32 v2, s2
            v_mov_b32 v3, s3
            v_mov_b32 v4, s4
            v_mov_b32 v5, s5
            .if (.amdgcn.gfx_generation_number >= 12)
                v_mov_b32 v18, 0xbeef
                flat_store_dword v[4:5], v18 scope:SCOPE_SYS
                v_mov_b32 v18, 0x1
                flat_store_dword v[2:3], v18 scope:SCOPE_SYS
                v_mov_b32 v18, 0xcafe
                flat_store_dword v[0:1], v18 scope:SCOPE_SYS
            .else
                v_mov_b32 v18, 0xbeef
                flat_store_dword v[4:5], v18 glc
                v_mov_b32 v18, 0x1
                flat_store_dword v[2:3], v18 glc
                v_mov_b32 v18, 0xcafe
                flat_store_dword v[0:1], v18 glc
            .endif
        .else
            s_mov_b32 s18, 0xbeef
            s_store_dword s18, s[0:1], 0x4 glc
            s_mov_b32 s18, 0x1
            s_store_dword s18, s[2:3], 0 glc
            s_mov_b32 s18, 0xcafe
            s_store_dword s18, s[0:1], 0x0 glc
        .endif
        s_endpgm
)";

/* Shader: FlushBufferForAcquireReleaseIsa
 * Purpose: Populate five cache lines (offsets 0x40..0x140) with 0x77 and a flag at 0x0 (0) to prime cache state.
 * Inputs: s[0:1] - shared buffer (>= 64*6 bytes).
 * Use Case: Prepares data prior to an acquire/release synchronization test.
 * Pseudo-code:
 *   void shader_flush_prepare(uint32_t *buf) {
 *       buf[0] = 0; // flag remains unset
 *       for (int i=0;i<5;i++) buf[(0x40>>2) + i*(0x40>>2)] = 0x77;
 *   }
 */
static const char FlushBufferForAcquireReleaseIsa[] =
    SHADER_START
    R"(
        .if (.amdgcn.gfx_generation_number == 9 && .amdgcn.gfx_generation_minor >= 4)
            s_mov_b32 s11, 0x77
            s_mov_b32 s12, 0x0
            // Store some data on 5 different cache lines
            s_store_dword s12, s[0:1], 0x0 glc
            s_store_dword s11, s[0:1], 0x40 glc
            s_store_dword s11, s[0:1], 0x80 glc
            s_store_dword s11, s[0:1], 0xc0 glc
            s_store_dword s11, s[0:1], 0x100 glc
            s_store_dword s11, s[0:1], 0x140 glc
            s_waitcnt lgkmcnt(0)
        .endif
        s_endpgm
)";

/* Shader: WriteReleaseVectorIsa
 * Purpose: Write values {1..5} to five distinct cache-line offsets; perform write-release (wb + L2 flush) then set flag at 0x0.
 * Inputs: s[0:1] - shared buffer with acquiring shader.
 * Memory Ordering: buffer_wbl2 before flag ensures data visible prior to flag store.
 * Pseudo-code:
 *   void shader_write_release_vec(uint32_t *shared) {
 *       shared[0x40/4]=1; shared[0x80/4]=2; shared[0xC0/4]=3; shared[0x100/4]=4; shared[0x140/4]=5;
 *       write_back(); // release
 *       shared[0] = 1; // flag
 *   }
 */
static const char WriteReleaseVectorIsa[] =
    SHADER_START
    R"(
        .if (.amdgcn.gfx_generation_number == 9 && .amdgcn.gfx_generation_minor >= 4)
            v_mov_b32 v11, 0x1
            v_mov_b32 v12, 0x2
            v_mov_b32 v13, 0x3
            v_mov_b32 v14, 0x4
            v_mov_b32 v15, 0x5
            v_mov_b32 v21, 0x40
            v_mov_b32 v22, 0x80
            v_mov_b32 v23, 0xc0
            v_mov_b32 v24, 0x100
            v_mov_b32 v25, 0x140
            // Store some data on 5 different cache lines
            global_store_dword v21, v11, s[0:1]
            global_store_dword v22, v12, s[0:1]
            global_store_dword v23, v13, s[0:1]
            global_store_dword v24, v14, s[0:1]
            global_store_dword v25, v15, s[0:1] nt sc1 sc0
            s_waitcnt vmcnt(0)
            // Write-Release
            s_mov_b32 s16, 0x1
            buffer_wbl2 sc1 sc0
            s_waitcnt vmcnt(0) & lgkmcnt(0)
            s_store_dword s16, s[0:1], 0x0 glc
        .endif
        s_endpgm
)";

/* Shader: WriteReleaseScalarIsa
 * Purpose: Same as vector variant but uses scalar stores and scalar cache write-back.
 * Values {6..10} to offsets 0x40..0x140 then release + set flag.
 * Pseudo-code:
 *   void shader_write_release_scalar(uint32_t *shared) {
 *       uint32_t vals[5]={6,7,8,9,10};
 *       for(int i=0;i<5;i++) shared[(0x40>>2)+i*(0x40>>2)] = vals[i];
 *       scalar_dcache_wb(); write_back();
 *       shared[0] = 1;
 *   }
 */
static const char WriteReleaseScalarIsa[] =
    SHADER_START
    R"(
        .if (.amdgcn.gfx_generation_number == 9 && .amdgcn.gfx_generation_minor >= 4)
            s_mov_b32 s11, 0x6
            s_mov_b32 s12, 0x7
            s_mov_b32 s13, 0x8
            s_mov_b32 s14, 0x9
            s_mov_b32 s15, 0xa
            // Store some data on 5 different cache lines
            s_store_dword s11, s[0:1], 0x40
            s_store_dword s12, s[0:1], 0x80
            s_store_dword s13, s[0:1], 0xc0
            s_store_dword s14, s[0:1], 0x100
            s_store_dword s15, s[0:1], 0x140 glc
            s_waitcnt lgkmcnt(0)
            // Write-Release
            s_dcache_wb // WB Scalar L1 cache
            s_mov_b32 s16, 0x1
            buffer_wbl2 sc1 sc0
            s_waitcnt vmcnt(0) & lgkmcnt(0)
            s_store_dword s16, s[0:1], 0x0 glc
            s_waitcnt lgkmcnt(0)
        .endif
        s_endpgm
)";

/* Shader: ReadAcquireVectorIsa
 * Purpose: Acquire side of release/acquire. Waits for flag==1 then invalidates caches, loads five values and writes them to output buffer.
 * Inputs:
 *   s[0:1] shared buffer (writer performed release)
 *   s[2:3] output buffer (CPU-visible)
 * Ordering: buffer_inv before loads ensures fresh data; stores use non-temporal sc1 sc0 modifiers.
 * Pseudo-code:
 *   void shader_read_acquire_vec(uint32_t *shared, uint32_t *out) {
 *       while(shared[0]!=1); invalidate();
 *       for(int i=0;i<5;i++) {
 *           unsigned idx = (0x40>>2)+i*(0x40>>2);
 *           out[idx] = shared[idx];
 *       }
 *   }
 */
static const char ReadAcquireVectorIsa[] =
    SHADER_START
    R"(
        .if (.amdgcn.gfx_generation_number == 9 && .amdgcn.gfx_generation_minor >= 4)
            // Read-Acquire
            s_mov_b32 s18, 0x1
            LOOP:
            s_load_dword s17, s[0:1], 0x0 glc
            s_waitcnt lgkmcnt(0)
            s_cmp_eq_i32 s17, s18
            s_cbranch_scc0 LOOP
            buffer_inv sc1 sc0
            // Load data
            v_mov_b32 v21, 0x40
            v_mov_b32 v22, 0x80
            v_mov_b32 v23, 0xc0
            v_mov_b32 v24, 0x100
            v_mov_b32 v25, 0x140
            global_load_dword v11, v21, s[0:1]
            global_load_dword v12, v22, s[0:1]
            global_load_dword v13, v23, s[0:1]
            global_load_dword v14, v24, s[0:1]
            global_load_dword v15, v25, s[0:1]
            s_waitcnt vmcnt(0)
            // Store data for output
            v_mov_b32 v21, 0x40
            v_mov_b32 v22, 0x80
            v_mov_b32 v23, 0xc0
            v_mov_b32 v24, 0x100
            v_mov_b32 v25, 0x140
            global_store_dword v21, v11, s[2:3] nt sc1 sc0
            global_store_dword v22, v12, s[2:3] nt sc1 sc0
            global_store_dword v23, v13, s[2:3] nt sc1 sc0
            global_store_dword v24, v14, s[2:3] nt sc1 sc0
            global_store_dword v25, v15, s[2:3] nt sc1 sc0
            s_waitcnt vmcnt(0)
        .endif
        s_endpgm
)";

/* Shader: ReadAcquireScalarIsa
 * Purpose: Scalar-load variant of acquire side. Polls flag then invalidates and reads via scalar path, writing results to output buffer.
 * Pseudo-code:
 *   void shader_read_acquire_scalar(uint32_t *shared, uint32_t *out) {
 *       while(shared[0]!=1); invalidate();
 *       for(int i=0;i<5;i++) {
 *           unsigned idx = (0x40>>2)+i*(0x40>>2);
 *           out[idx] = shared[idx];
 *       }
 *   }
 */
static const char ReadAcquireScalarIsa[] =
    SHADER_START
    R"(
        .if (.amdgcn.gfx_generation_number == 9 && .amdgcn.gfx_generation_minor >= 4)
            // Read-Acquire
            s_mov_b32 s18, 0x1
            LOOP:
            s_load_dword s17, s[0:1], 0x0 glc
            s_waitcnt lgkmcnt(0)
            s_cmp_eq_i32 s17, s18
            s_cbranch_scc0 LOOP
            buffer_inv sc1 sc0
            // Load data
            s_load_dword s21, s[0:1], 0x40
            s_load_dword s22, s[0:1], 0x80
            s_load_dword s23, s[0:1], 0xc0
            s_load_dword s24, s[0:1], 0x100
            s_load_dword s25, s[0:1], 0x140
            s_waitcnt lgkmcnt(0)
            // Store data for output
            s_store_dword s21, s[2:3], 0x40 glc
            s_store_dword s22, s[2:3], 0x80 glc
            s_store_dword s23, s[2:3], 0xc0 glc
            s_store_dword s24, s[2:3], 0x100 glc
            s_store_dword s25, s[2:3], 0x140 glc
            s_waitcnt lgkmcnt(0)
        .endif
        s_endpgm
)";

/**
 * KFDQMTest
 */

/* Shader: LoopIsa
 * Purpose: Stress math pipelines by performing iterative transcendental + arithmetic operations.
 * Inputs: s1 - iteration count. (Others setup constants.)
 * Registers: v0..v28 used for accumulating and intermediate math.
 * Pseudo-code (simplified approximation):
 *   void shader_dense_loop(uint32_t iterations) {
 *       float accA[4]={0}, accB[4]={0}, accC[4]={0}, accD[4]={0};
 *       for (uint32_t i=0;i<iterations;i++) {
 *           float f = (float)i;
 *           // Chain of log/exp/sqrt/rsq operations updating accumulators.
 *           // Real ISA interleaves for latency hiding.
 *           for(int k=0;k<4;k++) {
 *               float t = expf(logf(f + accA[k]) * scale(k));
 *               accB[k] += t;
 *               accC[k] += sqrtf(accA[k]);
 *               accD[k] += 1.0f/sqrtf(accA[k]+1.0f);
 *           }
 *       }
 *   }
 */
static const char LoopIsa[] =
    SHADER_START
    R"(
        s_movk_i32    s0, 0x0008
        s_movk_i32    s1, 0x00ff
        s_mov_b32     s4, 0
        s_mov_b32     s5, 0
        s_mov_b32     s6, 0
        s_mov_b32     s7, 0
        s_mov_b32     s12, 0
        s_mov_b32     s13, 0
        s_mov_b32     s14, 0
        s_mov_b32     s15, 0
        v_mov_b32     v0, 0
        v_mov_b32     v1, 0
        v_mov_b32     v2, 0
        v_mov_b32     v3, 0
        v_mov_b32     v4, 0
        v_mov_b32     v5, 0
        v_mov_b32     v6, 0
        v_mov_b32     v7, 0
        v_mov_b32     v8, 0
        v_mov_b32     v9, 0
        v_mov_b32     v10, 0
        v_mov_b32     v11, 0
        v_mov_b32     v12, 0
        v_mov_b32     v13, 0
        v_mov_b32     v14, 0
        v_mov_b32     v15, 0
        v_mov_b32     v16, 0
        LOOP:
        s_mov_b32     s8, s4
        s_mov_b32     s9, s1
        s_mov_b32     s10, s6
        s_mov_b32     s11, s7
        s_cmp_le_i32  s1, s0
        s_cbranch_scc1  END_OF_PGM
        v_add_f32     v0, 2.0, v0
        v_cvt_f32_i32 v17, s1
        .if (.amdgcn.gfx_generation_number >= 12)
            s_wait_dscnt     0
            s_wait_kmcnt     0
        .else
            s_waitcnt lgkmcnt(0)
        .endif
        v_add_f32     v18, s8, v17
        v_add_f32     v19, s9, v17
        v_add_f32     v20, s10, v17
        v_add_f32     v21, s11, v17
        v_add_f32     v22, s12, v17
        v_add_f32     v23, s13, v17
        v_add_f32     v24, s14, v17
        v_add_f32     v17, s15, v17
        v_log_f32     v25, v18
        v_mul_f32     v25, v22, v25
        v_exp_f32     v25, v25
        v_log_f32     v26, v19
        v_mul_f32     v26, v23, v26
        v_exp_f32     v26, v26
        v_log_f32     v27, v20
        v_mul_f32     v27, v24, v27
        v_exp_f32     v27, v27
        v_log_f32     v28, v21
        v_mul_f32     v28, v17, v28
        v_exp_f32     v28, v28
        v_add_f32     v5, v5, v25
        v_add_f32     v6, v6, v26
        v_add_f32     v7, v7, v27
        v_add_f32     v8, v8, v28
        v_mul_f32     v18, 0x3fb8aa3b, v18
        v_exp_f32     v18, v18
        v_mul_f32     v19, 0x3fb8aa3b, v19
        v_exp_f32     v19, v19
        v_mul_f32     v20, 0x3fb8aa3b, v20
        v_exp_f32     v20, v20
        v_mul_f32     v21, 0x3fb8aa3b, v21
        v_exp_f32     v21, v21
        v_add_f32     v9, v9, v18
        v_add_f32     v10, v10, v19
        v_add_f32     v11, v11, v20
        v_add_f32     v12, v12, v21
        v_sqrt_f32    v18, v22
        v_sqrt_f32    v19, v23
        v_sqrt_f32    v20, v24
        v_sqrt_f32    v21, v17
        v_add_f32     v13, v13, v18
        v_add_f32     v14, v14, v19
        v_add_f32     v15, v15, v20
        v_add_f32     v16, v16, v21
        v_rsq_f32     v18, v22
        v_rsq_f32     v19, v23
        v_rsq_f32     v20, v24
        v_rsq_f32     v17, v17
        v_add_f32     v1, v1, v18
        v_add_f32     v2, v2, v19
        v_add_f32     v3, v3, v20
        v_add_f32     v4, v4, v17
        s_add_u32     s0, s0, 1
        s_branch      LOOP
        END_OF_PGM:
        s_endpgm
)";


/**
 * KFDCWSRTest
 */

/* Shader: PersistentIterateIsa
 * Purpose: Persistently writes a stable value to per-workgroup output slot until host sets quit flag (0x12345678) in input buffer.
 * Inputs:
 *   s[0:1] input buffer (DW0 quit flag location)
 *   s[2:3] output buffer base
 *   s4/ttmp9 workgroup id
 * Behavior:
 *   Derive per-WG output address -> load initial value -> loop { store value; increment local counter; check quit flag }.
 * Pseudo-code:
 *   void shader_persistent(uint32_t *in, uint32_t *out_base, uint32_t wg_id) {
 *       uint32_t *slot = out_base + wg_id; uint32_t val = *slot; uint32_t ctr=0;
 *       while (in[0] != 0x12345678) { *slot = val; ctr++; }
 *   }
 */
static const char PersistentIterateIsa[] =
    SHADER_START
    SHADER_MACROS_U32
    SHADER_MACROS_FLAT
    R"(
        // Compute address of output buffer
        .if (.amdgcn.gfx_generation_number >= 12)
            v_mov_b32               v0, ttmp9   // use workgroup id as index
        .else
            v_mov_b32               v0, s4      // use workgroup id as index
        .endif
        v_lshlrev_b32           v0, 2, v0       // v0 *= 4
        V_ADD_CO_U32            v4, s2, v0      // v[4:5] = s[2:3] + v0 * 4
        v_mov_b32               v5, s3          // v[4:5] = s[2:3] + v0 * 4
        V_ADD_CO_CI_U32         v5, v5, 0       // v[4:5] = s[2:3] + v0 * 4

        // Store known-value output in register
        .if (.amdgcn.gfx_generation_number >= 12)
            FLAT_LOAD_DWORD_NSS     v6, v[4:5] scope:SCOPE_SYS
            s_wait_loadcnt 0                        // wait for memory reads to finish
        .else
            FLAT_LOAD_DWORD_NSS     v6, v[4:5] glc
            s_waitcnt vmcnt(0) & lgkmcnt(0)         // wait for memory reads to finish
        .endif

        // Initialize counter
        v_mov_b32               v7, 0

        LOOP:
        flat_store_dword        v[4:5], v6      // store known-val in output
        V_ADD_CO_U32            v7, 1, v7       // increment counter

        .if (.amdgcn.gfx_generation_number >= 12)
            s_load_dword            s6, s[0:1], 0 scope:SCOPE_SYS
            s_wait_loadcnt 0                        // wait for memory reads to finish
        .else
            s_load_dword            s6, s[0:1], 0 glc
            s_waitcnt vmcnt(0) & lgkmcnt(0)         // wait for memory reads to finish
        .endif
        s_cmp_eq_i32            s6, 0x12345678  // compare input buf to stopval
        s_cbranch_scc1          L_QUIT          // branch if notified to quit by host

        s_branch LOOP

        L_QUIT:
        s_endpgm
)";

/**
 * KFDEvictTest
 */

/* Shader: ReadMemoryIsa
 * Purpose: For each workgroup, repeatedly read pages of a local buffer until host signals completion (flag 0x5678), then write 0x5678 to result slot.
 * Inputs:
 *   s[0:1] address buffer: contains per-WG local buffer addresses (64-bit each)
 *   s[2:3] result buffer: per-WG result slot (DW0 local size MB initially; overwritten with 0x5678)
 *   s4/ttmp9 workgroup id
 * Flow:
 *   Compute output slot & pointer to local address; load local address & size; repeat: check quit flag; if not set, walk pages (4KB stride) reading; loop.
 * Pseudo-code:
 *   void shader_read_mem(uint64_t *addr_buf, uint32_t *res_buf, uint32_t wg_id) {
 *       uint32_t *res_slot = res_buf + wg_id; uint32_t size_mb = *res_slot;
 *       uint64_t local_addr = addr_buf[wg_id];
 *       while (*(uint32_t*)addr_buf != 0x5678) {
 *           for (uint64_t off=0; off < (uint64_t)size_mb*MB; off+=4096) {
 *               volatile uint64_t tmp = *(uint64_t*)(local_addr + off);
 *               (void)tmp;
 *           }
 *       }
 *       *res_slot = 0x5678;
 *   }
 */
static const char ReadMemoryIsa[] =
    SHADER_START
    SHADER_MACROS_U32
    SHADER_MACROS_FLAT
    R"(
        // Compute address of corresponding output buffer
        .if (.amdgcn.gfx_generation_number >= 12)
            v_mov_b32           v0, ttmp9       // use workgroup id as index
        .else
            v_mov_b32           v0, s4          // use workgroup id as index
        .endif
        v_lshlrev_b32           v0, 2, v0       // v0 *= 4
        V_ADD_CO_U32            v4, s2, v0      // v[4:5] = s[2:3] + v0 * 4
        v_mov_b32               v5, s3          // v[4:5] = s[2:3] + v0 * 4
        V_ADD_CO_CI_U32         v5, v5, 0       // v[4:5] = s[2:3] + v0 * 4

        // Compute input buffer offset used to store corresponding local buffer address
        v_lshlrev_b32           v0, 1, v0       // v0 *= 8
        V_ADD_CO_U32            v2, s0, v0      // v[2:3] = s[0:1] + v0 * 8
        v_mov_b32               v3, s1          // v[2:3] = s[0:1] + v0 * 8
        V_ADD_CO_CI_U32         v3, v3, 0       // v[2:3] = s[0:1] + v0 * 8

        // Load local buffer size from output buffer
        .if (.amdgcn.gfx_generation_number >= 12)
            FLAT_LOAD_DWORD_NSS     v11, v[4:5] scope:SCOPE_DEV
        .else
            FLAT_LOAD_DWORD_NSS     v11, v[4:5] slc
        .endif

        // Load 64bit local buffer address stored at v[2:3] to v[6:7]
        .if (.amdgcn.gfx_generation_number >= 12)
            FLAT_LOAD_DWORDX2_NSS   v[6:7], v[2:3] scope:SCOPE_DEV
            s_wait_loadcnt 0
        .else
            FLAT_LOAD_DWORDX2_NSS   v[6:7], v[2:3] slc
            s_waitcnt vmcnt(0) & lgkmcnt(0)         // wait for memory reads to finish
        .endif
        v_mov_b32               v8, 0x5678
        s_movk_i32              s8, 0x5678
        L_REPEAT:
        .if (.amdgcn.gfx_generation_number >= 12)
            s_load_dword        s16, s[0:1], 0x0 scope:SCOPE_SYS
            s_wait_kmcnt        0                      // wait for memory reads to finish
        .else
            s_load_dword        s16, s[0:1], 0x0 glc
            s_waitcnt           vmcnt(0) & lgkmcnt(0)  // wait for memory reads to finish
        .endif
        s_cmp_eq_i32            s16, s8
        s_cbranch_scc1          L_QUIT          // if notified to quit by host

        // Loop read local buffer starting at v[6:7]
        // every 4k page only read once
        v_mov_b32               v9, 0
        v_mov_b32               v10, 0x1000     // 4k page
        v_mov_b32               v12, v6
        v_mov_b32               v13, v7
        L_LOOP_READ:
        .if (.amdgcn.gfx_generation_number >= 12)
            FLAT_LOAD_DWORDX2_NSS   v[14:15], v[12:13] scope:SCOPE_DEV
        .else
            FLAT_LOAD_DWORDX2_NSS   v[14:15], v[12:13] slc
        .endif
        V_ADD_CO_U32            v9, v9, v10
        V_ADD_CO_U32            v12, v12, v10
        V_ADD_CO_CI_U32         v13, v13, 0
        V_CMP_LT_U32            v9, v11
        s_cbranch_vccnz         L_LOOP_READ
        s_branch                L_REPEAT
        L_QUIT:
        flat_store_dword        v[4:5], v8
        .if (.amdgcn.gfx_generation_number >= 12)
            s_wait_storecnt     0
        .else
            s_waitcnt vmcnt(0) & lgkmcnt(0)         // wait for memory writes to finish
        .endif
        s_endpgm
)";

/**
 * KFDGWSTest
 */

/* Shader: GwsInitIsa
 * Purpose: Initialize a GDS (Global Data Share) semaphore/counter via ds_gws_init.
 * Inputs: s[0:1] base address (may hold initial data).
 * Limited to pre-GFX12 path.
 * Pseudo-code:
 *   void shader_gws_init(uint32_t *addr) { gws_init(addr[0]); }
 */
static const char GwsInitIsa[] =
    SHADER_START
    R"(
        .if (.amdgcn.gfx_generation_number >= 12)
        .else
            s_mov_b32 m0, 0
            s_nop 0
            s_load_dword s16, s[0:1], 0x0 glc
            s_waitcnt 0
            v_mov_b32 v0, s16
            s_waitcnt 0
            ds_gws_init v0 offset:0 gds
            s_waitcnt 0
            s_endpgm
        .endif
)";

/* Shader: GwsAtomicIncreaseIsa
 * Purpose: Perform atomic increment protected by GWS semaphore across workgroups.
 * Inputs: s[0:1] pointer to shared counter.
 * Flow: acquire semaphore -> load -> increment -> store -> release.
 * Pseudo-code:
 *   void shader_gws_atomic_inc(uint32_t *counter) {
 *       gws_sema_p(); uint32_t v = *counter; v++; *counter = v; gws_sema_v();
 *   }
 */
static const char GwsAtomicIncreaseIsa[] =
    SHADER_START
    R"(
        // Assume src address in s0, s1
        .if (.amdgcn.gfx_generation_number >= 12)
        .elseif (.amdgcn.gfx_generation_number >= 10)
            s_mov_b32 m0, 0
            s_mov_b32 exec_lo, 0x1
            v_mov_b32 v0, s0
            v_mov_b32 v1, s1
            ds_gws_sema_p offset:0 gds
            s_waitcnt 0
            flat_load_dword v2, v[0:1] glc dlc
            s_waitcnt 0
            v_add_nc_u32 v2, v2, 1
            flat_store_dword v[0:1], v2
            s_waitcnt_vscnt null, 0
            ds_gws_sema_v offset:0 gds
        .else
            s_mov_b32 m0, 0
            s_nop 0
            ds_gws_sema_p offset:0 gds
            s_waitcnt 0
            s_load_dword s16, s[0:1], 0x0 glc
            s_waitcnt 0
            s_add_u32 s16, s16, 1
            s_store_dword s16, s[0:1], 0x0 glc
            s_waitcnt lgkmcnt(0)
            ds_gws_sema_v offset:0 gds
        .endif
        s_waitcnt 0
        s_endpgm
)";


/* Shader: CheckCuMaskIsa
 * Purpose: Record hardware ID register value per workgroup to validate CU masking distribution.
 * Inputs: s[2:3] output buffer base; s4/ttmp9 workgroup id.
 * Output: out[wg_id] = HW_ID (or HW_ID1 on GFX12) low 32 bits.
 * Pseudo-code:
 *   void shader_check_cumask(uint32_t *out, uint32_t wg_id) {
 *       uint32_t hwid = read_hw_id();
 *       out[wg_id] = hwid;
 *   }
 */
static const char CheckCuMaskIsa[] =
    SHADER_START
    SHADER_MACROS_U32
    SHADER_MACROS_FLAT
    R"(
        // Get workgroup id
        .if (.amdgcn.gfx_generation_number >= 12)
            v_mov_b32    v0, ttmp9
        .else
            v_mov_b32    v0, s4
        .endif

        // Address of output buffer element: v[4:5] = s[2:3] + v0 * 4
        v_lshlrev_b32    v6, 2, v0
        V_ADD_CO_U32     v4, s2, v6
        v_mov_b32        v5, s3
        V_ADD_CO_CI_U32  v5, v5, 0

        // Store HW_ID1 content
        .if (.amdgcn.gfx_generation_number >= 12)
            s_getreg_b32     s6, hwreg(HW_REG_HW_ID1)
        .else
            s_getreg_b32     s6, hwreg(HW_REG_HW_ID)
        .endif
        v_mov_b32        v1, s6
        flat_store_dword v[4:5], v1

        s_endpgm
)";


/* Shader: JumpToTrapIsa
 * Purpose: Trigger a trap (s_trap 1) then loop until a condition is externally satisfied (v4 changed), finally store v4.
 * Inputs: s[0:1] destination to store final value.
 * Trap Handling: External trap handler expected to modify wave state / v4.
 * Pseudo-code:
 *   void shader_jump_to_trap(uint32_t *out) {
 *       volatile uint32_t v4 = 0;
 *       trigger_trap(1);
 *       while (v4 == 0) { ; } // wait for handler
 *       *out = v4;
 *   }
 */
static const char JumpToTrapIsa[] =
    SHADER_START
    SHADER_MACROS_U32
    R"(
        /*copy the parameters from scalar registers to vector registers*/
        v_mov_b32 v4, 0
        v_mov_b32 v0, s0
        v_mov_b32 v1, s1
        s_trap 1
        EXIT_LOOP:
        V_CMP_EQ_U32 v4, 0
        s_cbranch_vccnz EXIT_LOOP
        flat_store_dword v[0:1], v4
        s_waitcnt vmcnt(0)&lgkmcnt(0)
        s_endpgm
)";

/* Shader: TrapHandlerIsa
 * Purpose: Handles traps (vm faults, address watch events, doorbell fetch) and restores wave state, advancing PC past trap instruction.
 * Behavior Summary:
 *   - Distinguish vmfault vs watch trap via trap status registers.
 *   - Optionally fetch doorbell index (MSG_RTN_GET_DOORBELL) on newer GPUs.
 *   - Send interrupt message, adjust m0, capture checkpoint values.
 *   - Restore IB status, STATUS registers, and resume (s_rfe_b64) at adjusted PC.
 * Pseudo-code (conceptual):
 *   void trap_handler(context *ctx) {
 *       flags = read_trap_status();
 *       if (is_vm_fault(flags)) { restore_and_exit(ctx); return; }
 *       if (is_watch_event(flags)) { ctx->watch_checkpoint = ctx->pc_delta; ctx->watch_status = flags; }
 *       else { ctx->doorbell = fetch_doorbell(); signal_interrupt(ctx); }
 *       advance_pc(ctx); restore_wave_state(ctx); resume(ctx);
 *   }
 */
static const char TrapHandlerIsa[] =
    SHADER_START
    R"(
        CHECK_VMFAULT:
        /*if trap jumped to by vmfault, restore skip m0 signalling*/
        .if (.amdgcn.gfx_generation_number < 12)
            s_getreg_b32 ttmp14, hwreg(HW_REG_TRAPSTS)
            s_and_b32 ttmp2, ttmp14, 0x800
        .else
            s_getreg_b32 ttmp14, hwreg(HW_REG_EXCP_FLAG_PRIV)
            s_and_b32 ttmp2, ttmp14, 0x10
        .endif
        s_cbranch_scc1 RESTORE_AND_EXIT
        /*check for address watch event and record pc check point delta*/
        .if (.amdgcn.gfx_generation_number < 12)
            s_and_b32 ttmp2, ttmp14, 0x7080
        .else
            s_and_b32 ttmp2, ttmp14, 0xf
        .endif
        s_cbranch_scc0 GET_DOORBELL
        v_mov_b32 v5, v4 // capture watch checkpoint
        v_mov_b32 v6, ttmp14 // capture watch trapsts
        s_branch RESTORE_AND_EXIT
        GET_DOORBELL:
        .if .amdgcn.gfx_generation_number < 11
            s_mov_b32 ttmp2, exec_lo
            s_mov_b32 ttmp3, exec_hi
            s_mov_b32 exec_lo, 0x80000000
            s_sendmsg 10
            WAIT_SENDMSG:
            /*wait until msb is cleared (i.e. doorbell fetched)*/
            s_nop 7
            s_bitcmp0_b32 exec_lo, 0x1F
            s_cbranch_scc0 WAIT_SENDMSG
            /* restore exec */
            s_mov_b32 exec_hi, ttmp3
            s_and_b32 exec_lo, exec_lo, 0xfff
            s_mov_b32 ttmp3, exec_lo
            s_mov_b32 exec_lo, ttmp2
        .else
            s_sendmsg_rtn_b32 ttmp3, sendmsg(MSG_RTN_GET_DOORBELL)
            s_waitcnt lgkmcnt(0)
            s_and_b32 ttmp3, ttmp3, 0x3ff
        .endif
        s_mov_b32 ttmp2, m0
        s_or_b32 ttmp3, ttmp3, 0x800
        /* set m0, send interrupt and restore m0 and exit trap*/
        s_mov_b32 m0, ttmp3
        s_nop 0x0
        s_sendmsg sendmsg(MSG_INTERRUPT)
        s_waitcnt lgkmcnt(0)
        s_mov_b32 m0, ttmp2
        v_mov_b32 v4, ttmp1
        /* restore and increment program counter to skip shader trap jump*/
        s_add_u32 ttmp0, ttmp0, 4
        s_addc_u32 ttmp1, ttmp1, 0
        s_and_b32 ttmp1, ttmp1, 0xffff
        RESTORE_AND_EXIT:
        /* restore SQ_WAVE_IB_STS */
        s_lshr_b32 ttmp2, ttmp11, (26 - 15)
        s_and_b32 ttmp2, ttmp2, (0x8000 | 0x1F0000)
        s_setreg_b32 hwreg(HW_REG_IB_STS), ttmp2
        /* restore SQ_WAVE_STATUS */
        s_and_b64 exec, exec, exec
        s_and_b64 vcc, vcc, vcc
        s_setreg_b32 hwreg(HW_REG_STATUS), ttmp12
        s_rfe_b64 [ttmp0, ttmp1]
)";

#define WATCH_START SHADER_START SHADER_MACROS_U32\
    "v_mov_b32 v0, s0\n"\
    "v_mov_b32 v1, s1\n"\
    "v_mov_b32 v2, s2\n"\
    "v_mov_b32 v3, s3\n"\
    "flat_load_dword v4, v[2:3]\n"\
    "s_waitcnt vmcnt(0) & lgkmcnt(0)\n"\
    "v_mov_b32 v5, 0\n"\
    "v_mov_b32 v6, 0\n"

#define WATCH_END "\n"\
    "v_mov_b32 v4, 2\n"\
    "LOOP:\n"\
    "V_CMP_EQ_U32 v6, 0\n"\
    "s_cbranch_vccnz LOOP\n"\
    "V_ADD_CO_U32 v6, v6, v5\n"\
    "flat_store_dword v[2:3], v6\n"\
    "s_waitcnt vmcnt(0) & lgkmcnt(0)\n"\
    "s_endpgm\n"

/* Shader: WatchReadIsa
 * Purpose: Perform a read from watched address (s[0:1]) and then enter a loop updating a counter and eventually storing result.
 * Pseudo-code (simplified):
 *   void shader_watch_read(uint32_t *addr, uint32_t *out) {
 *       uint32_t base = *out; uint32_t chk=0; while(!exit_condition()) chk += base; *out = chk; }
 */
static const char WatchReadIsa[] =
    WATCH_START
    "flat_load_dword v7, v[0:1]"
    WATCH_END;

/* Shader: WatchWriteIsa
 * Purpose: Perform an initial store to watched address then loop similarly to WatchReadIsa accumulating until exit.
 * Pseudo-code:
 *   void shader_watch_write(uint32_t *addr, uint32_t *out) {
 *       *addr = out[0]; uint32_t chk=0; while(!exit_condition()) chk += out[0]; out[0]=chk; }
 */
static const char WatchWriteIsa[] =
    WATCH_START
    "flat_store_dword v[0:1], v4"
    WATCH_END;

static const char * const amdgpu_test_shaders[] = {
        NoopIsa,
        CopyDwordIsa,
        InfiniteLoopIsa,
        AtomicIncIsa,
        ScratchCopyDwordIsa,
        PollMemoryIsa,
        PollAndCopyIsa,
        LoopIsa,
        PersistentIterateIsa,
        ReadMemoryIsa,
        CheckCuMaskIsa,
        //PollNCMemoryIsa,
        //CopyOnSignalIsa,
        WriteFlagAndValueIsa,
        WriteAndSignalIsa,
        //GwsInitIsa,
        GwsAtomicIncreaseIsa,
        JumpToTrapIsa,
        WatchReadIsa,
        WatchWriteIsa,
        NULL
};

#endif
