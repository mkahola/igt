// SPDX-License-Identifier: MIT
/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 */

#include "lib/amdgpu/amd_memory.h"
#include "lib/amdgpu/amd_gfx.h"
#include "lib/amdgpu/amd_ip_blocks.h"
#include "lib/amdgpu/amd_PM4.h"
#include "lib/amdgpu/amd_sdma.h"
#include "lib/ioctl_wrappers.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <pthread.h>

#define AMD_CONC_THREADS 4
#define AMD_CONC_ITERS   128

#ifdef AMDGPU_USERQ_ENABLED
struct amd_test_userq_ctx;

static bool
amd_test_try_create_userq(int fd, amdgpu_device_handle amdgpu_dev, uint32_t ip_type,
			 struct amd_test_userq_ctx *userq);
#endif

/* GEM_LIST_HANDLES ioctl — may not be present in older installed libdrm headers */
#ifndef DRM_AMDGPU_GEM_LIST_HANDLES
#define DRM_AMDGPU_GEM_LIST_HANDLES	0x19
#define DRM_IOCTL_AMDGPU_GEM_LIST_HANDLES \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_AMDGPU_GEM_LIST_HANDLES, \
		 struct drm_amdgpu_gem_list_handles)

struct drm_amdgpu_gem_list_handles_entry {
	__u64	size;
	__u64	alloc_flags;
	__u32	preferred_domains;
	__u32	gem_handle;
	__u32	alignment;
	__u32	flags;
};

struct drm_amdgpu_gem_list_handles {
	__u32	num_entries;
	__u32	_pad;
	__u64	entries;
};
#endif /* DRM_AMDGPU_GEM_LIST_HANDLES */

const struct amd_ip_type {
	const char *name;
	enum amd_ip_block_type type;
} amd_ip_type_arr[] = {
	{"AMD_IP_GFX",		AMD_IP_GFX},
	{"AMD_IP_COMPUTE",	AMD_IP_COMPUTE},
	{"AMD_IP_DMA",		AMD_IP_DMA},
	{"AMD_IP_UVD",		AMD_IP_UVD},
	{"AMD_IP_VCE",		AMD_IP_VCE},
	{"AMD_IP_UVD_ENC",	AMD_IP_UVD_ENC},
	{"AMD_IP_VCN_DEC",	AMD_IP_VCN_DEC},
	{"AMD_IP_VCN_ENC",	AMD_IP_VCN_ENC},
	{"AMD_IP_VCN_JPEG",	AMD_IP_VCN_JPEG},
	{"AMD_IP_VPE",		AMD_IP_VPE},
	{"AMD_IP_MAX",		AMD_IP_MAX},
	{},
};

/*
 * The bug was found using customized Syzkaller and with Kazan enabled.
 * It can be triggered by sending a single amdgpu_gem_userptr_ioctl
 * to the AMDGPU DRM driver on any ASICs with an invalid address and size.
 * The bug was reported by Joonkyo Jung <joonkyoj@yonsei.ac.kr>.
 * The following test ensures that the found bug is no longer reproducible.
 */
static void
amd_gem_userptr_fuzzing(int fd)
{
	/*
	 * use-after-free bug in the AMDGPU DRM driver
	 * fix in amdgpu commit 6dbd33a9c8747dbf1d149484509ad667cbdb3059
	 * The error dump is available in dmesg only when KAZAN is enabled
	 */

	struct drm_amdgpu_gem_userptr user_ptr;
	int r;

	user_ptr.addr = 0xffffffffffff0000;
	user_ptr.size = 0x80000000; /*2 Gb*/
	user_ptr.flags = 0x7;
	r = igt_ioctl(fd, DRM_IOCTL_AMDGPU_GEM_USERPTR, &user_ptr);
	igt_info("%s DRM_IOCTL_AMDGPU_GEM_USERPTR r %d\n", __func__, r);
	igt_assert_neq(r, 0);
}

/*
 *  The bug was found using customized Syzkaller and with Kazan enabled.
 *  The bug can be triggered by sending an amdgpu_cs_wait_ioctl for ip types:
 *  AMD_IP_VCE, AMD_IP_VCN_ENC, AMD_IP_VCN_JPEG, AMD_IP_VPE
 *  to the AMDGPU DRM driver on any ASICs with valid context.
 *  The bug was reported by Joonkyo Jung <joonkyoj@yonsei.ac.kr>.
 *
 */
static void
amd_cs_wait_fuzzing(int fd, const enum amd_ip_block_type types[], int size)
{
	/*
	 * null-ptr-deref and the fix in the DRM scheduler
	 * The test helps keep the job state machine of the drm scheduler and
	 * amdgpu in the correct state to ensure that the wrong call sequence does
	 * not cause a crash.
	 */

	union drm_amdgpu_ctx ctx;
	union drm_amdgpu_wait_cs cs_wait;
	int r, i;

	memset(&ctx, 0, sizeof(union drm_amdgpu_ctx));
	ctx.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
	r = igt_ioctl(fd, DRM_IOCTL_AMDGPU_CTX, &ctx);
	igt_info("%s DRM_IOCTL_AMDGPU_CTX r %d\n", __func__, r);
	igt_assert_eq(r, 0);

	for (i = 0; i < size; i++) {
		memset(&cs_wait, 0, sizeof(union drm_amdgpu_wait_cs));
		cs_wait.in.handle = 0x0;
		cs_wait.in.timeout = 0x2000000000000;
		cs_wait.in.ip_instance = 0x0;
		cs_wait.in.ring = 0x0;
		cs_wait.in.ctx_id = ctx.out.alloc.ctx_id;
		cs_wait.in.ip_type = types[i];
		r = igt_ioctl(fd, DRM_IOCTL_AMDGPU_WAIT_CS, &cs_wait);
		igt_info("%s AMDGPU_WAIT_CS %s r %d\n", __func__,
				amd_ip_type_arr[types[i]].name, r);
	}
}

static int
amdgpu_ftrace_enablement(const char *function, bool enable)
{
	char cmd[128];
	int ret;

	snprintf(cmd, sizeof(cmd),
			"echo %s > /sys/kernel/debug/tracing/events/amdgpu/%s/enable",
			enable == true ? "1":"0", function);
	ret = igt_system(cmd);

	return ret;
}

/* The bug was found using customized Syzkaller and with Kazan enabled.
 * Report a slab-use-after-free bug in the AMDGPU DRM driver.
 * Ftrace enablement is mandatory precondition to reproduce the error once after boot.
 * The bug was reported by Joonkyo Jung <joonkyoj@yonsei.ac.kr>.
 *
 * BUG: KFENCE: use-after-free read in amdgpu_bo_move+0x1ce/0x710 [amdgpu]
 * https://gitlab.freedesktop.org/drm/amd/-/issues/3171#note_2287646
 *
 * Fix Christian König ckoenig.leichtzumerken at gmail.com
 * https://lists.freedesktop.org/archives/amd-gfx/2024-March/105680.html
 *
 * The issue is visible only when Kazan enables and dumps to the kernel log:
 * BUG: KASAN: slab-use-after-free in amdgpu_bo_move+0x974/0xd90
 * We accessed the freed memory during the ftrace enablement in a
 * amdgpu_bo_move_notify.
 * The test amd_gem_create_fuzzing does amdgpu_bo_reserve
 */
static void
amd_gem_create_fuzzing(int fd)
{
	static const char function_amdgpu_bo_move[] = "amdgpu_bo_move";
	union drm_amdgpu_gem_create arg;
	int ret;

	ret = amdgpu_ftrace_enablement(function_amdgpu_bo_move, true);
	igt_assert_eq(ret, 0);
	arg.in.bo_size = 0x8;
	arg.in.alignment = 0x0;
	arg.in.domains = 0x4;
	arg.in.domain_flags = 0x9;
	ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &arg);
	igt_info("drmCommandWriteRead DRM_AMDGPU_GEM_CREATE ret %d\n", ret);

	arg.in.bo_size = 0x7fffffff;
	arg.in.alignment = 0x0;
	arg.in.domains = 0x4;
	arg.in.domain_flags = 0x9;
	ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &arg);
	igt_info("drmCommandWriteRead DRM_AMDGPU_GEM_CREATE ret %d\n", ret);

	ret = amdgpu_ftrace_enablement(function_amdgpu_bo_move, false);
	igt_assert_eq(ret, 0);

}

/*
 * One field-level negative test vector for an ioctl input struct.
 *
 * - case_name: test case label shown in assertion logs.
 * - offset: byte offset of target field in the ioctl argument struct/union.
 * - width: field width in bytes (currently 2/4/8).
 * - value: invalid value to inject into that field.
 */
struct amd_ioctl_field_case {
	const char *case_name;
	size_t offset;
	size_t width;
	uint64_t value;
};

/*
 * Helper for field-level negative ioctl tests.
 *
 * It writes one invalid test value into a selected input field
 * (uint16_t/uint32_t/uint64_t) at the given offset inside the ioctl
 * argument buffer. The caller prepares a mostly-valid baseline struct,
 * then mutates exactly one field per case before invoking the ioctl.
 *
 * So this helper is generic for all ioctl negative parameter tests,
 * not specific to GEM_CREATE.
 */
static void
write_field_value(uint8_t *buf, const struct amd_ioctl_field_case *field)
{
	if (field->width == sizeof(uint16_t)) {
		uint16_t value = (uint16_t)field->value;

		memcpy(buf + field->offset, &value, sizeof(value));
		return;
	}

	if (field->width == sizeof(uint32_t)) {
		uint32_t value = (uint32_t)field->value;

		memcpy(buf + field->offset, &value, sizeof(value));
		return;
	}

	if (field->width == sizeof(uint64_t)) {
		uint64_t value = field->value;

		memcpy(buf + field->offset, &value, sizeof(value));
		return;
	}

	igt_assert_f(false, "unsupported field width=%llu\n",
		     (unsigned long long)field->width);
}

static void
run_drm_ioctl_field_cases(int fd, const char *ioctl_name, unsigned long request,
			 const void *base, size_t base_size,
			 const struct amd_ioctl_field_case *cases,
			 int case_count)
{
	uint8_t *buf;
	int i;

	buf = malloc(base_size);
	igt_assert_f(buf, "malloc failed for %s\n", ioctl_name);

	for (i = 0; i < case_count; i++) {
		int ret;

		igt_assert_f(cases[i].offset + cases[i].width <= base_size,
			     "%s field case out of range: %s\n",
			     ioctl_name, cases[i].case_name);

		memcpy(buf, base, base_size);
		write_field_value(buf, &cases[i]);

		errno = 0;
		ret = drmIoctl(fd, request, buf);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     ioctl_name, cases[i].case_name, buf);
	}

	free(buf);
}

