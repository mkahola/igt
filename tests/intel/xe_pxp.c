// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024-2025 Intel Corporation
 */

#include <fcntl.h>

#include "igt.h"
#include "igt_syncobj.h"
#include "intel_batchbuffer.h"
#include "intel_bufops.h"
#include "intel_mocs.h"
#include "intel_pat.h"
#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"

IGT_TEST_DESCRIPTION("Test PXP that manages protected content through arbitrated HW-PXP-session");
/* Note: PXP = "Protected Xe Path" */

/**
 * TEST: Test PXP functionality
 * Category: Content protection
 * Mega feature: PXP
 * Sub-category: PXP tests
 * Functionality: Execution of protected content
 * Test category: functionality test
 */

static int __pxp_bo_create(int fd, uint32_t vm, uint64_t size, uint32_t placement,
			   uint32_t session_type, uint32_t flags, uint32_t *handle)
{
	struct drm_xe_ext_set_property ext = {
		.base.next_extension = 0,
		.base.name = DRM_XE_GEM_CREATE_EXTENSION_SET_PROPERTY,
		.property = DRM_XE_GEM_CREATE_SET_PROPERTY_PXP_TYPE,
		.value = session_type,
	};
	int ret = 0;

	if (__xe_bo_create(fd, vm, size, placement, flags, &ext, handle)) {
		ret = -errno;
		errno = 0;
	}

	return ret;
}

static uint32_t pxp_bo_create(int fd, uint32_t vm, uint64_t size, uint32_t type)
{
	uint32_t handle;

	igt_assert_eq(__pxp_bo_create(fd, vm, size, system_memory(fd), type, 0, &handle), 0);

	return handle;
}

static uint32_t pxp_bo_create_display(int fd, uint32_t vm, uint64_t size, uint32_t type)
{
	uint32_t handle;

	igt_assert_eq(__pxp_bo_create(fd, vm, size, vram_if_possible(fd, 0), type,
				      DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM |
				      DRM_XE_GEM_CREATE_FLAG_SCANOUT,
				      &handle), 0);

	return handle;
}

static int __create_pxp_rcs_queue(int fd, uint32_t vm,
				  uint32_t session_type,
				  uint32_t *q)
{
	struct drm_xe_engine_class_instance inst = {
		.engine_class = DRM_XE_ENGINE_CLASS_RENDER,
	};
	struct drm_xe_ext_set_property ext = { 0 };
	uint64_t ext_ptr = to_user_pointer(&ext);

	ext.base.next_extension = 0,
	ext.base.name = DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY,
	ext.property = DRM_XE_EXEC_QUEUE_SET_PROPERTY_PXP_TYPE,
	ext.value = session_type;

	return __xe_exec_queue_create(fd, vm, 1, 1, &inst, ext_ptr, q);
}

static uint32_t create_pxp_rcs_queue(int fd, uint32_t vm)
{
	uint32_t q;
	int err;

	err = __create_pxp_rcs_queue(fd, vm, DRM_XE_PXP_TYPE_HWDRM, &q);
	igt_assert_eq(err, 0);

	return q;
}

static uint32_t create_regular_rcs_queue(int fd, uint32_t vm)
{
	struct drm_xe_engine_class_instance inst = {
		.engine_class = DRM_XE_ENGINE_CLASS_RENDER,
	};

	return xe_exec_queue_create(fd, vm, &inst, 0);
}

static bool is_pxp_hw_supported(int fd)
{
	int ret = xe_wait_for_pxp_init(fd);

	/* -EINVAL means the PXP interface is not available */
	igt_require(ret != -EINVAL);

	return ret == 0;
}

/**
 * SUBTEST: pxp-bo-alloc
 * Description: Verify PXP bo allocation works as expected
 */
static void test_pxp_bo_alloc(int fd, bool pxp_supported)
{
	uint32_t bo;
	int ret;

	/* BO creation with DRM_XE_PXP_TYPE_NONE must always succeed */
	ret = __pxp_bo_create(fd, 0, 4096, system_memory(fd), DRM_XE_PXP_TYPE_NONE, 0, &bo);
	igt_assert_eq(ret, 0);
	gem_close(fd, bo);

	/* BO creation with DRM_XE_PXP_TYPE_HWDRM must only succeed if PXP is supported */
	ret = __pxp_bo_create(fd, 0, 4096, system_memory(fd), DRM_XE_PXP_TYPE_HWDRM, 0, &bo);
	igt_assert_eq(ret, pxp_supported ? 0 : -ENODEV);
	if (!ret)
		gem_close(fd, bo);

	/* BO creation with an invalid type must always fail */
	ret = __pxp_bo_create(fd, 0, 4096, system_memory(fd), 0xFF, 0, &bo);
	igt_assert_eq(ret, -EINVAL);
}

/**
 * SUBTEST: pxp-queue-alloc
 * Description: Verify PXP exec queue creation works as expected
 */
static void test_pxp_queue_creation(int fd, bool pxp_supported)
{
	uint32_t q;
	uint32_t vm;
	int ret;

	vm = xe_vm_create(fd, 0, 0);

	/* queue creation with DRM_XE_PXP_TYPE_NONE must always succeed */
	ret = __create_pxp_rcs_queue(fd, vm, DRM_XE_PXP_TYPE_NONE, &q);
	igt_assert_eq(ret, 0);
	xe_exec_queue_destroy(fd, q);

	/* queue creation with DRM_XE_PXP_TYPE_HWDRM must only succeed if PXP is supported */
	ret = __create_pxp_rcs_queue(fd, vm, DRM_XE_PXP_TYPE_HWDRM, &q);
	igt_assert_eq(ret, pxp_supported ? 0 : -ENODEV);
	if (!ret)
		xe_exec_queue_destroy(fd, q);

	/* queue creation with an invalid type must always fail */
	ret = __create_pxp_rcs_queue(fd, vm, 0xFF, &q);
	igt_assert_eq(ret, -EINVAL);

	xe_vm_destroy(fd, vm);
}

