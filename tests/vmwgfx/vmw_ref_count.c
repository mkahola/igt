// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Copyright (c) 2021-2024 Broadcom. All Rights Reserved. The term
 * “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
 */

#include "igt_vmwgfx.h"

IGT_TEST_DESCRIPTION("Perform tests related to vmwgfx's ref_count codepaths.");

#define NUM_SURFACES 10
static uint32 data[10] = { 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 };

static void write_to_mob(int fd, struct vmw_mob *mob)
{
	void *write_data;

	write_data = vmw_ioctl_mob_map(fd, mob);
	igt_assert(write_data);
	memcpy(write_data, data, sizeof(data));
	vmw_ioctl_mob_unmap(mob);
}

static bool verify_mob_data(int fd, struct vmw_mob *mob)
{
	uint32 *read_data;
	void *readback;
	uint32 i;
	bool data_is_equal = true;

	read_data = malloc(mob->size);

	readback = vmw_ioctl_mob_map(fd, mob);
	memcpy(read_data, readback, sizeof(data));
	vmw_ioctl_mob_unmap(mob);

	for (i = 0; i < ARRAY_SIZE(data); i++) {
		if (read_data[i] != data[i]) {
			data_is_equal = false;
			break;
		}
	}

	free(read_data);
	return data_is_equal;
}

static struct vmw_surface *
create_and_write_shareable_surface(int32 fd, SVGA3dSize surface_size)
{
	struct vmw_mob mob = { 0 };
	struct vmw_surface *surface;

	surface = vmw_ioctl_create_surface_full(
		fd, SVGA3D_SURFACE_HINT_RENDERTARGET, SVGA3D_BUFFER, 0,
		SVGA3D_MS_PATTERN_NONE, SVGA3D_MS_QUALITY_NONE,
		SVGA3D_TEX_FILTER_NONE, 1, 1, surface_size, SVGA3D_INVALID_ID,
		drm_vmw_surface_flag_shareable | drm_vmw_surface_flag_create_buffer);

	mob.handle = surface->base.buffer_handle;
	mob.map_handle = surface->base.buffer_map_handle;
	mob.size = surface->base.buffer_size;

	write_to_mob(fd, &mob);

	return surface;
}

static bool ref_surface_and_check_contents(int32 fd, uint32 surface_handle)
{
	struct vmw_surface *surface;
	struct vmw_mob mob = { 0 };
	bool data_valid;

	surface = vmw_ioctl_surface_ref(fd, surface_handle,
					DRM_VMW_HANDLE_LEGACY);

	mob.handle = surface->base.handle;
	mob.size = surface->base.buffer_size;
	mob.map_handle = surface->base.buffer_map_handle;

	data_valid = verify_mob_data(fd, &mob);

	vmw_ioctl_surface_unref(fd, surface);

	return data_valid;
}