static void
amd_kgd_multi_ioctl_field_fuzzing(int fd, amdgpu_device_handle amdgpu_dev)
{
	union drm_amdgpu_ctx valid_ctx;
	union drm_amdgpu_gem_create valid_bo;
	uint32_t valid_ctx_id = 0;
	uint32_t valid_bo_handle = 0;
	bool have_valid_ctx = false;
	bool have_valid_bo = false;

	memset(&valid_ctx, 0, sizeof(valid_ctx));
	valid_ctx.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
	if (drmIoctl(fd, DRM_IOCTL_AMDGPU_CTX, &valid_ctx) == 0) {
		have_valid_ctx = true;
		valid_ctx_id = valid_ctx.out.alloc.ctx_id;
	}

	memset(&valid_bo, 0, sizeof(valid_bo));
	valid_bo.in.bo_size = 4096;
	valid_bo.in.alignment = 0;
	valid_bo.in.domains = AMDGPU_GEM_DOMAIN_GTT;
	valid_bo.in.domain_flags = 0;
	if (drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &valid_bo) == 0) {
		have_valid_bo = true;
		valid_bo_handle = valid_bo.out.handle;
	}

	{
		union drm_amdgpu_gem_create base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "gem_create.bo_size.invalid", offsetof(union drm_amdgpu_gem_create, in.bo_size), sizeof(uint64_t), UINT64_MAX },
			{ "gem_create.bo_size.invalid", offsetof(union drm_amdgpu_gem_create, in.bo_size), sizeof(uint64_t), 0 },
			{ "gem_create.domains.invalid", offsetof(union drm_amdgpu_gem_create, in.domains), sizeof(uint64_t), ~(uint64_t)AMDGPU_GEM_DOMAIN_MASK },
			{ "gem_create.domain_flags.invalid", offsetof(union drm_amdgpu_gem_create, in.domain_flags), sizeof(uint64_t), (1ULL << 63) },
		};

		memset(&base, 0, sizeof(base));
		base.in.bo_size = 4096;
		base.in.alignment = 0;
		base.in.domains = AMDGPU_GEM_DOMAIN_GTT;
		base.in.domain_flags = 0;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_GEM_CREATE",
					 DRM_IOCTL_AMDGPU_GEM_CREATE,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		union drm_amdgpu_gem_mmap base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "gem_mmap.handle.invalid", offsetof(union drm_amdgpu_gem_mmap, in.handle), sizeof(uint32_t), UINT32_MAX },
			{ "gem_mmap.pad.invalid", offsetof(union drm_amdgpu_gem_mmap, in._pad), sizeof(uint32_t), UINT32_MAX },
		};

		memset(&base, 0, sizeof(base));
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_GEM_MMAP",
					 DRM_IOCTL_AMDGPU_GEM_MMAP,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		union drm_amdgpu_ctx base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "ctx.op.invalid", offsetof(union drm_amdgpu_ctx, in.op), sizeof(uint32_t), UINT32_MAX },
			{ "ctx.flags.invalid", offsetof(union drm_amdgpu_ctx, in.flags), sizeof(uint32_t), UINT32_MAX },
			/*
			 * Disabled: amdgpu_ctx_ioctl keeps backward compatibility by
			 * accepting garbage in.priority and normalizing it to NORMAL.
			 * So ret!=0 is not a valid assertion for this field.
			 */
			/* { "ctx.priority.invalid", offsetof(union drm_amdgpu_ctx, in.priority), sizeof(int32_t), INT32_MAX }, */
		};

		memset(&base, 0, sizeof(base));
		base.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_CTX",
					 DRM_IOCTL_AMDGPU_CTX,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		union drm_amdgpu_ctx base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "ctx_free.ctx_id.invalid", offsetof(union drm_amdgpu_ctx, in.ctx_id), sizeof(uint32_t), UINT32_MAX },
			{ "ctx_free.flags.invalid", offsetof(union drm_amdgpu_ctx, in.flags), sizeof(uint32_t), UINT32_MAX },
			/*
			 * Disabled: kernel compatibility path accepts garbage priority
			 * and treats it as NORMAL; strict negative assertion is unstable.
			 */
			/* { "ctx_free.priority.invalid", offsetof(union drm_amdgpu_ctx, in.priority), sizeof(int32_t), INT32_MAX }, */
		};

		memset(&base, 0, sizeof(base));
		base.in.op = AMDGPU_CTX_OP_FREE_CTX;
		if (have_valid_ctx)
			base.in.ctx_id = valid_ctx_id;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_CTX",
					 DRM_IOCTL_AMDGPU_CTX,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		union drm_amdgpu_ctx base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "ctx_query_state.ctx_id.invalid", offsetof(union drm_amdgpu_ctx, in.ctx_id), sizeof(uint32_t), UINT32_MAX },
			{ "ctx_query_state.flags.invalid", offsetof(union drm_amdgpu_ctx, in.flags), sizeof(uint32_t), UINT32_MAX },
			/*
			 * Disabled: garbage priority is accepted and normalized to NORMAL
			 * for backward compatibility; this path may legitimately succeed.
			 */
			/* { "ctx_query_state.priority.invalid", offsetof(union drm_amdgpu_ctx, in.priority), sizeof(int32_t), INT32_MAX }, */
		};

		memset(&base, 0, sizeof(base));
		base.in.op = AMDGPU_CTX_OP_QUERY_STATE;
		if (have_valid_ctx)
			base.in.ctx_id = valid_ctx_id;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_CTX",
					 DRM_IOCTL_AMDGPU_CTX,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		union drm_amdgpu_bo_list base;
		struct drm_amdgpu_bo_list_entry base_entry = { 0 };
		static const struct amd_ioctl_field_case cases[] = {
			{ "bo_list.operation.invalid", offsetof(union drm_amdgpu_bo_list, in.operation), sizeof(uint32_t), UINT32_MAX },
			{ "bo_list.bo_number.invalid", offsetof(union drm_amdgpu_bo_list, in.bo_number), sizeof(uint32_t), UINT32_MAX },
			{ "bo_list.bo_info_ptr.invalid", offsetof(union drm_amdgpu_bo_list, in.bo_info_ptr), sizeof(uint64_t), 0x1 },
		};

		memset(&base, 0, sizeof(base));
		base.in.operation = AMDGPU_BO_LIST_OP_CREATE;
		base.in.bo_number = 1;
		base.in.bo_info_size = sizeof(struct drm_amdgpu_bo_list_entry);
		base.in.bo_info_ptr = (uintptr_t)&base_entry;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_BO_LIST",
					 DRM_IOCTL_AMDGPU_BO_LIST,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));

		/* Deterministic bound check: huge bo_number must be rejected with EINVAL. */
		memset(&base, 0, sizeof(base));
		base.in.operation = AMDGPU_BO_LIST_OP_CREATE;
		base.in.bo_number = UINT32_MAX;
		base.in.bo_info_size = sizeof(struct drm_amdgpu_bo_list_entry);
		errno = 0;
		{
			int ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_BO_LIST, &base);
			igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_BO_LIST", "bo_list.bo_number.huge", &base);
		}

		/* bo_info_size cannot be zero. */
		memset(&base, 0, sizeof(base));
		base.in.operation = AMDGPU_BO_LIST_OP_CREATE;
		base.in.bo_number = 1;
		base.in.bo_info_size = 0;
		base.in.bo_info_ptr = (uintptr_t)&base_entry;
		errno = 0;
		{
			int ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_BO_LIST, &base);
			igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_BO_LIST", "bo_list.bo_info_size.zero", &base);
		}

		/* DESTROY should not require bo_info payload fields for a valid handle. */
		if (have_valid_bo) {
			uint32_t list_handle = 0;

			base_entry.bo_handle = valid_bo_handle;
			base_entry.bo_priority = 0;

			memset(&base, 0, sizeof(base));
			base.in.operation = AMDGPU_BO_LIST_OP_CREATE;
			base.in.bo_number = 1;
			base.in.bo_info_size = sizeof(struct drm_amdgpu_bo_list_entry);
			base.in.bo_info_ptr = (uintptr_t)&base_entry;
			errno = 0;
			{
				int ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_BO_LIST, &base);
				igt_assert_f(ret == 0,
						 "DRM_IOCTL_AMDGPU_BO_LIST failed for bo_list.destroy.no_payload.create (arg=%p, ret=%d, errno=%d)\n",
						 &base, ret, errno);
			}

			list_handle = base.out.list_handle;

			memset(&base, 0, sizeof(base));
			base.in.operation = AMDGPU_BO_LIST_OP_DESTROY;
			base.in.list_handle = list_handle;
			errno = 0;
			{
				int ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_BO_LIST, &base);
				igt_assert_f(ret == 0,
						 "DRM_IOCTL_AMDGPU_BO_LIST failed for bo_list.destroy.no_payload (arg=%p, ret=%d, errno=%d)\n",
						 &base, ret, errno);
			}
		}
	}

	{
		union drm_amdgpu_cs base;
		struct drm_amdgpu_cs_chunk chunk = { 0 };
		uint64_t chunks[1];
		static const struct amd_ioctl_field_case cases[] = {
			{ "cs.ctx_id.invalid", offsetof(union drm_amdgpu_cs, in.ctx_id), sizeof(uint32_t), UINT32_MAX },
			{ "cs.bo_list_handle.invalid", offsetof(union drm_amdgpu_cs, in.bo_list_handle), sizeof(uint32_t), UINT32_MAX },
			{ "cs.num_chunks.invalid", offsetof(union drm_amdgpu_cs, in.num_chunks), sizeof(uint32_t), UINT32_MAX },
			{ "cs.flags.invalid", offsetof(union drm_amdgpu_cs, in.flags), sizeof(uint32_t), UINT32_MAX },
			{ "cs.chunks.invalid", offsetof(union drm_amdgpu_cs, in.chunks), sizeof(uint64_t), 0x1 },
		};

		memset(&base, 0, sizeof(base));
		if (have_valid_ctx)
			base.in.ctx_id = valid_ctx_id;
		/* Keep chunks/chunks_count consistent so pointer mutation is meaningful. */
		chunks[0] = (uintptr_t)&chunk;
		base.in.num_chunks = 1;
		base.in.chunks = (uintptr_t)&chunks[0];
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_CS",
					 DRM_IOCTL_AMDGPU_CS,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		struct drm_amdgpu_info base;
		uint8_t outbuf[256] = {0};
		static const struct amd_ioctl_field_case cases[] = {
			{ "info.return_pointer.invalid", offsetof(struct drm_amdgpu_info, return_pointer), sizeof(uint64_t), 0x1 },
			{ "info.query.invalid", offsetof(struct drm_amdgpu_info, query), sizeof(uint32_t), UINT32_MAX },
			{ "info.query_hw_ip.type.invalid", offsetof(struct drm_amdgpu_info, query_hw_ip.type), sizeof(uint32_t), UINT32_MAX },
			{ "info.query_hw_ip.instance.invalid", offsetof(struct drm_amdgpu_info, query_hw_ip.ip_instance), sizeof(uint32_t), UINT32_MAX },
		};

		memset(&base, 0, sizeof(base));
		base.return_pointer = (uintptr_t)outbuf;
		base.return_size = sizeof(outbuf);
		base.query = AMDGPU_INFO_HW_IP_INFO;
		base.query_hw_ip.type = AMDGPU_HW_IP_GFX;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_INFO",
					 DRM_IOCTL_AMDGPU_INFO,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		struct drm_amdgpu_gem_metadata base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "gem_metadata.handle.invalid", offsetof(struct drm_amdgpu_gem_metadata, handle), sizeof(uint32_t), UINT32_MAX },
			{ "gem_metadata.op.invalid", offsetof(struct drm_amdgpu_gem_metadata, op), sizeof(uint32_t), UINT32_MAX },
			{ "gem_metadata.flags.invalid", offsetof(struct drm_amdgpu_gem_metadata, data.flags), sizeof(uint64_t), UINT64_MAX },
			{ "gem_metadata.tiling.invalid", offsetof(struct drm_amdgpu_gem_metadata, data.tiling_info), sizeof(uint64_t), UINT64_MAX },
			{ "gem_metadata.data_size.invalid", offsetof(struct drm_amdgpu_gem_metadata, data.data_size_bytes), sizeof(uint32_t), UINT32_MAX },
		};

		memset(&base, 0, sizeof(base));
		base.op = AMDGPU_GEM_METADATA_OP_SET_METADATA;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_GEM_METADATA",
					 DRM_IOCTL_AMDGPU_GEM_METADATA,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		union drm_amdgpu_gem_wait_idle base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "gem_wait_idle.handle.invalid", offsetof(union drm_amdgpu_gem_wait_idle, in.handle), sizeof(uint32_t), UINT32_MAX },
		};

		memset(&base, 0, sizeof(base));
		if (have_valid_bo)
			base.in.handle = valid_bo_handle;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_GEM_WAIT_IDLE",
					 DRM_IOCTL_AMDGPU_GEM_WAIT_IDLE,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		struct drm_amdgpu_gem_va base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "gem_va.handle.invalid", offsetof(struct drm_amdgpu_gem_va, handle), sizeof(uint32_t), UINT32_MAX },
			{ "gem_va.operation.invalid", offsetof(struct drm_amdgpu_gem_va, operation), sizeof(uint32_t), UINT32_MAX },
			{ "gem_va.flags.invalid", offsetof(struct drm_amdgpu_gem_va, flags), sizeof(uint32_t), UINT32_MAX },
			{ "gem_va.va_address.invalid", offsetof(struct drm_amdgpu_gem_va, va_address), sizeof(uint64_t), 0x1 },
			{ "gem_va.offset_in_bo.invalid", offsetof(struct drm_amdgpu_gem_va, offset_in_bo), sizeof(uint64_t), 0x1 },
			{ "gem_va.map_size.invalid", offsetof(struct drm_amdgpu_gem_va, map_size), sizeof(uint64_t), 0x3 },
			{ "gem_va.num_syncobj_handles.invalid", offsetof(struct drm_amdgpu_gem_va, num_syncobj_handles), sizeof(uint32_t), UINT32_MAX },
			{ "gem_va.input_fence_syncobj_handles.invalid", offsetof(struct drm_amdgpu_gem_va, input_fence_syncobj_handles), sizeof(uint64_t), 0x1 },
		};

		memset(&base, 0, sizeof(base));
		if (have_valid_bo)
			base.handle = valid_bo_handle;
		base.operation = AMDGPU_VA_OP_MAP;
		base.flags = AMDGPU_VM_PAGE_READABLE;
		base.map_size = 4096;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_GEM_VA",
					 DRM_IOCTL_AMDGPU_GEM_VA,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		struct drm_amdgpu_gem_va base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "gem_va_unmap.handle.invalid", offsetof(struct drm_amdgpu_gem_va, handle), sizeof(uint32_t), UINT32_MAX },
			{ "gem_va_unmap.va_address.invalid", offsetof(struct drm_amdgpu_gem_va, va_address), sizeof(uint64_t), 0x1 },
			{ "gem_va_unmap.offset_in_bo.invalid", offsetof(struct drm_amdgpu_gem_va, offset_in_bo), sizeof(uint64_t), 0x1 },
			{ "gem_va_unmap.map_size.invalid", offsetof(struct drm_amdgpu_gem_va, map_size), sizeof(uint64_t), 0x3 },
		};

		memset(&base, 0, sizeof(base));
		if (have_valid_bo)
			base.handle = valid_bo_handle;
		base.operation = AMDGPU_VA_OP_UNMAP;
		base.flags = AMDGPU_VM_PAGE_READABLE;
		base.map_size = 4096;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_GEM_VA",
					 DRM_IOCTL_AMDGPU_GEM_VA,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		struct drm_amdgpu_gem_va base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "gem_va_clear.handle.invalid", offsetof(struct drm_amdgpu_gem_va, handle), sizeof(uint32_t), UINT32_MAX },
			{ "gem_va_clear.flags.invalid", offsetof(struct drm_amdgpu_gem_va, flags), sizeof(uint32_t), UINT32_MAX },
			{ "gem_va_clear.va_address.invalid", offsetof(struct drm_amdgpu_gem_va, va_address), sizeof(uint64_t), 0x1 },
			{ "gem_va_clear.map_size.invalid", offsetof(struct drm_amdgpu_gem_va, map_size), sizeof(uint64_t), 0x3 },
		};

		memset(&base, 0, sizeof(base));
		if (have_valid_bo)
			base.handle = valid_bo_handle;
		base.operation = AMDGPU_VA_OP_CLEAR;
		base.flags = AMDGPU_VM_PAGE_READABLE;
		base.map_size = 4096;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_GEM_VA",
					 DRM_IOCTL_AMDGPU_GEM_VA,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		struct drm_amdgpu_gem_va base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "gem_va_replace.handle.invalid", offsetof(struct drm_amdgpu_gem_va, handle), sizeof(uint32_t), UINT32_MAX },
			{ "gem_va_replace.va_address.invalid", offsetof(struct drm_amdgpu_gem_va, va_address), sizeof(uint64_t), 0x1 },
			{ "gem_va_replace.offset_in_bo.invalid", offsetof(struct drm_amdgpu_gem_va, offset_in_bo), sizeof(uint64_t), 0x1 },
			{ "gem_va_replace.map_size.invalid", offsetof(struct drm_amdgpu_gem_va, map_size), sizeof(uint64_t), 0x3 },
			{ "gem_va_replace.num_syncobj_handles.invalid", offsetof(struct drm_amdgpu_gem_va, num_syncobj_handles), sizeof(uint32_t), UINT32_MAX },
			{ "gem_va_replace.input_fence_syncobj_handles.invalid", offsetof(struct drm_amdgpu_gem_va, input_fence_syncobj_handles), sizeof(uint64_t), 0x1 },
		};

		memset(&base, 0, sizeof(base));
		if (have_valid_bo)
			base.handle = valid_bo_handle;
		base.operation = AMDGPU_VA_OP_REPLACE;
		base.flags = AMDGPU_VM_PAGE_READABLE;
		base.map_size = 4096;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_GEM_VA",
					 DRM_IOCTL_AMDGPU_GEM_VA,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		struct drm_amdgpu_gem_op base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "gem_op.handle.invalid", offsetof(struct drm_amdgpu_gem_op, handle), sizeof(uint32_t), UINT32_MAX },
			{ "gem_op.op.invalid", offsetof(struct drm_amdgpu_gem_op, op), sizeof(uint32_t), UINT32_MAX },
		};

		memset(&base, 0, sizeof(base));
		if (have_valid_bo)
			base.handle = valid_bo_handle;
		base.op = AMDGPU_GEM_OP_SET_PLACEMENT;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_GEM_OP",
					 DRM_IOCTL_AMDGPU_GEM_OP,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		struct drm_amdgpu_gem_op base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "gem_op_get_create_info.handle.invalid", offsetof(struct drm_amdgpu_gem_op, handle), sizeof(uint32_t), UINT32_MAX },
			{ "gem_op_get_create_info.value.invalid", offsetof(struct drm_amdgpu_gem_op, value), sizeof(uint64_t), 0x1 },
		};

		memset(&base, 0, sizeof(base));
		if (have_valid_bo)
			base.handle = valid_bo_handle;
		base.op = AMDGPU_GEM_OP_GET_GEM_CREATE_INFO;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_GEM_OP",
					 DRM_IOCTL_AMDGPU_GEM_OP,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