static void fill_bo_content(int fd, uint32_t bo, uint32_t size, uint8_t initcolor)
{
	uint32_t *ptr;

	ptr = xe_bo_mmap_ext(fd, bo, size, PROT_READ|PROT_WRITE);

	/* read and count all dword matches till size */
	memset(ptr, initcolor, size);

	igt_assert(munmap(ptr, size) == 0);
}

static void __check_bo_color(int fd, uint32_t bo, uint32_t size, uint32_t color, bool should_match)
{
	uint64_t comp;
	uint64_t *ptr;
	int i, num_matches = 0;

	comp = color;
	comp = comp | (comp << 32);

	ptr =  xe_bo_mmap_ext(fd, bo, size, PROT_READ);

	igt_assert_eq(size % sizeof(uint64_t), 0);

	for (i = 0; i < (size / sizeof(uint64_t)); i++)
		if (ptr[i] == comp)
			++num_matches;

	if (should_match)
		igt_assert_eq(num_matches, (size / sizeof(uint64_t)));
	else
		igt_assert_eq(num_matches, 0);
}

static void check_bo_color(int fd, uint32_t bo, uint32_t size, uint8_t color, bool should_match)
{
	uint32_t comp;

	/*
	 * We memset the buffer using a u8 color value. However, this is too
	 * small to ensure the encrypted data does not accidentally match it,
	 * so we scale it up to a bigger size.
	 */
	comp = color;
	comp = comp | (comp << 8) | (comp << 16) | (comp << 24);

	return __check_bo_color(fd, bo, size, comp, should_match);
}

static uint32_t __bo_create_and_fill(int fd, uint32_t vm, bool protected,
				     uint32_t size, uint8_t init_color)
{
	uint32_t bo;

	if (protected)
		bo = pxp_bo_create(fd, vm, size, DRM_XE_PXP_TYPE_HWDRM);
	else
		bo = xe_bo_create(fd, vm, size, system_memory(fd), 0);

	fill_bo_content(fd, bo, size, init_color);

	return bo;
}

static uint32_t pxp_bo_create_and_fill(int fd, uint32_t vm, uint32_t size,
				       uint8_t init_color)
{
	return __bo_create_and_fill(fd, vm, true, size, init_color);
}

static uint32_t regular_bo_create_and_fill(int fd, uint32_t vm, uint32_t size,
					   uint8_t init_color)
{
	return __bo_create_and_fill(fd, vm, false, size, init_color);
}

static struct intel_buf *buf_create(int fd, struct buf_ops *bops, uint32_t handle,
				    int width, int height, int bpp, uint64_t size)
{
	igt_assert(handle);
	igt_assert(size);
	return intel_buf_create_full(bops, handle, width, height, bpp, 0,
				     I915_TILING_NONE, 0, size, 0,
				     system_memory(fd),
				     DEFAULT_PAT_INDEX, DEFAULT_MOCS_INDEX);
}

/* Rendering tests surface attributes */
#define TSTSURF_WIDTH		64
#define TSTSURF_HEIGHT		64
#define TSTSURF_BYTESPP		4
#define TSTSURF_STRIDE		(TSTSURF_WIDTH * TSTSURF_BYTESPP)
#define TSTSURF_SIZE		(TSTSURF_STRIDE * TSTSURF_HEIGHT)
#define TSTSURF_INITCOLOR1  0xAA
#define TSTSURF_FILLCOLOR1  0x55
#define TSTSURF_INITCOLOR2  0x33

static void pxp_rendercopy(int fd, uint32_t q, uint32_t vm, uint32_t copy_size,
			   uint32_t srcbo, bool src_pxp, uint32_t dstbo, bool dst_pxp)
{
	igt_render_copyfunc_t render_copy;
	struct intel_buf *srcbuf, *dstbuf;
	struct buf_ops *bops;
	struct intel_bb *ibb;

	/*
	 * we use the defined width and height below, which only works if the BO
	 * size is TSTSURF_SIZE
	 */
	igt_assert_eq(copy_size, TSTSURF_SIZE);

	render_copy = igt_get_render_copyfunc(fd);
	igt_assert(render_copy);

	bops = buf_ops_create(fd);
	igt_assert(bops);

	ibb = intel_bb_create_with_context(fd, q, vm, NULL, 4096);
	igt_assert(ibb);
	intel_bb_set_pxp(ibb, true, DISPLAY_APPTYPE, DRM_XE_PXP_HWDRM_DEFAULT_SESSION);

	dstbuf = buf_create(fd, bops, dstbo, TSTSURF_WIDTH, TSTSURF_HEIGHT,
			    TSTSURF_BYTESPP * 8, TSTSURF_SIZE);
	intel_buf_set_pxp(dstbuf, dst_pxp);

	srcbuf = buf_create(fd, bops, srcbo, TSTSURF_WIDTH, TSTSURF_HEIGHT,
			    TSTSURF_BYTESPP * 8, TSTSURF_SIZE);
	intel_buf_set_pxp(srcbuf, src_pxp);

	render_copy(ibb, srcbuf, 0, 0, TSTSURF_WIDTH, TSTSURF_HEIGHT, dstbuf, 0, 0);
	intel_bb_sync(ibb);

	intel_buf_destroy(srcbuf);
	intel_buf_destroy(dstbuf);
	intel_bb_destroy(ibb);
	buf_ops_destroy(bops);
}

