/* SPDX-License-Identifier: MIT */
/* Stub implementation used when LLVM dev libraries are not present. */
#include "amd_llvm_asm_stub.h"
#include <errno.h>
#include <stdio.h>

int amdgpu_llvm_asm_init(void)
{
	return -ENOTSUP; /* signals feature unavailable */
}

void amdgpu_llvm_asm_shutdown(void)
{
}

void amdgpu_format_mcpu(unsigned int major, unsigned int minor, unsigned int step,
			char *dst, size_t dst_size)
{
	if (!dst || dst_size < 8)
		return;
	/* Format gfx<major><minor><hexstep> similar to KFD assembler */
	snprintf(dst, dst_size, "gfx%u%u%x", major, minor, step & 0xFF);
}

int amdgpu_family_id_to_mcpu(uint32_t family_id, char *dst, size_t dst_size)
{
	return -ENOTSUP;
}

int amdgpu_llvm_assemble(const char *mcpu,
		 const char *isa_text,
		 uint8_t *out_buf,
		 size_t out_buf_size,
		 size_t *out_size)
{
	return -ENOTSUP;
}