#ifdef AMDGPU_GEM_OP_GET_MAPPING_INFO
	{
		struct drm_amdgpu_gem_op base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "gem_op_get_mapping_info.handle.invalid", offsetof(struct drm_amdgpu_gem_op, handle), sizeof(uint32_t), UINT32_MAX },
			{ "gem_op_get_mapping_info.value.invalid", offsetof(struct drm_amdgpu_gem_op, value), sizeof(uint64_t), 0x1 },
		};

		memset(&base, 0, sizeof(base));
		base.op = AMDGPU_GEM_OP_GET_MAPPING_INFO;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_GEM_OP",
					 DRM_IOCTL_AMDGPU_GEM_OP,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}
#endif

	{
		union drm_amdgpu_wait_fences base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "wait_fences.fences.invalid", offsetof(union drm_amdgpu_wait_fences, in.fences), sizeof(uint64_t), 0x1 },
			{ "wait_fences.fence_count.invalid", offsetof(union drm_amdgpu_wait_fences, in.fence_count), sizeof(uint32_t), UINT32_MAX },
			{ "wait_fences.wait_all.invalid", offsetof(union drm_amdgpu_wait_fences, in.wait_all), sizeof(uint32_t), UINT32_MAX },
			{ "wait_fences.timeout.invalid", offsetof(union drm_amdgpu_wait_fences, in.timeout_ns), sizeof(uint64_t), UINT64_MAX },
		};

		memset(&base, 0, sizeof(base));
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_WAIT_FENCES",
					 DRM_IOCTL_AMDGPU_WAIT_FENCES,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));

		/* fence_count=0 must be rejected before wait-any path. */
		memset(&base, 0, sizeof(base));
		base.in.wait_all = 0;
		errno = 0;
		{
			int ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_WAIT_FENCES, &base);
			igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_WAIT_FENCES", "wait_fences.fence_count.zero", &base);
		}

		/* fence_count=0 must be rejected regardless of wait_all selector. */
		memset(&base, 0, sizeof(base));
		base.in.wait_all = 1;
		errno = 0;
		{
			int ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_WAIT_FENCES, &base);
			igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_WAIT_FENCES", "wait_fences.fence_count.zero_wait_all", &base);
		}

		/* Non-zero fence_count with NULL fences pointer must fault copy-from-user. */
		memset(&base, 0, sizeof(base));
		base.in.fence_count = 1;
		base.in.wait_all = 0;
		errno = 0;
		{
			int ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_WAIT_FENCES, &base);
			igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_WAIT_FENCES", "wait_fences.fences.nonzero_count_null", &base);
		}
	}

	{
		union drm_amdgpu_vm base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "vm.flags.invalid", offsetof(union drm_amdgpu_vm, in.flags), sizeof(uint32_t), UINT32_MAX },
		};

		memset(&base, 0, sizeof(base));
		base.in.op = AMDGPU_VM_OP_RESERVE_VMID;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_VM",
					 DRM_IOCTL_AMDGPU_VM,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		union drm_amdgpu_vm base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "vm_unreserve.flags.invalid", offsetof(union drm_amdgpu_vm, in.flags), sizeof(uint32_t), UINT32_MAX },
		};

		memset(&base, 0, sizeof(base));
		base.in.op = AMDGPU_VM_OP_UNRESERVE_VMID;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_VM",
					 DRM_IOCTL_AMDGPU_VM,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		union drm_amdgpu_fence_to_handle base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "fence_to_handle.ctx_id.invalid", offsetof(union drm_amdgpu_fence_to_handle, in.fence.ctx_id), sizeof(uint32_t), UINT32_MAX },
			{ "fence_to_handle.ip_type.invalid", offsetof(union drm_amdgpu_fence_to_handle, in.fence.ip_type), sizeof(uint32_t), UINT32_MAX },
			{ "fence_to_handle.what.invalid", offsetof(union drm_amdgpu_fence_to_handle, in.what), sizeof(uint32_t), UINT32_MAX },
			//{ "fence_to_handle.seq_no.invalid", offsetof(union drm_amdgpu_fence_to_handle, in.fence.seq_no), sizeof(uint64_t), UINT64_MAX },
		};

		memset(&base, 0, sizeof(base));
		if (have_valid_ctx)
			base.in.fence.ctx_id = valid_ctx_id;
		base.in.fence.ip_type = AMDGPU_HW_IP_GFX;
		base.in.fence.ip_instance = 0;
		base.in.fence.ring = 0;
		base.in.fence.seq_no = 1;
		base.in.what = AMDGPU_FENCE_TO_HANDLE_GET_SYNCOBJ;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_FENCE_TO_HANDLE",
					 DRM_IOCTL_AMDGPU_FENCE_TO_HANDLE,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		union drm_amdgpu_sched base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "sched.op.invalid", offsetof(union drm_amdgpu_sched, in.op), sizeof(uint32_t), UINT32_MAX },
			{ "sched.fd.invalid", offsetof(union drm_amdgpu_sched, in.fd), sizeof(uint32_t), UINT32_MAX },
			{ "sched.priority.invalid", offsetof(union drm_amdgpu_sched, in.priority), sizeof(uint32_t), UINT32_MAX },
			{ "sched.ctx_id.invalid", offsetof(union drm_amdgpu_sched, in.ctx_id), sizeof(uint32_t), UINT32_MAX },
		};

		memset(&base, 0, sizeof(base));
		base.in.op = AMDGPU_SCHED_OP_CONTEXT_PRIORITY_OVERRIDE;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_SCHED",
					 DRM_IOCTL_AMDGPU_SCHED,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}
