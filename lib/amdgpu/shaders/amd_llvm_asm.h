/* SPDX-License-Identifier: MIT
 * Runtime AMDGPU ISA assembler interface using LLVM MC (optional).
 * If LLVM development libraries are not available at build time this
 * header still exists but the implementation will be a stub and all
 * entry points return -ENOTSUP.
 */
#ifndef AMD_LLVM_ASM_H
#define AMD_LLVM_ASM_H

#include <stddef.h>
#include <stdint.h>

/* Initialize LLVM target tables required for AMDGPU assembly.
 * Returns 0 on success, negative errno on failure. Safe to call multiple times. */
int amdgpu_llvm_asm_init(void);

/* Shutdown / release any static LLVM state (optional). */
void amdgpu_llvm_asm_shutdown(void);

/* Assemble the given textual ISA (AMDGPU GCN) for the provided MCPU target.
 * mcpu example: "gfx942", "gfx1100" etc.
 * out_buf: destination buffer for raw .text bytes
 * out_buf_size: capacity of out_buf
 * out_size: filled with number of bytes written on success
 * Returns 0 on success, -ENOSPC if buffer too small, -EINVAL for bad input,
 * -ENOTSUP if LLVM support not compiled in, or other negative errno on MC failure.
 */
int amdgpu_llvm_assemble(const char *mcpu,
                         const char *isa_text,
                         uint8_t *out_buf,
                         size_t out_buf_size,
                         size_t *out_size);

/* Convenience: derive mcpu string from (major, minor, stepping) triplet. */
void amdgpu_format_mcpu(unsigned major, unsigned minor, unsigned step,
                        char *dst, size_t dst_size);

/* Map drm/amdgpu_query_gpu_info family_id to a suitable mcpu target.
 * Fills dst with an mcpu like "gfx1200"; falls back to "gfx803" if unknown.
 * Returns 0 on success. */
int amdgpu_family_id_to_mcpu(uint32_t family_id, char *dst, size_t dst_size);

#endif /* AMD_LLVM_ASM_H */