int igt_main()
{
	int32 fd1, fd2;
	const uint32 size = sizeof(data);
	SVGA3dSize surface_size = { .width = size, .height = 1, .depth = 1 };

	igt_fixture()
	{
		fd1 = drm_open_driver_render(DRIVER_VMWGFX);
		fd2 = drm_open_driver_render(DRIVER_VMWGFX);
		igt_require(fd1 != -1);
		igt_require(fd2 != -1);
	}

	igt_describe("Test prime transfers with explicit mobs.");
	igt_subtest("surface_prime_transfer_explicit_mob")
	{
		struct vmw_mob *mob;
		struct vmw_surface *surface;
		int32 surface_fd;
		uint32 surface_handle;

		mob = vmw_ioctl_mob_create(fd1, size);
		surface = vmw_ioctl_create_surface_full(
			fd1, SVGA3D_SURFACE_HINT_RENDERTARGET, SVGA3D_BUFFER, 0,
			SVGA3D_MS_PATTERN_NONE, SVGA3D_MS_QUALITY_NONE,
			SVGA3D_TEX_FILTER_NONE, 1, 1, surface_size, mob->handle,
			drm_vmw_surface_flag_shareable);

		write_to_mob(fd1, mob);

		surface_fd =
			prime_handle_to_fd_for_mmap(fd1, surface->base.handle);

		vmw_ioctl_mob_close_handle(fd1, mob);
		vmw_ioctl_surface_unref(fd1, surface);

		surface_handle = prime_fd_to_handle(fd2, surface_fd);
		close(surface_fd);

		igt_assert(ref_surface_and_check_contents(fd2, surface_handle));
	}

	igt_describe("Test prime transfers with implicit mobs.");
	igt_subtest("surface_prime_transfer_implicit_mob")
	{
		struct vmw_surface *surface;
		int32 surface_fd;
		uint32 surface_handle;

		surface = create_and_write_shareable_surface(fd1, surface_size);

		surface_fd =
			prime_handle_to_fd_for_mmap(fd1, surface->base.handle);

		vmw_ioctl_surface_unref(fd1, surface);

		surface_handle = prime_fd_to_handle(fd2, surface_fd);
		close(surface_fd);

		igt_assert(ref_surface_and_check_contents(fd2, surface_handle));
	}

	igt_describe("Test prime transfers with a fd dup.");
	igt_subtest("surface_prime_transfer_fd_dup")
	{
		int32 surface_fd1, surface_fd2;
		uint32 surface_handle;
		struct vmw_surface *surface;

		surface = create_and_write_shareable_surface(fd1, surface_size);

		surface_fd1 =
			prime_handle_to_fd_for_mmap(fd1, surface->base.handle);
		vmw_ioctl_surface_unref(fd1, surface);

		surface_fd2 = dup(surface_fd1);
		close(surface_fd1);

		surface_handle = prime_fd_to_handle(fd2, surface_fd2);
		close(surface_fd2);

		igt_assert(ref_surface_and_check_contents(fd2, surface_handle));
	}

	igt_describe("Test prime lifetime with 2 surfaces.");
	igt_subtest("surface_prime_transfer_two_surfaces")
	{
		int32 surface_fd;
		uint32 surface_handle1, surface_handle2;
		struct vmw_surface *surface1, *surface2;

		surface1 =
			create_and_write_shareable_surface(fd1, surface_size);
		surface2 =
			create_and_write_shareable_surface(fd1, surface_size);

		surface_fd =
			prime_handle_to_fd_for_mmap(fd1, surface1->base.handle);
		vmw_ioctl_surface_unref(fd1, surface1);

		surface_handle1 = prime_fd_to_handle(fd2, surface_fd);
		close(surface_fd);

		surface_fd =
			prime_handle_to_fd_for_mmap(fd1, surface2->base.handle);
		vmw_ioctl_surface_unref(fd1, surface2);

		surface_handle2 = prime_fd_to_handle(fd2, surface_fd);
		close(surface_fd);

		igt_assert(
			ref_surface_and_check_contents(fd2, surface_handle1));
		igt_assert(
			ref_surface_and_check_contents(fd2, surface_handle2));
	}

	igt_describe("Test prime transfers with multiple handles.");
	igt_subtest("surface_prime_transfer_single_surface_multiple_handle")
	{
		int32 surface_fd;
		uint32 surface_handle_old;
		uint32 surface_handle1, surface_handle2, surface_handle3;
		struct vmw_surface *surface;

		surface = create_and_write_shareable_surface(fd1, surface_size);
		surface_handle_old = surface->base.handle;

		surface_fd =
			prime_handle_to_fd_for_mmap(fd1, surface->base.handle);
		vmw_ioctl_surface_unref(fd1, surface);

		surface_handle1 = prime_fd_to_handle(fd1, surface_fd);
		surface_handle2 = prime_fd_to_handle(fd2, surface_fd);
		surface_handle3 = prime_fd_to_handle(fd2, surface_fd);
		close(surface_fd);

		igt_assert_eq_u32(surface_handle_old, surface_handle1);
		igt_assert_eq_u32(surface_handle2, surface_handle3);

		igt_assert(
			ref_surface_and_check_contents(fd1, surface_handle1));
		igt_assert(
			ref_surface_and_check_contents(fd2, surface_handle2));
	}

	igt_describe("Test repeated unrefs on a mob.");
	igt_subtest("mob_repeated_unref")
	{
		struct vmw_mob *mob;
		int i = 0;

		mob = vmw_ioctl_mob_create(fd1, size);
		write_to_mob(fd1, mob);

		/* Shouldn't crash on multiple invocations */
		for (i = 0; i < 3; i++) {
			struct drm_vmw_handle_close_arg arg = {
				.handle = mob->handle
			};
			drmCommandWrite(fd1, DRM_VMW_HANDLE_CLOSE, &arg,
					      sizeof(arg));
		}
		free(mob);
	}

	igt_describe("Test repeated unrefs on a surface.");
	igt_subtest("surface_repeated_unref")
	{
		struct vmw_surface *surface;
		int i = 0;

		surface = vmw_ioctl_create_surface_full(
			fd1, SVGA3D_SURFACE_HINT_RENDERTARGET, SVGA3D_BUFFER, 0,
			SVGA3D_MS_PATTERN_NONE, SVGA3D_MS_QUALITY_NONE,
			SVGA3D_TEX_FILTER_NONE, 1, 1, surface_size, SVGA3D_INVALID_ID,
			drm_vmw_surface_flag_shareable);

		/* Shouldn't crash on multiple invocations */
		for (i = 0; i < 3; i++) {
			struct drm_vmw_surface_arg s_arg = {
				.sid = surface->base.handle,
				.handle_type = DRM_VMW_HANDLE_LEGACY
			};
			drmCommandWrite(fd1, DRM_VMW_UNREF_SURFACE, &s_arg,
					sizeof(s_arg));
		}
		free(surface);
	}

	igt_describe("Test unref on a refed surface.");
	igt_subtest("surface_alloc_ref_unref")
	{
		struct vmw_surface *surface;
		struct vmw_surface *ref_surface;
		struct vmw_mob readback_mob = { 0 };

		surface = create_and_write_shareable_surface(fd1, surface_size);

		ref_surface = vmw_ioctl_surface_ref(fd1, surface->base.handle,
						    DRM_VMW_HANDLE_LEGACY);

		vmw_ioctl_surface_unref(fd1, surface);

		readback_mob.handle = ref_surface->base.handle;
		readback_mob.size = ref_surface->base.buffer_size;
		readback_mob.map_handle = ref_surface->base.buffer_map_handle;

		igt_assert(verify_mob_data(fd1, &readback_mob));

		vmw_ioctl_surface_unref(fd1, ref_surface);
	}

	igt_describe("Test refing a surface from the buffer handle.");
	igt_subtest("surface_buffer_ref")
	{
		struct vmw_surface *surfaces[NUM_SURFACES] = {0};
		struct vmw_surface *refs[NUM_SURFACES] = {0};
		struct vmw_surface *buf_refs[NUM_SURFACES] = {0};
		int i;
		SVGA3dSize surf_size;

		for (i = 0; i < NUM_SURFACES; ++i) {
			surf_size.width = 32 + i * 16;
			surf_size.height = 32 + i * 16;
			surf_size.depth = 1;

			surfaces[i] = vmw_create_surface_simple(fd1,
								SVGA3D_SURFACE_HINT_TEXTURE |
								SVGA3D_SURFACE_HINT_RENDERTARGET |
								SVGA3D_SURFACE_BIND_RENDER_TARGET,
								SVGA3D_R8G8B8A8_UNORM, surf_size,
								SVGA3D_INVALID_ID);
			igt_assert(surfaces[i]);
		}

		for (i = 0; i < NUM_SURFACES; ++i) {
			int prime_fd = prime_handle_to_fd_for_mmap(fd1,
								   surfaces[i]->base.handle);

			refs[i] = vmw_ioctl_surface_ref(fd1, prime_fd,
							DRM_VMW_HANDLE_PRIME);
			igt_assert_eq(surfaces[i]->base.handle,
				      refs[i]->base.handle);
			igt_assert_eq(surfaces[i]->base.backup_size,
				      refs[i]->base.backup_size);
			igt_assert_eq(surfaces[i]->base.buffer_size,
				      refs[i]->base.buffer_size);
			igt_assert_eq(surfaces[i]->base.buffer_map_handle,
				      refs[i]->base.buffer_map_handle);
			igt_assert_eq(surfaces[i]->params.base.format,
				      refs[i]->params.base.format);
		}

		for (i = 0; i < NUM_SURFACES; ++i) {
			int prime_fd = prime_handle_to_fd_for_mmap(fd1,
								   surfaces[i]->base.buffer_handle);

			buf_refs[i] = vmw_ioctl_surface_ref(fd1, prime_fd,
							    DRM_VMW_HANDLE_PRIME);
			igt_assert_eq(surfaces[i]->base.handle,
				      buf_refs[i]->base.handle);
			igt_assert_eq(surfaces[i]->base.backup_size,
				      buf_refs[i]->base.backup_size);
			igt_assert_eq(surfaces[i]->base.buffer_size,
				      buf_refs[i]->base.buffer_size);
			igt_assert_eq(surfaces[i]->base.buffer_map_handle,
				      buf_refs[i]->base.buffer_map_handle);
			igt_assert_eq(surfaces[i]->params.base.format,
				      buf_refs[i]->params.base.format);
		}

		for (i = 0; i < NUM_SURFACES; ++i) {
			vmw_ioctl_surface_unref(fd1, buf_refs[i]);
			vmw_ioctl_surface_unref(fd1, refs[i]);
			vmw_ioctl_surface_unref(fd1, surfaces[i]);
		}
	}

	igt_describe("Test refcounts on prime surfaces.");
	igt_subtest("surface_prime_refs") {
		struct vmw_surface *surfaces[NUM_SURFACES] = {0};
		int prime_fds[NUM_SURFACES] = {0};
		struct vmw_surface *refs[NUM_SURFACES] = {0};
		int i;
		SVGA3dSize surf_size;

		for (i = 0; i < NUM_SURFACES; ++i) {
			surf_size.width = 32 + i * 16;
			surf_size.height = 32 + i * 16;
			surf_size.depth = 1;

			surfaces[i] = vmw_create_surface_simple(fd1,
								SVGA3D_SURFACE_HINT_TEXTURE |
								SVGA3D_SURFACE_HINT_RENDERTARGET |
								SVGA3D_SURFACE_BIND_RENDER_TARGET,
								SVGA3D_R8G8B8A8_UNORM, surf_size,
								SVGA3D_INVALID_ID);
			igt_assert(surfaces[i]);
		}

		for (i = 0; i < NUM_SURFACES; ++i) {
			prime_fds[i] = prime_handle_to_fd(fd1,
							  surfaces[i]->base.handle);
			igt_assert_neq(prime_fds[i], 0);
			igt_assert_neq(prime_fds[i], -1);
			vmw_ioctl_surface_unref(fd1, surfaces[i]);
		}

		for (i = 0; i < NUM_SURFACES; ++i) {
			refs[i] = vmw_ioctl_surface_ref(fd1, prime_fds[i],
							DRM_VMW_HANDLE_PRIME);
			close(prime_fds[i]);
			igt_assert_neq(refs[i]->base.handle, 0);
			igt_assert_neq(refs[i]->base.backup_size, 0);
			igt_assert_neq(refs[i]->base.buffer_size, 0);
		}
	}

	igt_describe("Test refcounts on prime surfaces with buffer handles.");
	igt_subtest("surface_buffer_prime_refs") {
		struct vmw_surface *surfaces[NUM_SURFACES] = {0};
		int prime_fds[NUM_SURFACES] = {0};
		struct vmw_surface *refs[NUM_SURFACES] = {0};
		int i;
		SVGA3dSize surf_size;

		for (i = 0; i < NUM_SURFACES; ++i) {
			surf_size.width = 32 + i * 16;
			surf_size.height = 32 + i * 16;
			surf_size.depth = 1;

			surfaces[i] = vmw_create_surface_simple(fd1,
								SVGA3D_SURFACE_HINT_TEXTURE |
								SVGA3D_SURFACE_HINT_RENDERTARGET |
								SVGA3D_SURFACE_BIND_RENDER_TARGET,
								SVGA3D_R8G8B8A8_UNORM, surf_size,
								SVGA3D_INVALID_ID);
			igt_assert(surfaces[i]);
		}

		for (i = 0; i < NUM_SURFACES; ++i) {
			prime_fds[i] = prime_handle_to_fd(fd1,
							  surfaces[i]->base.buffer_handle);
			igt_assert_neq(prime_fds[i], 0);
			igt_assert_neq(prime_fds[i], -1);
			vmw_ioctl_surface_unref(fd1, surfaces[i]);
		}

		for (i = 0; i < NUM_SURFACES; ++i) {
			refs[i] = vmw_ioctl_surface_ref(fd1, prime_fds[i],
							DRM_VMW_HANDLE_PRIME);
			close(prime_fds[i]);
			igt_assert_neq(refs[i]->base.handle, 0);
			igt_assert_neq(refs[i]->base.backup_size, 0);
			igt_assert_neq(refs[i]->base.buffer_size, 0);
		}
	}

	igt_fixture() {
		drm_close_driver(fd1);
		drm_close_driver(fd2);
	}
}