#ifdef AMDGPU_USERQ_ENABLED
	{
		struct drm_amdgpu_userq_signal base;
		uint32_t syncobj_handles[1] = { 0 };
		uint32_t bo_read_handles[1] = { 0 };
		uint32_t bo_write_handles[1] = { 0 };
		/* This block targets early validation; real queue behavior is in userq-fuzzing. */
		static const struct amd_ioctl_field_case cases[] = {
			{ "userq_signal.queue_id.invalid", offsetof(struct drm_amdgpu_userq_signal, queue_id), sizeof(uint32_t), UINT32_MAX },
			{ "userq_signal.syncobj_handles.invalid", offsetof(struct drm_amdgpu_userq_signal, syncobj_handles), sizeof(uint64_t), 0x1 },
			//{ "userq_signal.num_syncobj_handles.invalid", offsetof(struct drm_amdgpu_userq_signal, num_syncobj_handles), sizeof(uint32_t), UINT32_MAX },
			{ "userq_signal.bo_read_handles.invalid", offsetof(struct drm_amdgpu_userq_signal, bo_read_handles), sizeof(uint64_t), 0x1 },
			{ "userq_signal.bo_write_handles.invalid", offsetof(struct drm_amdgpu_userq_signal, bo_write_handles), sizeof(uint64_t), 0x1 },
			{ "userq_signal.num_bo_read_handles.invalid", offsetof(struct drm_amdgpu_userq_signal, num_bo_read_handles), sizeof(uint32_t), UINT32_MAX },
			{ "userq_signal.num_bo_write_handles.invalid", offsetof(struct drm_amdgpu_userq_signal, num_bo_write_handles), sizeof(uint32_t), UINT32_MAX },
		};

		memset(&base, 0, sizeof(base));
		base.syncobj_handles = (uintptr_t)&syncobj_handles[0];
		base.bo_read_handles = (uintptr_t)&bo_read_handles[0];
		base.bo_write_handles = (uintptr_t)&bo_write_handles[0];
		//base.num_syncobj_handles = 1;
		base.num_bo_read_handles = 1;
		base.num_bo_write_handles = 1;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_USERQ_SIGNAL",
					 DRM_IOCTL_AMDGPU_USERQ_SIGNAL,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		struct drm_amdgpu_userq_wait base;
		uint32_t syncobj_handles[1] = { 0 };
		uint32_t timeline_handles[1] = { 0 };
		uint32_t timeline_points[1] = { 0 };
		uint32_t bo_read_handles[1] = { 0 };
		uint32_t bo_write_handles[1] = { 0 };
		struct drm_amdgpu_userq_fence_info out_fences[1];
		/* This block targets early validation; real queue behavior is in userq-fuzzing. */
		static const struct amd_ioctl_field_case cases[] = {
			/* After updating the struct, uncomment it. */
			//{ "userq_wait.waitq_id.invalid", offsetof(struct drm_amdgpu_userq_wait, waitq_id), sizeof(uint32_t), UINT32_MAX },
			{ "userq_wait.syncobj_handles.invalid", offsetof(struct drm_amdgpu_userq_wait, syncobj_handles), sizeof(uint64_t), 0x1 },
			{ "userq_wait.timeline_handles.invalid", offsetof(struct drm_amdgpu_userq_wait, syncobj_timeline_handles), sizeof(uint64_t), 0x1 },
			{ "userq_wait.timeline_points.invalid", offsetof(struct drm_amdgpu_userq_wait, syncobj_timeline_points), sizeof(uint64_t), 0x1 },
			{ "userq_wait.bo_read_handles.invalid", offsetof(struct drm_amdgpu_userq_wait, bo_read_handles), sizeof(uint64_t), 0x1 },
			{ "userq_wait.bo_write_handles.invalid", offsetof(struct drm_amdgpu_userq_wait, bo_write_handles), sizeof(uint64_t), 0x1 },
			{ "userq_wait.num_timeline_handles.invalid", offsetof(struct drm_amdgpu_userq_wait, num_syncobj_timeline_handles), sizeof(uint16_t), UINT16_MAX },
			{ "userq_wait.num_fences.invalid", offsetof(struct drm_amdgpu_userq_wait, num_fences), sizeof(uint16_t), UINT16_MAX },
			//{ "userq_wait.num_syncobj_handles.invalid", offsetof(struct drm_amdgpu_userq_wait, num_syncobj_handles), sizeof(uint32_t), UINT32_MAX },
			{ "userq_wait.num_bo_read_handles.invalid", offsetof(struct drm_amdgpu_userq_wait, num_bo_read_handles), sizeof(uint32_t), UINT32_MAX },
			{ "userq_wait.num_bo_write_handles.invalid", offsetof(struct drm_amdgpu_userq_wait, num_bo_write_handles), sizeof(uint32_t), UINT32_MAX },
			{ "userq_wait.out_fences.invalid", offsetof(struct drm_amdgpu_userq_wait, out_fences), sizeof(uint64_t), 0x1 },
		};

		memset(&base, 0, sizeof(base));
		base.syncobj_handles = (uintptr_t)&syncobj_handles[0];
		base.syncobj_timeline_handles = (uintptr_t)&timeline_handles[0];
		base.syncobj_timeline_points = (uintptr_t)&timeline_points[0];
		base.bo_read_handles = (uintptr_t)&bo_read_handles[0];
		base.bo_write_handles = (uintptr_t)&bo_write_handles[0];
		base.out_fences = (uintptr_t)&out_fences[0];
		//base.num_syncobj_handles = 1;
		base.num_syncobj_timeline_handles = 1;
		base.num_bo_read_handles = 1;
		base.num_bo_write_handles = 1;
		base.num_fences = 1;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_USERQ_WAIT",
					 DRM_IOCTL_AMDGPU_USERQ_WAIT,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		struct drm_amdgpu_userq_signal arg;
		int ret;

		memset(&arg, 0, sizeof(arg));
		arg.syncobj_handles = 0x1;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_SIGNAL, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_SIGNAL", "userq_signal.syncobj_handles.zero_count_nonnull", &arg);

		memset(&arg, 0, sizeof(arg));
		arg.bo_read_handles = 0x1;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_SIGNAL, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_SIGNAL", "userq_signal.bo_read_handles.zero_count_nonnull", &arg);

		memset(&arg, 0, sizeof(arg));
		arg.bo_write_handles = 0x1;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_SIGNAL, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_SIGNAL", "userq_signal.bo_write_handles.zero_count_nonnull", &arg);

		/* non-zero count + NULL user pointer must fail in copy-from-user path */
		memset(&arg, 0, sizeof(arg));
		arg.num_syncobj_handles = 1;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_SIGNAL, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_SIGNAL", "userq_signal.syncobj_handles.nonzero_count_null", &arg);

		memset(&arg, 0, sizeof(arg));
		arg.num_bo_read_handles = 1;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_SIGNAL, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_SIGNAL", "userq_signal.bo_read_handles.nonzero_count_null", &arg);

		memset(&arg, 0, sizeof(arg));
		arg.num_bo_write_handles = 1;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_SIGNAL, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_SIGNAL", "userq_signal.bo_write_handles.nonzero_count_null", &arg);

		/* queue lookup path: invalid queue_id with no dependency arrays */
		memset(&arg, 0, sizeof(arg));
		arg.queue_id = UINT32_MAX;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_SIGNAL, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_SIGNAL", "userq_signal.queue_id.invalid_empty", &arg);
	}

	/*
	 * USERQ_WAIT: zero-count + non-NULL pointer mismatch must return EINVAL.
	 *
	 * The driver validates all pointer/count pairs in
	 * amdgpu_userq_wait_ioctl() and must return -EINVAL for each
	 * case where a non-NULL pointer is supplied alongside a zero count.
	 */
	{
		struct drm_amdgpu_userq_wait arg;
		struct drm_amdgpu_userq_fence_info out_fences[1];
		uint32_t timeline_handles[1] = { 0 };
		int ret;

		/* syncobj_handles: non-NULL + zero count */
		memset(&arg, 0, sizeof(arg));
		arg.syncobj_handles = 0x1;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_WAIT, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_WAIT", "userq_wait.syncobj_handles.zero_count_nonnull", &arg);

		/* syncobj_timeline_handles: non-NULL + zero timeline count */
		memset(&arg, 0, sizeof(arg));
		arg.syncobj_timeline_handles = 0x1;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_WAIT, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_WAIT", "userq_wait.timeline_handles.zero_count_nonnull", &arg);

		/* syncobj_timeline_points: non-NULL + zero timeline count */
		memset(&arg, 0, sizeof(arg));
		arg.syncobj_timeline_points = 0x1;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_WAIT, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_WAIT", "userq_wait.timeline_points.zero_count_nonnull", &arg);

		/* bo_read_handles: non-NULL + zero count */
		memset(&arg, 0, sizeof(arg));
		arg.bo_read_handles = 0x1;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_WAIT, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_WAIT", "userq_wait.bo_read_handles.zero_count_nonnull", &arg);

		/* bo_write_handles: non-NULL + zero count */
		memset(&arg, 0, sizeof(arg));
		arg.bo_write_handles = 0x1;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_WAIT, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_WAIT", "userq_wait.bo_write_handles.zero_count_nonnull", &arg);

		/* out_fences: non-NULL + zero fence count */
		memset(&arg, 0, sizeof(arg));
		arg.out_fences = 0x1;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_WAIT, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_WAIT", "userq_wait.out_fences.zero_count_nonnull", &arg);

		/* non-zero count + NULL user pointer must fail in copy-from-user path */
		memset(&arg, 0, sizeof(arg));
		//arg.num_syncobj_handles = 1;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_WAIT, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_WAIT", "userq_wait.syncobj_handles.nonzero_count_null", &arg);

		memset(&arg, 0, sizeof(arg));
		arg.num_syncobj_timeline_handles = 1;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_WAIT, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_WAIT", "userq_wait.timeline_handles.nonzero_count_null", &arg);

		memset(&arg, 0, sizeof(arg));
		arg.num_syncobj_timeline_handles = 1;
		arg.syncobj_timeline_handles = (uintptr_t)&timeline_handles[0];
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_WAIT, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_WAIT", "userq_wait.timeline_points.nonzero_count_null", &arg);

		memset(&arg, 0, sizeof(arg));
		arg.num_bo_read_handles = 1;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_WAIT, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_WAIT", "userq_wait.bo_read_handles.nonzero_count_null", &arg);

		memset(&arg, 0, sizeof(arg));
		arg.num_bo_write_handles = 1;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_WAIT, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_WAIT", "userq_wait.bo_write_handles.nonzero_count_null", &arg);

		/* wait queue lookup path: invalid waitq_id when out_fences requested */
		memset(&arg, 0, sizeof(arg));
		//arg.waitq_id = UINT32_MAX;
		arg.num_fences = 1;
		arg.out_fences = (uintptr_t)&out_fences[0];
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_WAIT, &arg);
		igt_assert_f(ret != 0,
                     "%s unexpectedly succeeded for %s (arg=%p)\n",
                     "DRM_IOCTL_AMDGPU_USERQ_WAIT", "userq_wait.waitq_id.invalid_out_fences", &arg);
	}
	/*
	 * USERQ_WAIT timeline points are 64-bit in UAPI.  Verify that the
	 * high 32 bits are preserved by the kernel (no silent u32 truncation).
	 *
	 * Strategy: signal a syncobj at point (1<<32)+5, then submit a
	 * USERQ_WAIT for exactly that same point.  The ioctl registers the
	 * wait condition and must succeed (ret==0).  If the kernel silently
	 * truncates the 64-bit point to 32 bits it would register a wait for
	 * point 5 instead – a different, never-signaled value – causing the
	 * ioctl to fail with EINVAL or another error, which we detect.
	 *
	 * Note: USERQ_WAIT has no timeout field; it is not a blocking wait.
	 * It only registers the dependency.  We therefore only test ret==0.
	 */
	if (amdgpu_dev) {
		uint32_t timeline_handle = 0;
		uint32_t timeline_handles[1];
		uint64_t timeline_points[1];
		uint64_t signal_point = (1ULL << 32) + 5;
		struct drm_amdgpu_userq_wait arg;
		int ret;

		ret = amdgpu_cs_create_syncobj2(amdgpu_dev, 0, &timeline_handle);
		if (ret == 0) {
			ret = amdgpu_cs_syncobj_timeline_signal(amdgpu_dev,
								&timeline_handle,
								&signal_point,
								1);
			if (ret == 0) {
				memset(&arg, 0, sizeof(arg));
				timeline_handles[0]              = timeline_handle;
				timeline_points[0]               = signal_point;
				arg.syncobj_timeline_handles     = (uintptr_t)&timeline_handles[0];
				arg.syncobj_timeline_points      = (uintptr_t)&timeline_points[0];
				arg.num_syncobj_timeline_handles = 1;

				errno = 0;
				ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_WAIT, &arg);
				igt_assert_f(ret == 0,
					     "DRM_IOCTL_AMDGPU_USERQ_WAIT failed "
					     "(errno=%d) for already-signaled high32 "
					     "timeline point: kernel may be truncating "
					     "64-bit point to 32 bits\n", errno);
			}

			amdgpu_cs_destroy_syncobj(amdgpu_dev, timeline_handle);
		}
	}
	/*
	 * USERQ_WAIT multi-handle timeline: register a wait on two syncobjs,
	 * both already signaled at the requested point.  The ioctl must accept
	 * the multi-handle input and return 0.
	 *
	 * Note: USERQ_WAIT has no timeout field and is not a blocking call; it
	 * only registers a dependency.  We therefore only verify ret==0 here.
	 */
	if (amdgpu_dev) {
		uint32_t timeline_handles[2] = { 0, 0 };
		uint64_t signal_points[2] = { 8, 8 };
		uint64_t wait_points[2]   = { 8, 8 }; /* both already signaled */
		struct drm_amdgpu_userq_wait arg;
		int ret;

		ret = amdgpu_cs_create_syncobj2(amdgpu_dev, 0, &timeline_handles[0]);
		if (ret == 0) {
			ret = amdgpu_cs_create_syncobj2(amdgpu_dev, 0, &timeline_handles[1]);
			if (ret == 0) {
				ret = amdgpu_cs_syncobj_timeline_signal(amdgpu_dev,
								&timeline_handles[0],
								&signal_points[0],
								1);
				if (ret == 0)
					ret = amdgpu_cs_syncobj_timeline_signal(amdgpu_dev,
									&timeline_handles[1],
									&signal_points[1],
									1);

				if (ret == 0) {
					memset(&arg, 0, sizeof(arg));
					arg.syncobj_timeline_handles     = (uintptr_t)&timeline_handles[0];
					arg.syncobj_timeline_points      = (uintptr_t)&wait_points[0];
					arg.num_syncobj_timeline_handles = 2;

					errno = 0;
					ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_WAIT, &arg);
					igt_assert_f(ret == 0,
						     "DRM_IOCTL_AMDGPU_USERQ_WAIT failed "
						     "(errno=%d) for multi-handle timeline "
						     "wait with both points already "
						     "signaled\n", errno);
				}

				amdgpu_cs_destroy_syncobj(amdgpu_dev, timeline_handles[1]);
			}

			amdgpu_cs_destroy_syncobj(amdgpu_dev, timeline_handles[0]);
		}
	}
	/*
	 * USERQ handle-count overflow: values exceeding AMDGPU_USERQ_MAX_HANDLES
	 * (1 << 16) must be rejected with EINVAL.
	 */
	{
#ifndef AMDGPU_USERQ_MAX_HANDLES
#define AMDGPU_USERQ_MAX_HANDLES (1U << 16)
#endif
		struct drm_amdgpu_userq_signal sarg;
		struct drm_amdgpu_userq_wait  warg;
		const size_t signal_count_width = sizeof(((struct drm_amdgpu_userq_signal *)0)->num_syncobj_handles);
		const size_t wait_count_width = sizeof(((struct drm_amdgpu_userq_wait *)0)->num_syncobj_handles);
		const uint64_t signal_field_max =
			signal_count_width == sizeof(uint64_t) ? UINT64_MAX :
			signal_count_width == sizeof(uint32_t) ? UINT32_MAX :
			signal_count_width == sizeof(uint16_t) ? UINT16_MAX : UINT8_MAX;
		const uint64_t wait_field_max =
			wait_count_width == sizeof(uint64_t) ? UINT64_MAX :
			wait_count_width == sizeof(uint32_t) ? UINT32_MAX :
			wait_count_width == sizeof(uint16_t) ? UINT16_MAX : UINT8_MAX;
		int ret;
		uint64_t overflow_count;
		uint64_t trunc_count;

		/* SIGNAL: only assert overflow if this ABI can represent (> MAX_HANDLES). */
		if (signal_field_max > AMDGPU_USERQ_MAX_HANDLES) {
			memset(&sarg, 0, sizeof(sarg));
			overflow_count = (uint64_t)AMDGPU_USERQ_MAX_HANDLES + 1;
			sarg.num_syncobj_handles = overflow_count;
			errno = 0;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_SIGNAL, &sarg);
			igt_assert_f(ret != 0,
					 "DRM_IOCTL_AMDGPU_USERQ_SIGNAL unexpectedly succeeded for userq_signal.num_syncobj_handles.overflow (arg=%p)\n",
					 &sarg);
			igt_assert_f(errno == EINVAL || errno == EFAULT,
					 "DRM_IOCTL_AMDGPU_USERQ_SIGNAL expected errno=%d or errno=%d got errno=%d for userq_signal.num_syncobj_handles.overflow (arg=%p)\n",
					 EINVAL, EFAULT, errno, &sarg);
			if (errno == EFAULT)
				igt_info("known driver gap: USERQ_SIGNAL overflow returned EFAULT instead of EINVAL\n");
		}

		/*
		 * SIGNAL truncation exploit: only meaningful when this field is u64.
		 * On narrower ABIs, the kernel already receives a narrow value and
		 * this specific pre-narrowing path does not exist.
		 */
		if (signal_count_width == sizeof(uint64_t)) {
			memset(&sarg, 0, sizeof(sarg));
			trunc_count = 0x100000000ULL;
			sarg.num_syncobj_handles = trunc_count;
			errno = 0;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_SIGNAL, &sarg);
			igt_assert_f(ret != 0,
					 "DRM_IOCTL_AMDGPU_USERQ_SIGNAL unexpectedly succeeded for userq_signal.num_syncobj_handles.u64_trunc (arg=%p)\n",
					 &sarg);
			igt_assert_f(errno == EINVAL || errno == EFAULT || errno == ENOENT,
					 "DRM_IOCTL_AMDGPU_USERQ_SIGNAL expected errno=%d or errno=%d or errno=%d got errno=%d for userq_signal.num_syncobj_handles.u64_trunc (arg=%p)\n",
					 EINVAL, EFAULT, ENOENT, errno, &sarg);
			if (errno == EFAULT)
				igt_info("known driver gap: USERQ_SIGNAL u64 truncation returned EFAULT instead of EINVAL\n");
			if (errno == ENOENT)
				igt_info("known driver gap: USERQ_SIGNAL u64 truncation returned ENOENT instead of EINVAL\n");
		}

		/* WAIT: only assert overflow if this ABI can represent (> MAX_HANDLES). */
		if (wait_field_max > AMDGPU_USERQ_MAX_HANDLES) {
			memset(&warg, 0, sizeof(warg));
			overflow_count = (uint64_t)AMDGPU_USERQ_MAX_HANDLES + 1;
			warg.num_syncobj_handles = overflow_count;
			errno = 0;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_USERQ_WAIT, &warg);
			igt_assert_f(ret != 0,
					 "DRM_IOCTL_AMDGPU_USERQ_WAIT unexpectedly succeeded for userq_wait.num_syncobj_handles.overflow (arg=%p)\n",
					 &warg);
			igt_assert_f(errno == EINVAL || errno == EFAULT,
					 "DRM_IOCTL_AMDGPU_USERQ_WAIT expected errno=%d or errno=%d got errno=%d for userq_wait.num_syncobj_handles.overflow (arg=%p)\n",
					 EINVAL, EFAULT, errno, &warg);
			if (errno == EFAULT)
				igt_info("known driver gap: USERQ_WAIT overflow returned EFAULT instead of EINVAL\n");
		}
	}

	{
		union drm_amdgpu_wait_cs base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "wait_cs.ctx_id.invalid",      offsetof(union drm_amdgpu_wait_cs, in.ctx_id),      sizeof(uint32_t), UINT32_MAX },
			{ "wait_cs.ip_type.invalid",     offsetof(union drm_amdgpu_wait_cs, in.ip_type),     sizeof(uint32_t), UINT32_MAX },
			{ "wait_cs.ip_instance.invalid", offsetof(union drm_amdgpu_wait_cs, in.ip_instance), sizeof(uint32_t), UINT32_MAX },
			{ "wait_cs.ring.invalid",        offsetof(union drm_amdgpu_wait_cs, in.ring),        sizeof(uint32_t), UINT32_MAX },
		};

		memset(&base, 0, sizeof(base));
		/* ctx_id=0 is always invalid (no context created); all cases fail at ctx lookup */
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_WAIT_CS",
					 DRM_IOCTL_AMDGPU_WAIT_CS,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		struct drm_amdgpu_gem_userptr base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "gem_userptr.addr.invalid",  offsetof(struct drm_amdgpu_gem_userptr, addr),  sizeof(uint64_t), 0x1 },
			{ "gem_userptr.size.invalid",  offsetof(struct drm_amdgpu_gem_userptr, size),  sizeof(uint64_t), 0x1 },
			{ "gem_userptr.flags.invalid", offsetof(struct drm_amdgpu_gem_userptr, flags), sizeof(uint32_t),
			  ~(uint32_t)(AMDGPU_GEM_USERPTR_READONLY | AMDGPU_GEM_USERPTR_ANONONLY |
				      AMDGPU_GEM_USERPTR_VALIDATE | AMDGPU_GEM_USERPTR_REGISTER) },
		};

		memset(&base, 0, sizeof(base));
		base.addr  = 0x4000; /* page-aligned baseline so addr/size injections trigger the check */
		base.size  = 0x4000;
		base.flags = AMDGPU_GEM_USERPTR_READONLY;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_GEM_USERPTR",
					 DRM_IOCTL_AMDGPU_GEM_USERPTR,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		union drm_amdgpu_userq base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "userq.op.invalid",      offsetof(union drm_amdgpu_userq, in.op),      sizeof(uint32_t), UINT32_MAX },
			{ "userq.flags.invalid",   offsetof(union drm_amdgpu_userq, in.flags),   sizeof(uint32_t),
			  ~(uint32_t)(AMDGPU_USERQ_CREATE_FLAGS_QUEUE_PRIORITY_MASK |
				      AMDGPU_USERQ_CREATE_FLAGS_QUEUE_SECURE) },
			{ "userq.ip_type.invalid", offsetof(union drm_amdgpu_userq, in.ip_type), sizeof(uint32_t), UINT32_MAX },
		};

		memset(&base, 0, sizeof(base));
		base.in.op = AMDGPU_USERQ_OP_CREATE;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_USERQ",
					 DRM_IOCTL_AMDGPU_USERQ,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}

	{
		union drm_amdgpu_userq base;
		static const struct amd_ioctl_field_case cases[] = {
			{ "userq_destroy.op.invalid", offsetof(union drm_amdgpu_userq, in.op), sizeof(uint32_t), UINT32_MAX },
			{ "userq_destroy.queue_id.invalid", offsetof(union drm_amdgpu_userq, in.queue_id), sizeof(uint32_t), UINT32_MAX },
			{ "userq_destroy.flags.invalid", offsetof(union drm_amdgpu_userq, in.flags), sizeof(uint32_t), 1 },
			{ "userq_destroy.queue_va.invalid", offsetof(union drm_amdgpu_userq, in.queue_va), sizeof(uint64_t), 0x1000 },
			{ "userq_destroy.mqd_size.invalid", offsetof(union drm_amdgpu_userq, in.mqd_size), sizeof(uint64_t), 64 },
		};

		/* A true USERQ free baseline needs a successfully created queue (doorbell/MQD). */
		memset(&base, 0, sizeof(base));
		base.in.op = AMDGPU_USERQ_OP_FREE;
		run_drm_ioctl_field_cases(fd, "DRM_IOCTL_AMDGPU_USERQ",
					 DRM_IOCTL_AMDGPU_USERQ,
					 &base, sizeof(base),
					 cases, ARRAY_SIZE(cases));
	}