static void copy_bo_cpu(int fd, uint32_t bo, uint32_t *dst, uint32_t size)
{
	uint32_t *src_ptr;

	src_ptr = xe_bo_mmap_ext(fd, bo, size, PROT_READ);

	memcpy(dst, src_ptr, size);

	igt_assert_eq(munmap(src_ptr, size), 0);
}

static void __test_render_regular_src_to_pxp_dest(int fd, uint32_t *outpixels, int outsize)
{
	uint32_t vm, srcbo, dstbo;
	uint32_t q;

	if (outpixels)
		igt_assert_lte(TSTSURF_SIZE, outsize);

	vm = xe_vm_create(fd, 0, 0);

	/*
	 * Perform a protected render operation but only label the dest as
	 * protected. After rendering, the content should be encrypted.
	 */
	q = create_pxp_rcs_queue(fd, vm);

	srcbo = regular_bo_create_and_fill(fd, vm, TSTSURF_SIZE, TSTSURF_FILLCOLOR1);
	dstbo = pxp_bo_create_and_fill(fd, vm, TSTSURF_SIZE, TSTSURF_INITCOLOR1);

	pxp_rendercopy(fd, q, vm, TSTSURF_SIZE, srcbo, false, dstbo, true);

	check_bo_color(fd, dstbo, TSTSURF_SIZE, TSTSURF_FILLCOLOR1, false);

	if (outpixels)
		copy_bo_cpu(fd, dstbo, outpixels, TSTSURF_SIZE);

	gem_close(fd, srcbo);
	gem_close(fd, dstbo);
	xe_exec_queue_destroy(fd, q);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: regular-src-to-pxp-dest-rendercopy
 * Description: copy from a regular BO to a PXP one and verify the encryption
 */
static void test_render_regular_src_to_pxp_dest(int fd)
{
	__test_render_regular_src_to_pxp_dest(fd, NULL, 0);
}

static int bocmp(int fd, uint32_t bo1, uint32_t bo2, uint32_t size)
{
	uint32_t *ptr1, *ptr2;
	int ret;

	ptr1 = xe_bo_mmap_ext(fd, bo1, size, PROT_READ);
	ptr2 = xe_bo_mmap_ext(fd, bo2, size, PROT_READ);

	ret = memcmp(ptr1, ptr2, size);

	igt_assert_eq(munmap(ptr1, size), 0);
	igt_assert_eq(munmap(ptr2, size), 0);

	return ret;
}

/**
 * SUBTEST: pxp-src-to-pxp-dest-rendercopy
 * Description: copy between 2 PXP BOs and verify the encryption
 */

static void test_render_pxp_protsrc_to_protdest(int fd)
{
	uint32_t vm, srcbo, dstbo, dstbo2;
	uint32_t q;

	vm = xe_vm_create(fd, 0, 0);

	q = create_pxp_rcs_queue(fd, vm);

	/*
	 * Copy from a regular src to a PXP dst to get a buffer with a
	 * valid encryption.
	 */
	srcbo = regular_bo_create_and_fill(fd, vm, TSTSURF_SIZE, TSTSURF_FILLCOLOR1);
	dstbo = pxp_bo_create_and_fill(fd, vm, TSTSURF_SIZE, TSTSURF_INITCOLOR1);

	pxp_rendercopy(fd, q, vm, TSTSURF_SIZE, srcbo, false, dstbo, true);

	check_bo_color(fd, dstbo, TSTSURF_SIZE, TSTSURF_FILLCOLOR1, false);

	/*
	 * Reuse prior dst as the new-src and create dst2 as the new-dest.
	 * After the rendering, we should find no difference in content since
	 * both new-src and new-dest are labelled as encrypted. HW should read
	 * and decrypt new-src, perform the copy and re-encrypt with the same
	 * key when going into new-dest
	 */
	dstbo2 = pxp_bo_create_and_fill(fd, vm, TSTSURF_SIZE, TSTSURF_INITCOLOR2);

	pxp_rendercopy(fd, q, vm, TSTSURF_SIZE, dstbo, true, dstbo2, true);

	igt_assert_eq(bocmp(fd, dstbo, dstbo2, TSTSURF_SIZE), 0);

	gem_close(fd, srcbo);
	gem_close(fd, dstbo);
	gem_close(fd, dstbo2);
	xe_exec_queue_destroy(fd, q);
	xe_vm_destroy(fd, vm);
}

#define PS_OP_TAG_LOW 0x1234fed0
#define PS_OP_TAG_HI 0x5678cbaf
static void emit_pipectrl(struct intel_bb *ibb, struct intel_buf *fenceb)
{
	uint32_t pipe_ctl_flags = 0;

	intel_bb_out(ibb, GFX_OP_PIPE_CONTROL(2));
	intel_bb_out(ibb, pipe_ctl_flags);

	pipe_ctl_flags = (PIPE_CONTROL_FLUSH_ENABLE |
			  PIPE_CONTROL_CS_STALL |
			  PIPE_CONTROL_QW_WRITE);
	intel_bb_out(ibb, GFX_OP_PIPE_CONTROL(6));
	intel_bb_out(ibb, pipe_ctl_flags);

	intel_bb_emit_reloc(ibb, fenceb->handle, 0, I915_GEM_DOMAIN_COMMAND, 0,
			    fenceb->addr.offset);
	intel_bb_out(ibb, PS_OP_TAG_LOW);
	intel_bb_out(ibb, PS_OP_TAG_HI);
	intel_bb_out(ibb, MI_NOOP);
	intel_bb_out(ibb, MI_NOOP);
}

static void assert_pipectl_storedw_done(int fd, uint32_t bo)
{
	uint32_t *ptr;

	ptr = xe_bo_mmap_ext(fd, bo, 4096, PROT_READ|PROT_WRITE);
	igt_assert_eq(ptr[0], PS_OP_TAG_LOW);
	igt_assert_eq(ptr[1], PS_OP_TAG_HI);

	igt_assert(munmap(ptr, 4096) == 0);
}

static int submit_flush_store_dw(int fd, uint32_t q, bool q_is_pxp, uint32_t vm,
				 uint32_t dst, bool dst_is_pxp)
{
	struct intel_buf *dstbuf;
	struct buf_ops *bops;
	struct intel_bb *ibb;
	int ret = 0;

	bops = buf_ops_create(fd);
	igt_assert(bops);

	ibb = intel_bb_create_with_context(fd, q, vm, NULL, 4096);
	igt_assert(ibb);
	intel_bb_set_pxp(ibb, q_is_pxp, DISPLAY_APPTYPE, DRM_XE_PXP_HWDRM_DEFAULT_SESSION);

	dstbuf = buf_create(fd, bops, dst, 256, 4, 32, 4096);
	intel_buf_set_pxp(dstbuf, dst_is_pxp);

	intel_bb_ptr_set(ibb, 0);
	intel_bb_add_intel_buf(ibb, dstbuf, true);
	emit_pipectrl(ibb, dstbuf);
	intel_bb_emit_bbe(ibb);
	ret = __xe_bb_exec(ibb, 0, false);
	if (ret == 0)
		ret = intel_bb_sync(ibb);
	if (ret == 0)
		assert_pipectl_storedw_done(fd, dst);

	intel_buf_destroy(dstbuf);
	intel_bb_destroy(ibb);
	buf_ops_destroy(bops);

	return ret;
}

static void trigger_pxp_debugfs_forced_teardown(int xe_fd)
{
	char str[32];
	int ret;
	int fd;

	fd = igt_debugfs_dir(xe_fd);
	igt_assert(fd >= 0);
	ret = igt_debugfs_simple_read(fd, "pxp/terminate", str, 32);
	igt_assert_f(ret >= 0, "Can't open pxp termination debugfs\n");

	/* give the kernel time to handle the termination */
	sleep(1);
}

enum termination_type {
	PXP_TERMINATION_IRQ,
	PXP_TERMINATION_RPM,
	PXP_TERMINATION_SUSPEND
};

static void trigger_termination(int fd, enum termination_type type)
{
	switch (type) {
	case PXP_TERMINATION_IRQ:
		trigger_pxp_debugfs_forced_teardown(fd);
		break;
	case PXP_TERMINATION_RPM:
		igt_require(igt_wait_for_pm_status(IGT_RUNTIME_PM_STATUS_SUSPENDED));
		break;
	case PXP_TERMINATION_SUSPEND:
		igt_system_suspend_autoresume(SUSPEND_STATE_MEM, SUSPEND_TEST_DEVICES);
		break;
	}
}

/**
 * SUBTEST: pxp-termination-key-update-post-termination-irq
 * Description: Verify key is changed after a termination irq
 */

/**
 * SUBTEST: pxp-termination-key-update-post-suspend
 * Description: Verify key is changed after a suspend/resume cycle
 */

/**
 * SUBTEST: pxp-termination-key-update-post-rpm
 * Description: Verify key is changed after a runtime suspend/resume cycle
 */

static void test_pxp_teardown_keychange(int fd, enum termination_type type)
{
	uint32_t *encrypted_data_before;
	uint32_t *encrypted_data_after;
	int matched_after_keychange = 0, loop = 0;

	encrypted_data_before = malloc(TSTSURF_SIZE);
	encrypted_data_after = malloc(TSTSURF_SIZE);
	igt_assert(encrypted_data_before && encrypted_data_after);

	__test_render_regular_src_to_pxp_dest(fd, encrypted_data_before, TSTSURF_SIZE);

	trigger_termination(fd, type);

	__test_render_regular_src_to_pxp_dest(fd, encrypted_data_after, TSTSURF_SIZE);

	while (loop < (TSTSURF_SIZE/TSTSURF_BYTESPP)) {
		if (encrypted_data_before[loop] == encrypted_data_after[loop])
			++matched_after_keychange;
		++loop;
	}
	igt_assert_eq(matched_after_keychange, 0);

	free(encrypted_data_before);
	free(encrypted_data_after);
}

static void pxp_vm_bind_sync(int fd, uint32_t vm, uint32_t bo, uint64_t addr,
			     uint64_t size, uint32_t op)
{
	struct drm_xe_sync sync = {
		.type = DRM_XE_SYNC_TYPE_SYNCOBJ,
		.flags = DRM_XE_SYNC_FLAG_SIGNAL,
		.handle = syncobj_create(fd, 0),
	};

	__xe_vm_bind_assert(fd, vm, 0, bo, 0, addr, size, op,
			    DRM_XE_VM_BIND_FLAG_CHECK_PXP, &sync, 1, 0, 0);

	igt_assert(syncobj_wait(fd, &sync.handle, 1, INT64_MAX, 0, NULL));
	syncobj_destroy(fd, sync.handle);
}

/**
 * SUBTEST: pxp-stale-bo-bind-post-termination-irq
 * Description: verify that VM bind on a stale BO (due to a termination irq) is rejected.
 */

/**
 * SUBTEST: pxp-stale-bo-bind-post-suspend
 * Description: verify that VM bind on a stale BO (due to a suspend/resume cycle)
 *              is rejected.
 */

/**
 * SUBTEST: pxp-stale-bo-bind-post-rpm
 * Description: verify that VM bind on a stale BO (due to a runtime suspend/resume
 *              cycle) is rejected.
 */

static void __test_pxp_stale_bo_bind(int fd, enum termination_type type, bool pxp)
{
	uint32_t vm, q;
	uint32_t bo;
	uint32_t flags = pxp ? DRM_XE_VM_BIND_FLAG_CHECK_PXP : 0;
	int ret;

	vm = xe_vm_create(fd, 0, 0);
	q = create_pxp_rcs_queue(fd, vm); /* start PXP session */

	bo = pxp_bo_create(fd, 0, 4096, DRM_XE_PXP_TYPE_HWDRM);

	/* map the BO to the VM to make sure it works */
	pxp_vm_bind_sync(fd, vm, bo, 0, 4096, DRM_XE_VM_BIND_OP_MAP);

	xe_exec_queue_destroy(fd, q);
	trigger_termination(fd, type);

	/* map of a stale PXP BO must fail if (and only if) the CHECK_PXP flag is set */
	ret = __xe_vm_bind(fd, vm, 0, bo, 0, 0, 4096, DRM_XE_VM_BIND_OP_MAP,
			   flags, NULL, 0, 0, DEFAULT_PAT_INDEX, 0);
	igt_assert_eq(ret, pxp ? -ENOEXEC : 0);

	/* unmap must always work */
	pxp_vm_bind_sync(fd, vm, bo, 0, 0, DRM_XE_VM_BIND_OP_UNMAP_ALL);

	xe_vm_destroy(fd, vm);

	/* mapping on a brand new vm should have the same behavior */
	vm = xe_vm_create(fd, 0, 0);
	ret = __xe_vm_bind(fd, vm, 0, bo, 0, 0, 4096, DRM_XE_VM_BIND_OP_MAP,
			   flags, NULL, 0, 0, DEFAULT_PAT_INDEX, 0);
	igt_assert_eq(ret, pxp ? -ENOEXEC : 0);

	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

static void test_pxp_stale_bo_bind(int fd, enum termination_type type)
{
	__test_pxp_stale_bo_bind(fd, type, true);
}

static uint32_t create_and_bind_simple_pxp_batch(int fd, uint32_t vm,
						 uint32_t size, uint64_t addr)
{
	uint32_t bo;
	uint32_t *map;

	bo = pxp_bo_create(fd, vm, 4096, DRM_XE_PXP_TYPE_HWDRM);
	pxp_vm_bind_sync(fd, vm, bo, addr, size, DRM_XE_VM_BIND_OP_MAP);

	map = xe_bo_map(fd, bo, 4096);
	*map = MI_BATCH_BUFFER_END;
	munmap(map, 4096);

	return bo;
}

/**
 * SUBTEST: pxp-stale-bo-exec-post-termination-irq
 * Description: verify that a submission using VM with a mapped stale BO (due to
 *              a termination irq) is rejected.
 */

/**
 * SUBTEST: pxp-stale-bo-exec-post-suspend
 * Description: verify that a submission using VM with a mapped stale BO (due to
 *              a suspend/resume cycle) is rejected.
 */

/**
 * SUBTEST: pxp-stale-bo-exec-post-rpm
 * Description: verify that a submission using VM with a mapped stale BO (due to
 *              a runtime suspend/resume cycle) is rejected.
 */

static void __test_pxp_stale_bo_exec(int fd, enum termination_type type, bool pxp)
{
	uint32_t vm, q;
	uint32_t bo;
	int expected;

	vm = xe_vm_create(fd, 0, 0);

	q = create_pxp_rcs_queue(fd, vm); /* start a PXP session */
	bo = create_and_bind_simple_pxp_batch(fd, vm, 4096, 0);

	xe_exec_queue_destroy(fd, q);
	trigger_termination(fd, type);

	/* create a clean queue using the VM with the invalid object mapped in */
	if (pxp) {
		q = create_pxp_rcs_queue(fd, vm);
		expected = -ENOEXEC;
	} else {
		q = create_regular_rcs_queue(fd, vm);
		expected = 0;
	}

	igt_assert_eq(xe_exec_sync_failable(fd, q, 0, NULL, 0), expected);

	/* now make sure we can unmap the stale BO and have a clean exec after */
	if (pxp) {
		pxp_vm_bind_sync(fd, vm, 0, 0, 4096, DRM_XE_VM_BIND_OP_UNMAP);
		gem_close(fd, bo);

		bo = create_and_bind_simple_pxp_batch(fd, vm, 4096, 0);
		igt_assert_eq(xe_exec_sync_failable(fd, q, 0, NULL, 0), 0);
	}

	xe_exec_queue_destroy(fd, q);
	gem_close(fd, bo);
	xe_vm_destroy(fd, vm);
}

static void test_pxp_stale_bo_exec(int fd, enum termination_type type)
{
	__test_pxp_stale_bo_exec(fd, type, true);
}

/**
 * SUBTEST: pxp-stale-queue-post-termination-irq
 * Description: verify that submissions on a stale queue (due to a termination
 *              irq) are cancelled
 */

/**
 * SUBTEST: pxp-stale-queue-post-suspend
 * Description: verify that submissions on a stale queue (due to a suspend/resume
 *              cycle) are cancelled
 */

static void test_pxp_stale_queue_execution(int fd, enum termination_type type)
{
	uint32_t vm, q;
	uint32_t dstbo;

	vm = xe_vm_create(fd, 0, 0);
	q = create_pxp_rcs_queue(fd, vm);

	dstbo = regular_bo_create_and_fill(fd, vm, 4096, 0);

	igt_assert_eq(submit_flush_store_dw(fd, q, true, vm, dstbo, false), 0);

	trigger_termination(fd, type);

	/* when we execute an invalid queue we expect the job to be canceled */
	igt_assert_eq(submit_flush_store_dw(fd, q, true, vm, dstbo, false), -ECANCELED);

	gem_close(fd, dstbo);
	xe_exec_queue_destroy(fd, q);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: pxp-optout
 * Description: verify that submssions with stale objects/queues are not blocked
 *              if the user does not opt-in to the PXP checks.
 */
static void test_pxp_optout(int fd)
{
	__test_pxp_stale_bo_exec(fd, PXP_TERMINATION_IRQ, false);
	__test_pxp_stale_bo_bind(fd, PXP_TERMINATION_IRQ, false);
}

static void setup_fb(int fd, igt_fb_t *pxp_fb, int width, int height, uint32_t size)
{
	/* create an FB using a PXP BO */
	igt_init_fb(pxp_fb, fd, width, height,
		    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_NONE,
		    IGT_COLOR_YCBCR_BT709, IGT_COLOR_YCBCR_LIMITED_RANGE);

	igt_calc_fb_size(pxp_fb);

	pxp_fb->gem_handle = pxp_bo_create_display(fd, 0, size, DRM_XE_PXP_TYPE_HWDRM);

	do_or_die(__kms_addfb(pxp_fb->fd, pxp_fb->gem_handle,
			pxp_fb->width, pxp_fb->height,
			pxp_fb->drm_format, pxp_fb->modifier,
			pxp_fb->strides, pxp_fb->offsets, pxp_fb->num_planes, DRM_MODE_FB_MODIFIERS,
			&pxp_fb->fb_id));
}

static void setup_protected_fb_from_ref(int fd, igt_fb_t *ref_fb, igt_fb_t *pxp_fb,
					uint32_t q, uint32_t vm)
{
	struct intel_buf *srcbuf, *dstbuf;
	struct buf_ops *bops;
	struct intel_bb *ibb;
	igt_render_copyfunc_t render_copy;

	render_copy = igt_get_render_copyfunc(fd);
	igt_assert(render_copy);

	bops = buf_ops_create(fd);
	igt_assert(bops);

	/* create an FB using a PXP BO */
	setup_fb(fd, pxp_fb, ref_fb->width, ref_fb->height, ref_fb->size);

	/* copy the contents of ref_fb into the pxp BO */
	srcbuf = igt_fb_create_intel_buf(fd, bops, ref_fb, "ref_fb");
	dstbuf = igt_fb_create_intel_buf(fd, bops, pxp_fb, "pxp_fb");
	intel_buf_set_pxp(dstbuf, true);

	ibb = intel_bb_create_with_context(fd, q, vm, NULL, 4096);
	igt_assert(ibb);
	intel_bb_set_pxp(ibb, true, DISPLAY_APPTYPE, DRM_XE_PXP_HWDRM_DEFAULT_SESSION);

	render_copy(ibb, srcbuf, 0, 0, pxp_fb->width, pxp_fb->height, dstbuf, 0, 0);
	intel_bb_sync(ibb);

	/* make sure the contents of the BOs don't match */
	igt_assert_neq(bocmp(fd, pxp_fb->gem_handle, ref_fb->gem_handle, pxp_fb->size), 0);

	intel_bb_destroy(ibb);
	intel_buf_destroy(srcbuf);
	intel_buf_destroy(dstbuf);
	buf_ops_destroy(bops);
}

static void commit_fb(igt_display_t *display, igt_plane_t *plane,
		      igt_fb_t *fb, drmModeModeInfo *mode)
{
	igt_plane_set_fb(plane, fb);
	igt_fb_set_size(fb, plane, mode->hdisplay, mode->vdisplay);
	igt_plane_set_size(plane, mode->hdisplay, mode->vdisplay);

	igt_display_commit2(display, COMMIT_ATOMIC);
}

static void compare_crcs(int fd, igt_display_t *display, igt_fb_t *ref_fb, igt_fb_t *pxp_fb)
{
	igt_output_t *output;
	drmModeModeInfo *mode;
	igt_plane_t *plane;
	igt_crtc_t *pipe;
	igt_pipe_crc_t *pipe_crc;
	igt_crc_t ref_crc, new_crc;

	for_each_connected_output(display, output) {
		mode = igt_output_get_mode(output);
		pipe = igt_crtc_for_pipe(display, output->pending_pipe);
		pipe_crc = igt_pipe_crc_new(fd, pipe->pipe,
					    IGT_PIPE_CRC_SOURCE_AUTO);
		plane = igt_crtc_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);
		igt_require(igt_pipe_connector_valid(pipe->pipe, output));
		igt_output_set_crtc(output, pipe);

		commit_fb(display, plane, ref_fb, mode);
		igt_pipe_crc_collect_crc(pipe_crc, &ref_crc);

		commit_fb(display, plane, pxp_fb, mode);
		igt_pipe_crc_collect_crc(pipe_crc, &new_crc);

		igt_assert_crc_equal(&ref_crc, &new_crc);

		/*
		 * Testing with one pipe-output combination is sufficient.
		 * So break the loop.
		 */
		break;
	}
}

/**
 * SUBTEST: display-pxp-fb
 * Description: Test that an encrypted fb is displayed correctly by comparing
 *              its CRCs with the ones generated by a non-encrypted FB
 *              containing the same image
 */

static void test_display_pxp_fb(int fd, igt_display_t *display)
{
	igt_output_t *output;
	drmModeModeInfo *mode;
	igt_fb_t ref_fb, pxp_fb;
	igt_plane_t *plane;
	igt_crtc_t *pipe;
	int width = 0, height = 0, i = 0;
	uint32_t q;
	uint32_t vm;

	vm = xe_vm_create(fd, 0, 0);
	q = create_pxp_rcs_queue(fd, vm); /* start the PXP session */

	for_each_connected_output(display, output) {
		mode = igt_output_get_mode(output);

		width = max_t(int, width, mode->hdisplay);
		height = max_t(int, height, mode->vdisplay);
	}

	igt_create_color_fb(fd, width, height, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			    0, 1, 0, &ref_fb);

	/* Do a modeset on all outputs */
	for_each_connected_output(display, output) {
		mode = igt_output_get_mode(output);
		pipe = igt_crtc_for_pipe(display, i);
		plane = igt_crtc_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);
		igt_require(igt_pipe_connector_valid(i, output));
		igt_output_set_crtc(output,
				    pipe);

		commit_fb(display, plane, &ref_fb, mode);

		i++;
	}

	/* Create an encrypted FB with the same contents as ref_fb */
	setup_protected_fb_from_ref(fd, &ref_fb, &pxp_fb, q, vm);

	/* Flip both FBs and make sure the CRCs match */
	compare_crcs(fd, display, &ref_fb, &pxp_fb);

	igt_remove_fb(fd, &ref_fb);
	igt_remove_fb(fd, &pxp_fb);
	xe_exec_queue_destroy(fd, q);
	xe_vm_destroy(fd, vm);
}

/**
 * SUBTEST: display-black-pxp-fb
 * Description: Test that an invalid encrypted fb is correctly converted to a
 *              black screen by comparing its CRCs with the ones generated by a
 *              non-encrypted FB filled with black
 */

static void test_display_black_pxp_fb(int fd, igt_display_t *display)
{
	igt_output_t *output;
	drmModeModeInfo *mode;
	igt_fb_t ref_fb, pxp_fb;
	igt_plane_t *plane;
	igt_crtc_t *pipe;
	int width = 0, height = 0, i = 0;
	uint32_t q;
	uint32_t vm;

	vm = xe_vm_create(fd, 0, 0);
	q = create_pxp_rcs_queue(fd, vm); /* start the PXP session */

	for_each_connected_output(display, output) {
		mode = igt_output_get_mode(output);

		width = max_t(int, width, mode->hdisplay);
		height = max_t(int, height, mode->vdisplay);
	}

	/* create a black fb */
	igt_create_color_fb(fd, width, height, DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			    0, 0, 0, &ref_fb);

	/* Do a modeset on all outputs */
	for_each_connected_output(display, output) {
		mode = igt_output_get_mode(output);
		pipe = igt_crtc_for_pipe(display, i);
		plane = igt_crtc_get_plane_type(pipe, DRM_PLANE_TYPE_PRIMARY);
		igt_require(igt_pipe_connector_valid(i, output));
		igt_output_set_crtc(output,
				    pipe);

		igt_plane_set_fb(plane, &ref_fb);
		igt_fb_set_size(&ref_fb, plane, mode->hdisplay, mode->vdisplay);
		igt_plane_set_size(plane, mode->hdisplay, mode->vdisplay);

		igt_display_commit2(display, COMMIT_ATOMIC);
		i++;
	}

	/* Create an fb filled with a non-black color */
	setup_fb(fd, &pxp_fb, ref_fb.width, ref_fb.height, ref_fb.size);
	fill_bo_content(fd, pxp_fb.gem_handle, pxp_fb.size, TSTSURF_INITCOLOR1);

	/* invalidate the BO */
	trigger_termination(fd, PXP_TERMINATION_IRQ);

	/* Flip both FBs and make sure the CRCs match */
	compare_crcs(fd, display, &ref_fb, &pxp_fb);

	igt_remove_fb(fd, &ref_fb);
	igt_remove_fb(fd, &pxp_fb);
	xe_exec_queue_destroy(fd, q);
	xe_vm_destroy(fd, vm);
}

static void require_pxp(bool pxp_supported)
{
	igt_require_f(pxp_supported, "PXP not supported\n");
}

static void require_pxp_render(int fd, bool pxp_supported)
{
	require_pxp(pxp_supported);
	igt_require_f(igt_get_render_copyfunc(fd), "No rendercopy found\n");
}

static void require_display(int xe_fd, igt_display_t *display)
{
	igt_require_pipe_crc(xe_fd);
	igt_display_require(display, xe_fd);
}

static void dpms_on_off(int fd, drmModeResPtr res, int mode)
{
	int i;

	if (!res)
		return;

	for (i = 0; i < res->count_connectors; i++) {
		drmModeConnector *connector =
			drmModeGetConnectorCurrent(fd, res->connectors[i]);

		if (!connector)
			continue;

		if (connector->connection == DRM_MODE_CONNECTED)
			kmstest_set_connector_dpms(fd, connector, mode);

		drmModeFreeConnector(connector);
	}
}

static void setup_rpm(int fd, drmModeResPtr res)
{
	igt_require(igt_setup_runtime_pm(fd));

	dpms_on_off(fd, res, DRM_MODE_DPMS_OFF);
}

static void restore_rpm(int fd, drmModeResPtr res)
{
	dpms_on_off(fd, res, DRM_MODE_DPMS_ON);

	igt_restore_runtime_pm();
}

static void run_termination_test(int fd, enum termination_type type, drmModeResPtr res,
				 void (*test)(int fd, enum termination_type type))
{
	int fw_handle;

	if (type != PXP_TERMINATION_RPM) {
		/* avoid rpm entry for non-rpm tests */
		fw_handle = igt_debugfs_open(fd, "forcewake_all", O_RDONLY);
		igt_require(fw_handle >= 0);
	} else {
		setup_rpm(fd, res);
	}

	test(fd, type);

	if (type != PXP_TERMINATION_RPM)
		close(fw_handle);
	else
		restore_rpm(fd, res);
}

static void termination_tests(int fd, bool pxp_supported, drmModeResPtr res,
			      enum termination_type type, const char *tag)
{
	igt_subtest_f("pxp-termination-key-update-post-%s", tag) {
		require_pxp_render(fd, pxp_supported);
		run_termination_test(fd, type, res, test_pxp_teardown_keychange);
	}

	igt_subtest_f("pxp-stale-bo-bind-post-%s", tag) {
		require_pxp(pxp_supported);
		run_termination_test(fd, type, res, test_pxp_stale_bo_bind);
	}

	igt_subtest_f("pxp-stale-bo-exec-post-%s", tag) {
		require_pxp(pxp_supported);
		run_termination_test(fd, type, res, test_pxp_stale_bo_exec);
	}

	/* An active PXP queue holds an RPM ref, so we can't test RPM with it */
	if (type != PXP_TERMINATION_RPM)
		igt_subtest_f("pxp-stale-queue-post-%s", tag) {
			require_pxp(pxp_supported);
			run_termination_test(fd, type, res, test_pxp_stale_queue_execution);
		}
}

int xe_fd = -1;

static void exit_handler(int sig)
{
	/*
	 * PXP can interact with some operations (e.g. suspend/resume), which
	 * could impact the behavior of other tests if they unknowingly start
	 * when PXP is already active. Therefore, let's make sure PXP is
	 * de-activated before we exit.
	 */
	trigger_termination(xe_fd, PXP_TERMINATION_IRQ);
	drm_close_driver(xe_fd);
	usleep(50 * 1000); /* give time for the termination to be processed */
}

int igt_main()
{
	bool pxp_supported = true;
	drmModeResPtr res;

	igt_fixture() {
		xe_fd = drm_open_driver(DRIVER_XE);
		igt_require(xe_has_engine_class(xe_fd, DRM_XE_ENGINE_CLASS_RENDER));
		pxp_supported = is_pxp_hw_supported(xe_fd);
		res = drmModeGetResources(xe_fd);
		if (pxp_supported)
			igt_install_exit_handler(exit_handler);
	}

	igt_subtest_group() {
		igt_describe("Verify PXP allocations work as expected");
		igt_subtest("pxp-bo-alloc")
			test_pxp_bo_alloc(xe_fd, pxp_supported);

		igt_subtest("pxp-queue-alloc")
			test_pxp_queue_creation(xe_fd, pxp_supported);
	}

	igt_subtest_group() {
		igt_describe("Verify protected render operations:");
		igt_subtest("regular-src-to-pxp-dest-rendercopy") {
			require_pxp_render(xe_fd, pxp_supported);
			test_render_regular_src_to_pxp_dest(xe_fd);
		}
		igt_subtest("pxp-src-to-pxp-dest-rendercopy") {
			require_pxp_render(xe_fd, pxp_supported);
			test_render_pxp_protsrc_to_protdest(xe_fd);
		}
	}

	igt_subtest_group() {
		const struct mode {
			enum termination_type type;
			const char *tag;
		} modes[] = {
			{ PXP_TERMINATION_IRQ, "termination-irq" },
			{ PXP_TERMINATION_SUSPEND, "suspend" },
			{ PXP_TERMINATION_RPM, "rpm" },
			{ 0, NULL }
		};

		igt_describe("Verify teardown management");

		for (const struct mode *m = modes; m->tag; m++)
			termination_tests(xe_fd, pxp_supported, res, m->type, m->tag);

		igt_subtest("pxp-optout") {
			require_pxp(pxp_supported);
			test_pxp_optout(xe_fd);
		}
	}

	igt_subtest_group() {
		igt_display_t display;

		igt_describe("Test the flip of PXP objects to display");
		igt_subtest("display-pxp-fb") {
			require_display(xe_fd, &display);
			require_pxp_render(xe_fd, pxp_supported);
			test_display_pxp_fb(xe_fd, &display);
		}

		igt_subtest("display-black-pxp-fb") {
			require_display(xe_fd, &display);
			require_pxp_render(xe_fd, pxp_supported);
			test_display_black_pxp_fb(xe_fd, &display);
		}
	}

	igt_fixture() {
		drmModeFreeResources(res);
		if (!pxp_supported)
			drm_close_driver(xe_fd);
	}
}
