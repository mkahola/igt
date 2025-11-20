// SPDX-License-Identifier: MIT
/*
 * Copyright 2024 Advanced Micro Devices, Inc.
 */

#include "lib/amdgpu/amd_memory.h"
#include "lib/amdgpu/amd_gfx.h"
#include "lib/ioctl_wrappers.h"

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
static
void amd_gem_userptr_fuzzing(int fd)
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
static
void amd_cs_wait_fuzzing(int fd, const enum amd_ip_block_type types[], int size)
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
	ret = drmIoctl(fd, 0xc0206440
			/* DRM_AMDGPU_GEM_CREATE amdgpu_gem_create_ioctl */, &arg);
	igt_info("drmCommandWriteRead DRM_AMDGPU_GEM_CREATE ret %d\n", ret);

	arg.in.bo_size = 0x7fffffff;
	arg.in.alignment = 0x0;
	arg.in.domains = 0x4;
	arg.in.domain_flags = 0x9;
	ret = drmIoctl(fd, 0xc0206440
			/* DRM_AMDGPU_GEM_CREATE amdgpu_gem_create_ioctl */, &arg);
	igt_info("drmCommandWriteRead DRM_AMDGPU_GEM_CREATE ret %d\n", ret);

	ret = amdgpu_ftrace_enablement(function_amdgpu_bo_move, false);
	igt_assert_eq(ret, 0);

}

int igt_main()
{
	int fd = -1;
	const enum amd_ip_block_type arr_types[] = {
			AMD_IP_GFX, AMD_IP_COMPUTE, AMD_IP_DMA, AMD_IP_UVD,
			AMD_IP_VCE, AMD_IP_UVD_ENC, AMD_IP_VCN_DEC, AMD_IP_VCN_ENC,
			AMD_IP_VCN_JPEG, AMD_IP_VPE
	};

	igt_fixture() {
		fd = drm_open_driver(DRIVER_AMDGPU);
		igt_require(fd != -1);
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

	igt_fixture() {
		drm_close_driver(fd);
	}
}