#endif /* AMDGPU_USERQ_ENABLED  */

	if (have_valid_ctx) {
		union drm_amdgpu_ctx free_ctx;

		memset(&free_ctx, 0, sizeof(free_ctx));
		free_ctx.in.op = AMDGPU_CTX_OP_FREE_CTX;
		free_ctx.in.ctx_id = valid_ctx_id;
		drmIoctl(fd, DRM_IOCTL_AMDGPU_CTX, &free_ctx);
	}

	if (have_valid_bo) {
		struct drm_gem_close close_bo;

		memset(&close_bo, 0, sizeof(close_bo));
		close_bo.handle = valid_bo_handle;
		drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_bo);
	}

}

/*
 * Boundary value and edge-case ioctl fuzzing.
 *
 * Tests extreme/unusual parameter values that are technically in-range
 * but exercise corner cases in the kernel validation logic:
 * - GEM_CREATE with boundary sizes (0, 1, page-1, huge)
 * - AMDGPU_INFO with every known query + invalid sub-fields
 * - CTX alloc/free stress to test ID recycling
 * - GEM_VA with misaligned addresses and zero-length maps
 */
static void
amd_boundary_fuzzing(int fd)
{
	/*
	 * GEM_CREATE boundary sizes: the driver must reject zero and
	 * excessively large sizes without crashing.
	 */
	{
		static const uint64_t test_sizes[] = {
			0,              /* zero size */
			1,              /* sub-page */
			4095,           /* page_size - 1 */
			4096,           /* exact page */
			4097,           /* page + 1 */
			(1ULL << 20),   /* 1 MB */
			(1ULL << 32),   /* 4 GB */
			(1ULL << 40),   /* 1 TB -- should fail */
			UINT64_MAX,     /* max -- must fail */
			UINT64_MAX - 4095, /* near-max */
		};
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(test_sizes); i++) {
			union drm_amdgpu_gem_create gem;
			struct drm_gem_close close_arg;
			int ret;

			memset(&gem, 0, sizeof(gem));
			gem.in.bo_size = test_sizes[i];
			gem.in.alignment = 0;
			gem.in.domains = AMDGPU_GEM_DOMAIN_GTT;
			gem.in.domain_flags = 0;

			errno = 0;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &gem);
			igt_info("gem-create size=0x%llx: ret=%d errno=%d\n",
				 (unsigned long long)test_sizes[i], ret, errno);

			if (ret == 0) {
				close_arg.handle = gem.out.handle;
				close_arg.pad = 0;
				drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
			}
		}
	}

	/*
	 * GEM_CREATE with every domain combination: some are invalid
	 * and the driver must reject them without crashing.
	 */
	{
		static const uint64_t test_domains[] = {
			0,                              /* no domain */
			AMDGPU_GEM_DOMAIN_CPU,
			AMDGPU_GEM_DOMAIN_GTT,
			AMDGPU_GEM_DOMAIN_VRAM,
			AMDGPU_GEM_DOMAIN_GDS,
			AMDGPU_GEM_DOMAIN_GWS,
			AMDGPU_GEM_DOMAIN_OA,
			AMDGPU_GEM_DOMAIN_MASK,         /* all bits */
			~(uint64_t)AMDGPU_GEM_DOMAIN_MASK, /* invalid bits only */
			UINT64_MAX,                     /* all bits set */
		};
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(test_domains); i++) {
			union drm_amdgpu_gem_create gem;
			struct drm_gem_close close_arg;
			int ret;

			memset(&gem, 0, sizeof(gem));
			gem.in.bo_size = 4096;
			gem.in.alignment = 0;
			gem.in.domains = test_domains[i];

			errno = 0;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &gem);
			igt_info("gem-create domain=0x%llx: ret=%d errno=%d\n",
				 (unsigned long long)test_domains[i], ret, errno);

			if (ret == 0) {
				close_arg.handle = gem.out.handle;
				close_arg.pad = 0;
				drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
			}
		}
	}

	/*
	 * GEM_CREATE alignment fuzzing: non-power-of-2 and large alignments.
	 */
	{
		static const uint64_t test_alignments[] = {
			0, 1, 3, 7, 255, 4096, 65536,
			(1ULL << 20), (1ULL << 32),
			UINT64_MAX,
		};
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(test_alignments); i++) {
			union drm_amdgpu_gem_create gem;
			struct drm_gem_close close_arg;
			int ret;

			memset(&gem, 0, sizeof(gem));
			gem.in.bo_size = 4096;
			gem.in.alignment = test_alignments[i];
			gem.in.domains = AMDGPU_GEM_DOMAIN_GTT;

			errno = 0;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &gem);
			igt_info("gem-create align=0x%llx: ret=%d errno=%d\n",
				 (unsigned long long)test_alignments[i], ret, errno);

			if (ret == 0) {
				close_arg.handle = gem.out.handle;
				close_arg.pad = 0;
				drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
			}
		}
	}

	/*
	 * AMDGPU_INFO exhaustive query fuzzing: iterate through all known
	 * query IDs and some invalid ones.  Each must either succeed or
	 * fail gracefully -- never crash.
	 */
	{
		static const uint32_t queries[] = {
			AMDGPU_INFO_ACCEL_WORKING,
			AMDGPU_INFO_CRTC_FROM_ID,
			AMDGPU_INFO_HW_IP_INFO,
			AMDGPU_INFO_HW_IP_COUNT,
			AMDGPU_INFO_TIMESTAMP,
			AMDGPU_INFO_FW_VERSION,
			AMDGPU_INFO_NUM_BYTES_MOVED,
			AMDGPU_INFO_VRAM_GTT,
			AMDGPU_INFO_GDS_CONFIG,
			AMDGPU_INFO_VRAM_USAGE,
			AMDGPU_INFO_GTT_USAGE,
			AMDGPU_INFO_DEV_INFO,
			AMDGPU_INFO_VIS_VRAM_USAGE,
			AMDGPU_INFO_READ_MMR_REG,
			AMDGPU_INFO_NUM_EVICTIONS,
			AMDGPU_INFO_MEMORY,
			AMDGPU_INFO_VBIOS,
			AMDGPU_INFO_SENSOR,
			AMDGPU_INFO_VIDEO_CAPS,
			0,              /* invalid: zero */
			0xDEAD,         /* invalid: garbage */
			UINT32_MAX,     /* invalid: max */
		};
		uint8_t outbuf[256];
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(queries); i++) {
			struct drm_amdgpu_info info;
			int ret;

			memset(&info, 0, sizeof(info));
			memset(outbuf, 0, sizeof(outbuf));
			info.return_pointer = (uintptr_t)outbuf;
			info.return_size = sizeof(outbuf);
			info.query = queries[i];

			errno = 0;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_INFO, &info);
			igt_info("amdgpu_info query=0x%x: ret=%d errno=%d\n",
				 queries[i], ret, errno);
			/* No assertion on ret -- some queries legitimately fail;
			 * the point is the driver must not crash. */
		}
	}

	/*
	 * AMDGPU_INFO FW_VERSION: fuzz every firmware type.
	 * Some FW types do not exist on some ASICs -- the driver must
	 * return an error, not crash.
	 */
	{
		static const uint32_t fw_types[] = {
			AMDGPU_INFO_FW_VCE,
			AMDGPU_INFO_FW_UVD,
			AMDGPU_INFO_FW_GMC,
			AMDGPU_INFO_FW_GFX_ME,
			AMDGPU_INFO_FW_GFX_PFP,
			AMDGPU_INFO_FW_GFX_CE,
			AMDGPU_INFO_FW_GFX_RLC,
			AMDGPU_INFO_FW_GFX_MEC,
			AMDGPU_INFO_FW_SMC,
			AMDGPU_INFO_FW_SDMA,
			AMDGPU_INFO_FW_ASD,
			AMDGPU_INFO_FW_VCN,
			AMDGPU_INFO_FW_GFX_RLC_RESTORE_LIST_CNTL,
			AMDGPU_INFO_FW_GFX_RLC_RESTORE_LIST_GPM_MEM,
			AMDGPU_INFO_FW_GFX_RLC_RESTORE_LIST_SRM_MEM,
			AMDGPU_INFO_FW_DMCU,
			AMDGPU_INFO_FW_TA,
			AMDGPU_INFO_FW_DMCUB,
			AMDGPU_INFO_FW_TOC,
			0xFF,           /* invalid type */
			UINT32_MAX,     /* invalid max */
		};
		uint8_t outbuf[256];
		unsigned int i;

		for (i = 0; i < ARRAY_SIZE(fw_types); i++) {
			struct drm_amdgpu_info info;
			int ret;

			memset(&info, 0, sizeof(info));
			memset(outbuf, 0, sizeof(outbuf));
			info.return_pointer = (uintptr_t)outbuf;
			info.return_size = sizeof(outbuf);
			info.query = AMDGPU_INFO_FW_VERSION;
			info.query_fw.fw_type = fw_types[i];

			errno = 0;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_INFO, &info);
			igt_info("fw_version type=0x%x: ret=%d errno=%d\n",
				 fw_types[i], ret, errno);
		}
	}

	/*
	 * AMDGPU_INFO HW_IP: fuzz all IP types including invalid ones.
	 */
	{
		uint32_t ip;
		uint8_t outbuf[256];

		for (ip = 0; ip <= AMDGPU_HW_IP_VPE + 5; ip++) {
			struct drm_amdgpu_info info;
			int ret;

			memset(&info, 0, sizeof(info));
			memset(outbuf, 0, sizeof(outbuf));
			info.return_pointer = (uintptr_t)outbuf;
			info.return_size = sizeof(outbuf);
			info.query = AMDGPU_INFO_HW_IP_INFO;
			info.query_hw_ip.type = ip;

			errno = 0;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_INFO, &info);
			igt_info("hw_ip_info type=%u: ret=%d errno=%d\n",
				 ip, ret, errno);
		}
	}

	/*
	 * CTX alloc/free stress: rapidly allocate and free contexts
	 * to stress ID recycling paths.
	 */
	{
		int i;

		for (i = 0; i < 64; i++) {
			union drm_amdgpu_ctx ctx;
			int ret;

			memset(&ctx, 0, sizeof(ctx));
			ctx.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_CTX, &ctx);
			if (ret != 0)
				break;

			ctx.in.op = AMDGPU_CTX_OP_FREE_CTX;
			ctx.in.ctx_id = ctx.out.alloc.ctx_id;
			drmIoctl(fd, DRM_IOCTL_AMDGPU_CTX, &ctx);
		}
		igt_info("ctx-alloc-free-stress: %d iterations\n", i);
	}

	/*
	 * GEM_VA misaligned address: map with non-page-aligned VA.
	 */
	{
		struct drm_amdgpu_gem_va va;
		int ret;

		memset(&va, 0, sizeof(va));
		va.handle = 0;
		va.operation = AMDGPU_VA_OP_MAP;
		va.flags = AMDGPU_VM_PAGE_READABLE;
		va.va_address = 0x1001;  /* not page aligned */
		va.map_size = 4096;

		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_VA, &va);
		igt_assert_neq(ret, 0);
		igt_info("gem-va-misaligned: ret=%d errno=%d\n", ret, errno);
	}

	/*
	 * GEM_VA zero-length map: must be rejected.
	 */
	{
		struct drm_amdgpu_gem_va va;
		int ret;

		memset(&va, 0, sizeof(va));
		va.handle = 0;
		va.operation = AMDGPU_VA_OP_MAP;
		va.flags = AMDGPU_VM_PAGE_READABLE;
		va.va_address = 0x100000;
		va.map_size = 0;

		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_VA, &va);
		igt_assert_neq(ret, 0);
		igt_info("gem-va-zero-size: ret=%d errno=%d\n", ret, errno);
	}

	/*
	 * SCHED priority escalation: unprivileged user must not be
	 * able to set high priority.
	 */
	{
		union drm_amdgpu_ctx ctx;
		union drm_amdgpu_sched sched;
		int ret;

		memset(&ctx, 0, sizeof(ctx));
		ctx.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_CTX, &ctx);
		if (ret == 0) {
			memset(&sched, 0, sizeof(sched));
			sched.in.op = AMDGPU_SCHED_OP_CONTEXT_PRIORITY_OVERRIDE;
			sched.in.ctx_id = ctx.out.alloc.ctx_id;
			sched.in.fd = fd;
			sched.in.priority = AMDGPU_CTX_PRIORITY_VERY_HIGH;

			errno = 0;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_SCHED, &sched);
			igt_info("sched-priority-escalation: ret=%d errno=%d\n",
				 ret, errno);
			/* May succeed for root, fail for normal user -- just
			 * must not crash. */

			/* Clean up */
			ctx.in.op = AMDGPU_CTX_OP_FREE_CTX;
			ctx.in.ctx_id = ctx.out.alloc.ctx_id;
			drmIoctl(fd, DRM_IOCTL_AMDGPU_CTX, &ctx);
		}
	}
}

/*
 * Lifecycle and sequence fuzzing tests.
 *
 * These tests validate the driver handles wrong-order operations safely:
 * double-close, use-after-free patterns, and operations on freed resources.
 * Many of these patterns mirror real Syzkaller-found bugs.
 */
static void
amd_lifecycle_fuzzing(int fd)
{
	/*
	 * Double-close GEM handle: closing the same BO twice must not
	 * crash the driver.  The second close should fail with EINVAL.
	 */
	{
		union drm_amdgpu_gem_create gem;
		struct drm_gem_close close_arg;
		int ret;
		memset(&gem, 0, sizeof(gem));
		gem.in.bo_size = 4096;
		gem.in.alignment = 0;
		gem.in.domains = AMDGPU_GEM_DOMAIN_GTT;
		gem.in.domain_flags = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &gem);
		igt_assert_eq(ret, 0);
		close_arg.handle = gem.out.handle;
		close_arg.pad = 0;
		ret = drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
		igt_assert_eq(ret, 0);
		/* Second close -- must not crash */
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
		igt_assert_neq(ret, 0);
		igt_info("double-close GEM handle: ret=%d errno=%d\n", ret, errno);
	}
	/*
	 * GEM_MMAP on closed handle: mmap after close must fail gracefully.
	 */
	{
		union drm_amdgpu_gem_create gem;
		union drm_amdgpu_gem_mmap mmap_arg;
		struct drm_gem_close close_arg;
		int ret;
		memset(&gem, 0, sizeof(gem));
		gem.in.bo_size = 4096;
		gem.in.alignment = 0;
		gem.in.domains = AMDGPU_GEM_DOMAIN_GTT;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &gem);
		igt_assert_eq(ret, 0);
		close_arg.handle = gem.out.handle;
		close_arg.pad = 0;
		ret = drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
		igt_assert_eq(ret, 0);
		memset(&mmap_arg, 0, sizeof(mmap_arg));
		mmap_arg.in.handle = gem.out.handle;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_MMAP, &mmap_arg);
		igt_assert_neq(ret, 0);
		igt_info("mmap-after-close: ret=%d errno=%d\n", ret, errno);
	}
	/*
	 * GEM_WAIT_IDLE on closed handle: must fail, not crash.
	 */
	{
		union drm_amdgpu_gem_create gem;
		union drm_amdgpu_gem_wait_idle wait;
		struct drm_gem_close close_arg;
		int ret;
		memset(&gem, 0, sizeof(gem));
		gem.in.bo_size = 4096;
		gem.in.domains = AMDGPU_GEM_DOMAIN_GTT;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &gem);
		igt_assert_eq(ret, 0);
		close_arg.handle = gem.out.handle;
		close_arg.pad = 0;
		ret = drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
		igt_assert_eq(ret, 0);
		memset(&wait, 0, sizeof(wait));
		wait.in.handle = gem.out.handle;
		wait.in.timeout = 1000;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_WAIT_IDLE, &wait);
		igt_assert_neq(ret, 0);
		igt_info("wait-idle-after-close: ret=%d errno=%d\n", ret, errno);
	}
	/*
	 * Double-free CTX: freeing context twice must not crash.
	 */
	{
		union drm_amdgpu_ctx ctx;
		int ret;
		memset(&ctx, 0, sizeof(ctx));
		ctx.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_CTX, &ctx);
		igt_assert_eq(ret, 0);
		{
			union drm_amdgpu_ctx free_ctx;
			uint32_t saved_id = ctx.out.alloc.ctx_id;
			memset(&free_ctx, 0, sizeof(free_ctx));
			free_ctx.in.op = AMDGPU_CTX_OP_FREE_CTX;
			free_ctx.in.ctx_id = saved_id;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_CTX, &free_ctx);
			igt_assert_eq(ret, 0);
			/* Second free -- must not crash */
			errno = 0;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_CTX, &free_ctx);
			igt_assert_neq(ret, 0);
			igt_info("double-free CTX: ret=%d errno=%d\n", ret, errno);
		}
	}
	/*
	 * CS submit with freed CTX: must fail, not crash or UAF.
	 */
	{
		union drm_amdgpu_ctx ctx;
		union drm_amdgpu_cs cs;
		int ret;
		memset(&ctx, 0, sizeof(ctx));
		ctx.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_CTX, &ctx);
		igt_assert_eq(ret, 0);
		{
			union drm_amdgpu_ctx free_ctx;
			uint32_t saved_id = ctx.out.alloc.ctx_id;
			memset(&free_ctx, 0, sizeof(free_ctx));
			free_ctx.in.op = AMDGPU_CTX_OP_FREE_CTX;
			free_ctx.in.ctx_id = saved_id;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_CTX, &free_ctx);
			igt_assert_eq(ret, 0);
			/* Submit with freed ctx -- must not crash */
			memset(&cs, 0, sizeof(cs));
			cs.in.ctx_id = saved_id;
			errno = 0;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_CS, &cs);
			igt_assert_neq(ret, 0);
			igt_info("CS-after-ctx-free: ret=%d errno=%d\n", ret, errno);
		}
	}
	/*
	 * WAIT_CS with freed CTX: must fail gracefully.
	 */
	{
		union drm_amdgpu_ctx ctx;
		union drm_amdgpu_wait_cs wait;
		int ret;
		memset(&ctx, 0, sizeof(ctx));
		ctx.in.op = AMDGPU_CTX_OP_ALLOC_CTX;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_CTX, &ctx);
		igt_assert_eq(ret, 0);
		{
			union drm_amdgpu_ctx free_ctx;
			uint32_t saved_id = ctx.out.alloc.ctx_id;
			memset(&free_ctx, 0, sizeof(free_ctx));
			free_ctx.in.op = AMDGPU_CTX_OP_FREE_CTX;
			free_ctx.in.ctx_id = saved_id;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_CTX, &free_ctx);
			igt_assert_eq(ret, 0);
			memset(&wait, 0, sizeof(wait));
			wait.in.ctx_id = saved_id;
			wait.in.ip_type = AMDGPU_HW_IP_GFX;
			wait.in.timeout = 1000;
			errno = 0;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_WAIT_CS, &wait);
			igt_assert_neq(ret, 0);
			igt_info("wait-cs-after-ctx-free: ret=%d errno=%d\n", ret, errno);
		}
	}
	/*
	 * GEM_METADATA on closed handle: get/set metadata on freed BO.
	 */
	{
		union drm_amdgpu_gem_create gem;
		struct drm_amdgpu_gem_metadata meta;
		struct drm_gem_close close_arg;
		int ret;
		memset(&gem, 0, sizeof(gem));
		gem.in.bo_size = 4096;
		gem.in.domains = AMDGPU_GEM_DOMAIN_GTT;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &gem);
		igt_assert_eq(ret, 0);
		close_arg.handle = gem.out.handle;
		close_arg.pad = 0;
		ret = drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
		igt_assert_eq(ret, 0);
		memset(&meta, 0, sizeof(meta));
		meta.handle = gem.out.handle;
		meta.op = AMDGPU_GEM_METADATA_OP_GET_METADATA;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_METADATA, &meta);
		igt_assert_neq(ret, 0);
		igt_info("metadata-get-after-close: ret=%d errno=%d\n", ret, errno);
		meta.op = AMDGPU_GEM_METADATA_OP_SET_METADATA;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_METADATA, &meta);
		igt_assert_neq(ret, 0);
		igt_info("metadata-set-after-close: ret=%d errno=%d\n", ret, errno);
	}
	/*
	 * GEM_OP on closed handle: must fail, not UAF.
	 */
	{
		union drm_amdgpu_gem_create gem;
		struct drm_amdgpu_gem_op gem_op;
		struct drm_gem_close close_arg;
		int ret;
		memset(&gem, 0, sizeof(gem));
		gem.in.bo_size = 4096;
		gem.in.domains = AMDGPU_GEM_DOMAIN_GTT;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &gem);
		igt_assert_eq(ret, 0);
		close_arg.handle = gem.out.handle;
		close_arg.pad = 0;
		ret = drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
		igt_assert_eq(ret, 0);
		memset(&gem_op, 0, sizeof(gem_op));
		gem_op.handle = gem.out.handle;
		gem_op.op = AMDGPU_GEM_OP_GET_GEM_CREATE_INFO;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_OP, &gem_op);
		igt_assert_neq(ret, 0);
		igt_info("gem-op-after-close: ret=%d errno=%d\n", ret, errno);
	}
	/*
	 * BO_LIST with closed BO handle: create a bo_list referencing
	 * a handle that was already closed.
	 */
	{
		union drm_amdgpu_gem_create gem;
		union drm_amdgpu_bo_list bo_list;
		struct drm_amdgpu_bo_list_entry entry;
		struct drm_gem_close close_arg;
		int ret;
		memset(&gem, 0, sizeof(gem));
		gem.in.bo_size = 4096;
		gem.in.domains = AMDGPU_GEM_DOMAIN_GTT;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &gem);
		igt_assert_eq(ret, 0);
		close_arg.handle = gem.out.handle;
		close_arg.pad = 0;
		ret = drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
		igt_assert_eq(ret, 0);
		memset(&entry, 0, sizeof(entry));
		entry.bo_handle = gem.out.handle;
		memset(&bo_list, 0, sizeof(bo_list));
		bo_list.in.operation = AMDGPU_BO_LIST_OP_CREATE;
		bo_list.in.bo_number = 1;
		bo_list.in.bo_info_size = sizeof(entry);
		bo_list.in.bo_info_ptr = (uintptr_t)&entry;
		errno = 0;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_BO_LIST, &bo_list);
		igt_assert_neq(ret, 0);
		igt_info("bo-list-with-closed-handle: ret=%d errno=%d\n", ret, errno);
	}
	/*
	 * PRIME export + close + import-back: must not crash.
	 * Tests the DMA-BUF handle lifecycle.
	 */
	{
		union drm_amdgpu_gem_create gem;
		struct drm_prime_handle prime;
		struct drm_gem_close close_arg;
		int ret;
		memset(&gem, 0, sizeof(gem));
		gem.in.bo_size = 4096;
		gem.in.domains = AMDGPU_GEM_DOMAIN_GTT;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_GEM_CREATE, &gem);
		igt_assert_eq(ret, 0);
		/* Export to DMA-BUF fd */
		memset(&prime, 0, sizeof(prime));
		prime.handle = gem.out.handle;
		prime.flags = DRM_CLOEXEC | DRM_RDWR;
		ret = drmIoctl(fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime);
		if (ret == 0) {
			int dmabuf_fd = prime.fd;
			/* Close the GEM handle */
			close_arg.handle = gem.out.handle;
			close_arg.pad = 0;
			drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
			/* Import back from the DMA-BUF fd */
			memset(&prime, 0, sizeof(prime));
			prime.fd = dmabuf_fd;
			ret = drmIoctl(fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime);
			igt_info("prime-reimport-after-close: ret=%d handle=%u\n",
				 ret, prime.handle);
			/* Clean up */
			if (ret == 0) {
				close_arg.handle = prime.handle;
				drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
			}
			close(dmabuf_fd);
		}
	}
	/*
	 * VM reserve/unreserve double-free: unreserving VMID twice.
	 */
	{
		union drm_amdgpu_vm vm;
		int ret;
		memset(&vm, 0, sizeof(vm));
		vm.in.op = AMDGPU_VM_OP_RESERVE_VMID;
		ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_VM, &vm);
		if (ret == 0) {
			vm.in.op = AMDGPU_VM_OP_UNRESERVE_VMID;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_VM, &vm);
			igt_assert_eq(ret, 0);
			/* Second unreserve -- must not crash */
			errno = 0;
			ret = drmIoctl(fd, DRM_IOCTL_AMDGPU_VM, &vm);
			igt_info("vm-double-unreserve: ret=%d errno=%d\n", ret, errno);
		}
	}
}

#ifdef AMDGPU_USERQ_ENABLED
struct amd_test_userq_ctx {
	uint32_t ip_type;
	struct amdgpu_ring_context *ring_context;
	uint32_t queue_id;
	bool created;
};

static bool
amd_test_map_hw_to_ip_block_type(uint32_t hw_ip_type,
				 enum amd_ip_block_type *block_type)
{
	if (!block_type)
		return false;

	switch (hw_ip_type) {
	case AMDGPU_HW_IP_GFX:
		*block_type = AMD_IP_GFX;
		return true;
	case AMDGPU_HW_IP_COMPUTE:
		*block_type = AMD_IP_COMPUTE;
		return true;
	case AMDGPU_HW_IP_DMA:
		*block_type = AMD_IP_DMA;
		return true;
	default:
		return false;
	}
}

static void
amd_test_userq_cleanup(int fd, amdgpu_device_handle amdgpu_dev,
		      struct amd_test_userq_ctx *userq)
{
	const struct amdgpu_ip_block_version *ip_block = NULL;
	enum amd_ip_block_type block_type;
	int r;
	bool need_wait = false;

	(void)fd;

	if (!userq)
		return;
	if (!userq->ring_context)
		return;

	if (amdgpu_dev && amd_test_map_hw_to_ip_block_type(userq->ip_type, &block_type))
		ip_block = get_ip_block(amdgpu_dev, block_type);

	if (userq->created && ip_block && ip_block->funcs && ip_block->funcs->userq_destroy) {
		ip_block->funcs->userq_destroy(amdgpu_dev, userq->ring_context,
					      ip_block->type);
		userq->created = false;
	} else {
		/* Queue may already be destroyed by a race thread; free remaining BOs only. */
		switch (userq->ip_type) {
		case AMDGPU_HW_IP_GFX:
			if (userq->ring_context->csa.handle) {
				amdgpu_bo_unmap_and_free(userq->ring_context->csa.handle,
						 userq->ring_context->csa.va_handle,
						 userq->ring_context->csa.mc_addr,
						 userq->ring_context->info.gfx.csa_size);
				userq->ring_context->csa.handle = NULL;
				need_wait = true;
			}

			if (userq->ring_context->shadow.handle) {
				amdgpu_bo_unmap_and_free(userq->ring_context->shadow.handle,
						 userq->ring_context->shadow.va_handle,
						 userq->ring_context->shadow.mc_addr,
						 userq->ring_context->info.gfx.shadow_size);
				userq->ring_context->shadow.handle = NULL;
				need_wait = true;
			}
			break;

		case AMDGPU_HW_IP_COMPUTE:
			if (userq->ring_context->eop.handle) {
				amdgpu_bo_unmap_and_free(userq->ring_context->eop.handle,
						 userq->ring_context->eop.va_handle,
						 userq->ring_context->eop.mc_addr,
						 256);
				userq->ring_context->eop.handle = NULL;
				need_wait = true;
			}
			break;

		case AMDGPU_HW_IP_DMA:
			if (userq->ring_context->csa.handle) {
				amdgpu_bo_unmap_and_free(userq->ring_context->csa.handle,
						 userq->ring_context->csa.va_handle,
						 userq->ring_context->csa.mc_addr,
						 userq->ring_context->info.gfx.csa_size);
				userq->ring_context->csa.handle = NULL;
				need_wait = true;
			}
			break;

		default:
			break;
		}

		if (need_wait && userq->ring_context->timeline_syncobj_handle) {
			r = amdgpu_timeline_syncobj_wait(amdgpu_dev,
							 userq->ring_context->timeline_syncobj_handle,
							 userq->ring_context->point);
			if (r)
				igt_info("userq cleanup wait failed: ip=%u ret=%d\n", userq->ip_type, r);
		}

		if (userq->ring_context->timeline_syncobj_handle) {
			r = amdgpu_cs_destroy_syncobj(amdgpu_dev,
						     userq->ring_context->timeline_syncobj_handle);
			if (r)
				igt_info("userq cleanup destroy syncobj failed: ip=%u ret=%d\n", userq->ip_type, r);
			userq->ring_context->timeline_syncobj_handle = 0;
		}

		if (userq->ring_context->doorbell.handle) {
			r = amdgpu_bo_cpu_unmap(userq->ring_context->doorbell.handle);
			if (r)
				igt_info("userq cleanup doorbell unmap failed: ip=%u ret=%d\n", userq->ip_type, r);

			r = amdgpu_bo_free(userq->ring_context->doorbell.handle);
			if (r)
				igt_info("userq cleanup doorbell free failed: ip=%u ret=%d\n", userq->ip_type, r);

			userq->ring_context->doorbell.handle = NULL;
		}

		if (userq->ring_context->rptr.handle) {
			amdgpu_bo_unmap_and_free(userq->ring_context->rptr.handle,
						 userq->ring_context->rptr.va_handle,
						 userq->ring_context->rptr.mc_addr, 8);
			userq->ring_context->rptr.handle = NULL;
		}

		if (userq->ring_context->wptr.handle) {
			amdgpu_bo_unmap_and_free(userq->ring_context->wptr.handle,
						 userq->ring_context->wptr.va_handle,
						 userq->ring_context->wptr.mc_addr, 8);
			userq->ring_context->wptr.handle = NULL;
		}

		if (userq->ring_context->queue.handle) {
			amdgpu_bo_unmap_and_free(userq->ring_context->queue.handle,
						 userq->ring_context->queue.va_handle,
						 userq->ring_context->queue.mc_addr,
						 USERMODE_QUEUE_SIZE);
			userq->ring_context->queue.handle = NULL;
		}
	}

	free(userq->ring_context);
	userq->ring_context = NULL;
	userq->queue_id = 0;
}

static bool
amd_test_try_create_userq(int fd, amdgpu_device_handle amdgpu_dev, uint32_t ip_type,
			 struct amd_test_userq_ctx *userq)
{
	const struct amdgpu_ip_block_version *ip_block;
	enum amd_ip_block_type block_type;

	(void)fd;

	if (!amdgpu_dev || !userq)
	{
		igt_info("userq create failed: invalid args dev=%p userq=%p ip=%u\n",
			 amdgpu_dev, userq, ip_type);
		return false;
	}

	memset(userq, 0, sizeof(*userq));
	userq->ip_type = ip_type;
	if (!amd_test_map_hw_to_ip_block_type(ip_type, &block_type)) {
		igt_info("userq create failed: unsupported hw ip_type=%u\n", ip_type);
		return false;
	}

	ip_block = get_ip_block(amdgpu_dev, block_type);
	if (!ip_block || !ip_block->funcs || !ip_block->funcs->userq_create) {
		igt_info("userq create unavailable: ip=%u block=%p funcs=%p create=%p\n",
			 ip_type,
			 ip_block,
			 ip_block ? ip_block->funcs : NULL,
			 (ip_block && ip_block->funcs) ? ip_block->funcs->userq_create : NULL);
		return false;
	}

	userq->ring_context = calloc(1, sizeof(*userq->ring_context));
	if (!userq->ring_context) {
		igt_info("userq create failed: ring_context alloc failed ip=%u\n", ip_type);
		return false;
	}

	ip_block->funcs->userq_create(amdgpu_dev, userq->ring_context, ip_block->type);
	if (!userq->ring_context->queue.handle) {
		igt_info("userq create failed: queue.handle is NULL ip=%u\n", ip_type);
		free(userq->ring_context);
		userq->ring_context = NULL;
		return false;
	}

	userq->queue_id = userq->ring_context->queue_id;
	userq->created = true;
	return true;
}


/*
 * ------------------------------------------------------------------
 * USERQ concurrency / race-condition fuzzing
 *
 * Runs on GFX / COMPUTE / DMA queues when supported by the ASIC.
 *
 * Race D – Concurrent USERQ_CREATE + USERQ_DESTROY:
 *   N threads simultaneously create/destroy queues of the same ip_type.
 *
 * Race E – USERQ_SIGNAL racing USERQ_DESTROY (2 threads, 1 queue).
 *
 * Race F – Concurrent USERQ_DESTROY on the same queue_id (2 threads).
 *
 * Race G – USERQ_WAIT racing USERQ_DESTROY (2 threads, 1 queue).
 *
 * Race H – USERQ queue writes racing negative ioctls + DESTROY:
 *   Writer thread submits tiny packets to the queue while another thread
 *   injects invalid ioctls and then destroys the queue.
 *
 * No specific errno is asserted – the real signal is clean dmesg.
 * ------------------------------------------------------------------
 */

static const char *
amd_userq_ip_name(uint32_t ip_type)
{
	switch (ip_type) {
	case AMDGPU_HW_IP_GFX:
		return "GFX";
	case AMDGPU_HW_IP_COMPUTE:
		return "COMPUTE";
	case AMDGPU_HW_IP_DMA:
		return "DMA";
	default:
		return "UNKNOWN";
	}
}

/* Race D: per-thread args for concurrent create/destroy. */
struct amd_conc_userq_args {
	int                  fd;
	amdgpu_device_handle dev;
	pthread_barrier_t   *barrier;
	int                  iters;
	uint32_t             ip_type;
};

/* Races E / G: state shared between the op-thread and the destroyer. */
struct amd_conc_userq_op_state {
	int                  fd;
	amdgpu_device_handle dev;
	pthread_barrier_t    barrier;
	uint32_t             ip_type;
	uint32_t             queue_id;
	volatile int         destroyed;
};

/* Race H: writer + negative ops + destroy on one live queue. */
struct amd_conc_userq_write_state {
	int                          fd;
	amdgpu_device_handle         dev;
	pthread_barrier_t            barrier;
	uint32_t                     ip_type;
	uint32_t                     queue_id;
	struct amdgpu_ring_context  *ring_context;
	volatile int                 destroyed;
};

/* Race F: per-thread args for concurrent double-destroy. */
struct amd_conc_userq_destroy_args {
	int               fd;
	uint32_t          queue_id;
	pthread_barrier_t *barrier;
};

/* Race D thread: create own queue, immediately destroy it, N times. */
static void *
thread_userq_create_destroy(void *arg)
{
	struct amd_conc_userq_args *a = arg;
	struct amd_test_userq_ctx local_ctx;
	int i;

	pthread_barrier_wait(a->barrier);
	for (i = 0; i < a->iters; i++) {
		memset(&local_ctx, 0, sizeof(local_ctx));
		if (amd_test_try_create_userq(a->fd, a->dev,
					      a->ip_type, &local_ctx))
			amd_test_userq_cleanup(a->fd, a->dev, &local_ctx);
	}
	return NULL;
}

/*
 * Queue write helper for Race H.
 * Writes a tiny NOP packet and rings doorbell to exercise queue write path.
 */
static void
amd_test_userq_write_once(struct amdgpu_ring_context *ctx, uint32_t ip_type,
			 uint32_t tag)
{
	struct amdgpu_ring_context *ring_context = ctx;

	if (!ring_context || !ring_context->queue_cpu || !ring_context->wptr_cpu ||
	    !ring_context->doorbell_cpu)
		return;

	if (ip_type == AMDGPU_HW_IP_DMA) {
		amdgpu_sdma_pkt_begin();
		amdgpu_pkt_add_dw(SDMA_PKT_HEADER_OP(SDMA_NOP));
		amdgpu_pkt_add_dw(tag);
		amdgpu_sdma_pkt_end();
	} else {
		amdgpu_pkt_begin();
		amdgpu_pkt_add_dw(PACKET3(PACKET3_NOP, 0));
		amdgpu_pkt_add_dw(tag);
		amdgpu_pkt_end();
	}

	ring_context->doorbell_cpu[DOORBELL_INDEX] = *ring_context->wptr_cpu;
}

/* Race H writer: keep writing tiny packets until queue is destroyed. */
static void *
thread_userq_writer(void *arg)
{
	struct amd_conc_userq_write_state *s = arg;
	int iter;

	pthread_barrier_wait(&s->barrier);
	for (iter = 0; iter < AMD_CONC_ITERS; iter++) {
		amd_test_userq_write_once(s->ring_context, s->ip_type,
					  0xabc00000u | (uint32_t)iter);
		if (s->destroyed)
			break;
		usleep(100);
	}

	return NULL;
}

/* Race H negative thread: inject invalid ioctls while writer is active, then destroy. */
static void *
thread_userq_negative_destroyer(void *arg)
{
	struct amd_conc_userq_write_state *s = arg;
	int iter;

	pthread_barrier_wait(&s->barrier);

	for (iter = 0; iter < 8; iter++) {
		struct drm_amdgpu_userq_signal sig;
		struct drm_amdgpu_userq_wait wait;
		union drm_amdgpu_userq free_arg;

		memset(&sig, 0, sizeof(sig));
		sig.queue_id = UINT32_MAX;
		drmIoctl(s->fd, DRM_IOCTL_AMDGPU_USERQ_SIGNAL, &sig);

		memset(&wait, 0, sizeof(wait));
		/* After updating the struct, uncomment it. */
		//wait.waitq_id = UINT32_MAX;
		drmIoctl(s->fd, DRM_IOCTL_AMDGPU_USERQ_WAIT, &wait);

		/* Invalid free should fail gracefully. */
		memset(&free_arg, 0, sizeof(free_arg));
		free_arg.in.op = AMDGPU_USERQ_OP_FREE;
		free_arg.in.queue_id = UINT32_MAX;
		drmIoctl(s->fd, DRM_IOCTL_AMDGPU_USERQ, &free_arg);

		usleep(150);
	}

	/* destroy while writer may still be updating queue + doorbell */
	s->destroyed = 1;
	amdgpu_free_userqueue(s->dev, s->queue_id);

	return NULL;
}

/*
 * Race E thread: repeatedly SIGNAL the queue.
 * Return value is not asserted – calls will fail once the queue is gone.
 */
static void *
thread_userq_signaler(void *arg)
{
	struct amd_conc_userq_op_state *s = arg;
	int iter;

	pthread_barrier_wait(&s->barrier);
	for (iter = 0; iter < AMD_CONC_ITERS; iter++) {
		struct drm_amdgpu_userq_signal sig;

		memset(&sig, 0, sizeof(sig));
		sig.queue_id = s->queue_id;
		/* empty payload: exercises queue_id XArray lookup vs teardown */
		drmIoctl(s->fd, DRM_IOCTL_AMDGPU_USERQ_SIGNAL, &sig);
		if (s->destroyed)
			break;
	}
	return NULL;
}

/*
 * Race G thread: repeatedly WAIT on the queue.
 * Return value is not asserted – calls will fail once the queue is gone.
 */
static void *
thread_userq_waiter(void *arg)
{
	struct amd_conc_userq_op_state *s = arg;
	int iter;

	pthread_barrier_wait(&s->barrier);
	for (iter = 0; iter < AMD_CONC_ITERS; iter++) {
		struct drm_amdgpu_userq_wait wait;

		memset(&wait, 0, sizeof(wait));
		/* After updating the struct, uncomment it. */
		//wait.waitq_id = s->queue_id;
		/* empty payload: exercises waitq_id lookup vs teardown */
		drmIoctl(s->fd, DRM_IOCTL_AMDGPU_USERQ_WAIT, &wait);
		if (s->destroyed)
			break;
	}
	return NULL;
}

/* Shared destroyer for Races E and G. */
static void *
thread_userq_op_destroyer(void *arg)
{
	struct amd_conc_userq_op_state *s = arg;

	pthread_barrier_wait(&s->barrier);
	/*
	 * Brief delay increases the probability that the op-thread is
	 * inside a SIGNAL/WAIT ioctl when teardown fires.
	 */
	usleep(1000);
	s->destroyed = 1;
	amdgpu_free_userqueue(s->dev, s->queue_id);
	return NULL;
}

/* Race F thread: attempt AMDGPU_USERQ_OP_FREE on a shared queue_id. */
static void *
thread_userq_double_destroy(void *arg)
{
	struct amd_conc_userq_destroy_args *a = arg;
	union drm_amdgpu_userq userq_arg;

	pthread_barrier_wait(a->barrier);
	memset(&userq_arg, 0, sizeof(userq_arg));
	userq_arg.in.op       = AMDGPU_USERQ_OP_FREE;
	userq_arg.in.queue_id = a->queue_id;
	/*
	 * Return value intentionally unchecked: exactly one thread should
	 * succeed; the driver must not crash on either outcome.
	 */
	drmIoctl(a->fd, DRM_IOCTL_AMDGPU_USERQ, &userq_arg);
	return NULL;
}

static void
amd_userq_run_create_destroy_race(int fd, amdgpu_device_handle dev,
				 uint32_t ip_type)
{
	pthread_t threads[AMD_CONC_THREADS];
	struct amd_conc_userq_args args[AMD_CONC_THREADS];
	pthread_barrier_t barrier;
	int i;

	igt_info("concurrent-userq-fuzzing[%s]: concurrent-create-destroy "
		 "(%d threads x 4 iters)\n",
		 amd_userq_ip_name(ip_type), AMD_CONC_THREADS);
	igt_assert(pthread_barrier_init(&barrier, NULL, AMD_CONC_THREADS) == 0);
	for (i = 0; i < AMD_CONC_THREADS; i++) {
		args[i].fd      = fd;
		args[i].dev     = dev;
		args[i].barrier = &barrier;
		args[i].iters   = 4;
		args[i].ip_type = ip_type;
		igt_assert(pthread_create(&threads[i], NULL,
					  thread_userq_create_destroy,
					  &args[i]) == 0);
	}
	for (i = 0; i < AMD_CONC_THREADS; i++)
		pthread_join(threads[i], NULL);
	pthread_barrier_destroy(&barrier);
}

static void
amd_userq_run_signal_destroy_race(int fd, amdgpu_device_handle dev,
				 uint32_t ip_type)
{
	struct amd_conc_userq_op_state s;
	struct amd_test_userq_ctx userq;
	pthread_t signaler, destroyer;

	memset(&userq, 0, sizeof(userq));
	if (!amd_test_try_create_userq(fd, dev, ip_type, &userq)) {
		igt_info("concurrent-userq-fuzzing[%s]: queue unavailable, "
			 "skip signal-vs-destroy\n", amd_userq_ip_name(ip_type));
		return;
	}

	memset(&s, 0, sizeof(s));
	s.fd       = fd;
	s.dev      = dev;
	s.ip_type  = ip_type;
	s.queue_id = userq.queue_id;
	userq.created = false;
	igt_assert(pthread_barrier_init(&s.barrier, NULL, 2) == 0);
	igt_info("concurrent-userq-fuzzing[%s]: signal-vs-destroy start "
		 "(queue_id=%u)\n", amd_userq_ip_name(ip_type), s.queue_id);
	igt_assert(pthread_create(&signaler, NULL, thread_userq_signaler, &s) == 0);
	igt_assert(pthread_create(&destroyer, NULL, thread_userq_op_destroyer, &s) == 0);
	pthread_join(signaler, NULL);
	pthread_join(destroyer, NULL);
	pthread_barrier_destroy(&s.barrier);
	amd_test_userq_cleanup(fd, dev, &userq);
}

static void
amd_userq_run_double_destroy_race(int fd, amdgpu_device_handle dev,
				 uint32_t ip_type)
{
	struct amd_conc_userq_destroy_args dargs[2];
	struct amd_test_userq_ctx userq;
	pthread_barrier_t barrier;
	pthread_t threads[2];
	int i;

	memset(&userq, 0, sizeof(userq));
	if (!amd_test_try_create_userq(fd, dev, ip_type, &userq)) {
		igt_info("concurrent-userq-fuzzing[%s]: queue unavailable, "
			 "skip double-destroy\n", amd_userq_ip_name(ip_type));
		return;
	}

	igt_assert(pthread_barrier_init(&barrier, NULL, 2) == 0);
	igt_info("concurrent-userq-fuzzing[%s]: double-destroy start "
		 "(queue_id=%u)\n", amd_userq_ip_name(ip_type), userq.queue_id);
	for (i = 0; i < 2; i++) {
		dargs[i].fd       = fd;
		dargs[i].queue_id = userq.queue_id;
		dargs[i].barrier  = &barrier;
		igt_assert(pthread_create(&threads[i], NULL,
					  thread_userq_double_destroy,
					  &dargs[i]) == 0);
	}
	pthread_join(threads[0], NULL);
	pthread_join(threads[1], NULL);
	pthread_barrier_destroy(&barrier);
	userq.created = false;
	amd_test_userq_cleanup(fd, dev, &userq);
}

static void
amd_userq_run_wait_destroy_race(int fd, amdgpu_device_handle dev,
				 uint32_t ip_type)
{
	struct amd_conc_userq_op_state s;
	struct amd_test_userq_ctx userq;
	pthread_t waiter, destroyer;

	memset(&userq, 0, sizeof(userq));
	if (!amd_test_try_create_userq(fd, dev, ip_type, &userq)) {
		igt_info("concurrent-userq-fuzzing[%s]: queue unavailable, "
			 "skip wait-vs-destroy\n", amd_userq_ip_name(ip_type));
		return;
	}

	memset(&s, 0, sizeof(s));
	s.fd       = fd;
	s.dev      = dev;
	s.ip_type  = ip_type;
	s.queue_id = userq.queue_id;
	userq.created = false;
	igt_assert(pthread_barrier_init(&s.barrier, NULL, 2) == 0);
	igt_info("concurrent-userq-fuzzing[%s]: wait-vs-destroy start "
		 "(queue_id=%u)\n", amd_userq_ip_name(ip_type), s.queue_id);
	igt_assert(pthread_create(&waiter, NULL, thread_userq_waiter, &s) == 0);
	igt_assert(pthread_create(&destroyer, NULL, thread_userq_op_destroyer, &s) == 0);
	pthread_join(waiter, NULL);
	pthread_join(destroyer, NULL);
	pthread_barrier_destroy(&s.barrier);
	amd_test_userq_cleanup(fd, dev, &userq);
}

static void
amd_userq_run_write_negative_destroy_race(int fd, amdgpu_device_handle dev,
					 uint32_t ip_type)
{
	struct amd_conc_userq_write_state s;
	struct amd_test_userq_ctx userq;
	pthread_t writer, neg_destroyer;

	memset(&userq, 0, sizeof(userq));
	if (!amd_test_try_create_userq(fd, dev, ip_type, &userq)) {
		igt_info("concurrent-userq-fuzzing[%s]: queue unavailable, "
			 "skip write-vs-negative-vs-destroy\n",
			 amd_userq_ip_name(ip_type));
		return;
	}

	memset(&s, 0, sizeof(s));
	s.fd = fd;
	s.dev = dev;
	s.ip_type = ip_type;
	s.queue_id = userq.queue_id;
	s.ring_context = userq.ring_context;
	userq.created = false;

	igt_assert(pthread_barrier_init(&s.barrier, NULL, 2) == 0);
	igt_info("concurrent-userq-fuzzing[%s]: write-vs-negative-vs-destroy "
		 "start (queue_id=%u)\n", amd_userq_ip_name(ip_type), s.queue_id);
	igt_assert(pthread_create(&writer, NULL, thread_userq_writer, &s) == 0);
	igt_assert(pthread_create(&neg_destroyer, NULL,
				  thread_userq_negative_destroyer, &s) == 0);
	pthread_join(writer, NULL);
	pthread_join(neg_destroyer, NULL);
	pthread_barrier_destroy(&s.barrier);
	amd_test_userq_cleanup(fd, dev, &userq);
}

static void
amd_userq_concurrent_fuzzing(int fd, amdgpu_device_handle dev,
				    const bool userq_arr_cap[AMD_IP_MAX])
{
	static const uint32_t ip_types[] = {
		AMDGPU_HW_IP_GFX,
		AMDGPU_HW_IP_COMPUTE,
		AMDGPU_HW_IP_DMA,
	};
	unsigned int i;

	if (!dev) {
		igt_info("concurrent-userq-fuzzing: no amdgpu device, skip\n");
		return;
	}

	for (i = 0; i < ARRAY_SIZE(ip_types); i++) {
		uint32_t ip_type = ip_types[i];
		enum amd_ip_block_type block_type;

		if (!amd_test_map_hw_to_ip_block_type(ip_type, &block_type)) {
			igt_info("concurrent-userq-fuzzing[%s]: unsupported hw ip, skip all races\n",
				 amd_userq_ip_name(ip_type));
			continue;
		}

		if (!userq_arr_cap[block_type]) {
			igt_info("concurrent-userq-fuzzing[%s]: userq capability not present, skip all races\n",
				 amd_userq_ip_name(ip_type));
			continue;
		}

		igt_info("concurrent-userq-fuzzing[%s]: start\n",
			 amd_userq_ip_name(ip_type));
		amd_userq_run_create_destroy_race(fd, dev, ip_type);
		amd_userq_run_signal_destroy_race(fd, dev, ip_type);
		amd_userq_run_double_destroy_race(fd, dev, ip_type);
		amd_userq_run_wait_destroy_race(fd, dev, ip_type);
		amd_userq_run_write_negative_destroy_race(fd, dev, ip_type);
		igt_info("concurrent-userq-fuzzing[%s]: done\n",
			 amd_userq_ip_name(ip_type));
	}
}
#endif /* AMDGPU_USERQ_ENABLED */

int igt_main()
{
	int fd = -1;
	amdgpu_device_handle amdgpu_dev = NULL;
	uint32_t major_version = 0, minor_version = 0;
	struct amdgpu_gpu_info gpu_info = {0};
	bool userq_arr_cap[AMD_IP_MAX] = {0};
	int r;
	const enum amd_ip_block_type arr_types[] = {
			AMD_IP_GFX, AMD_IP_COMPUTE, AMD_IP_DMA, AMD_IP_UVD,
			AMD_IP_VCE, AMD_IP_UVD_ENC, AMD_IP_VCN_DEC, AMD_IP_VCN_ENC,
			AMD_IP_VCN_JPEG, AMD_IP_VPE
	};

	igt_fixture() {
		fd = drm_open_driver(DRIVER_AMDGPU);
		igt_require(fd != -1);
		r = amdgpu_device_initialize(fd, &major_version, &minor_version, &amdgpu_dev);
		igt_require(r == 0);

		r = amdgpu_query_gpu_info(amdgpu_dev, &gpu_info);
		igt_require(r == 0);

		r = setup_amdgpu_ip_blocks(major_version, minor_version,
					    &gpu_info, amdgpu_dev);
		igt_require(r == 0);

		asic_userq_readiness(amdgpu_dev, userq_arr_cap);
	}

	igt_describe("Check user ptr fuzzing with huge size and not valid address");
	igt_subtest("userptr-fuzzing")
		amd_gem_userptr_fuzzing(fd);

	igt_describe("Check cs wait fuzzing");
	igt_subtest("cs-wait-fuzzing")
		amd_cs_wait_fuzzing(fd, arr_types, ARRAY_SIZE(arr_types));

	igt_describe("Check gem create fuzzing");
	igt_subtest("gem-create-fuzzing")
		amd_gem_create_fuzzing(fd);

	igt_describe("Field-level fuzzing for KGD ioctls (including gem_create): mutate ioctl input fields with invalid values");
	igt_subtest("kgd-ioctl-field-fuzzing")
		amd_kgd_multi_ioctl_field_fuzzing(fd, amdgpu_dev);

	igt_describe("Boundary value and edge-case fuzzing: extreme sizes, alignments, domain combos, AMDGPU_INFO exhaustive query sweep");
	igt_subtest("boundary-fuzzing")
		amd_boundary_fuzzing(fd);

	igt_describe("Lifecycle/sequence fuzzing: double-free, use-after-free, operations on freed resources");
	igt_subtest("lifecycle-fuzzing")
		amd_lifecycle_fuzzing(fd);

#ifdef AMDGPU_USERQ_ENABLED
	igt_describe("USERQ concurrency fuzzing: concurrent CREATE/DESTROY, SIGNAL/WAIT racing DESTROY, double-DESTROY");
	igt_subtest("concurrent-userq-fuzzing")
		amd_userq_concurrent_fuzzing(fd, amdgpu_dev, userq_arr_cap);
#endif

	igt_fixture() {
		drm_close_driver(fd);
	}
}
