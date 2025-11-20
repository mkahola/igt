// SPDX-License-Identifier: GPL-2.0 OR MIT
/*
 * Copyright (c) 2024 Broadcom. All Rights Reserved. The term
 * “Broadcom” refers to Broadcom Inc. and/or its subsidiaries.
 */

#include "igt_kms.h"
#include "igt_vmwgfx.h"

IGT_TEST_DESCRIPTION("Check whether basic DRM prime and dma-buf work correctly.");

static void replace_with_prime_rt(struct vmw_svga_device *device,
				  int32 context_id,
				  uint32 prime_fd,
				  uint32 buffer_handle,
				  struct vmw_default_objects *objects,
				  const SVGA3dSize *rt_size)
{
	struct vmw_execbuf *cmd_buf;
	SVGA3dRenderTargetViewDesc rtv_desc = { 0 };
	SVGA3dCmdDXDefineRenderTargetView rt_view_define_cmd = { 0 };
	SVGA3dCmdDXDestroyRenderTargetView rt_view_cmd = {
		.renderTargetViewId = objects->color_rt_id
	};

	vmw_ioctl_surface_unref(device->drm_fd, objects->color_rt);
	objects->color_rt = vmw_ioctl_surface_ref(device->drm_fd,
						  prime_fd,
						  DRM_VMW_HANDLE_PRIME);

	cmd_buf = vmw_execbuf_create(device->drm_fd, context_id);

	rtv_desc.tex.arraySize = 1;
	rtv_desc.tex.firstArraySlice = 0;
	rtv_desc.tex.mipSlice = 0;
	vmw_bitvector_find_next_bit(device->rt_view_bv,
				    &rt_view_define_cmd.renderTargetViewId);
	rt_view_define_cmd.sid = objects->color_rt->base.handle;
	rt_view_define_cmd.format = SVGA3D_B8G8R8X8_UNORM;
	rt_view_define_cmd.resourceDimension = SVGA3D_RESOURCE_TEXTURE2D;
	rt_view_define_cmd.desc = rtv_desc;
	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DESTROY_RENDERTARGET_VIEW,
			   &rt_view_cmd, sizeof(rt_view_cmd), NULL, 0);
	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DEFINE_RENDERTARGET_VIEW,
			   &rt_view_define_cmd, sizeof(rt_view_define_cmd),
			   NULL, 0);
	vmw_execbuf_submit(cmd_buf, NULL);
	vmw_execbuf_destroy(cmd_buf);

	vmw_bitvector_free_bit(device->rt_view_bv, objects->color_rt_id);
	objects->color_rt_id = rt_view_define_cmd.renderTargetViewId;
}

static void replace_with_surface(struct vmw_svga_device *device,
				 int32 context_id, struct vmw_surface *surf,
				 struct vmw_default_objects *objects,
				 const SVGA3dSize *rt_size)
{
	struct vmw_execbuf *cmd_buf;
	SVGA3dRenderTargetViewDesc rtv_desc = { 0 };
	SVGA3dCmdDXDefineRenderTargetView rt_view_define_cmd = { 0 };
	SVGA3dCmdDXDestroyRenderTargetView rt_view_cmd = {
		.renderTargetViewId = objects->color_rt_id
	};

	vmw_ioctl_surface_unref(device->drm_fd, objects->color_rt);
	objects->color_rt = vmw_ioctl_surface_ref(device->drm_fd,
						  surf->base.handle,
						  DRM_VMW_HANDLE_LEGACY);

	cmd_buf = vmw_execbuf_create(device->drm_fd, context_id);

	rtv_desc.tex.arraySize = 1;
	rtv_desc.tex.firstArraySlice = 0;
	rtv_desc.tex.mipSlice = 0;
	vmw_bitvector_find_next_bit(device->rt_view_bv,
				    &rt_view_define_cmd.renderTargetViewId);
	rt_view_define_cmd.sid = objects->color_rt->base.handle;
	rt_view_define_cmd.format = SVGA3D_B8G8R8X8_UNORM;
	rt_view_define_cmd.resourceDimension = SVGA3D_RESOURCE_TEXTURE2D;
	rt_view_define_cmd.desc = rtv_desc;
	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DESTROY_RENDERTARGET_VIEW,
			   &rt_view_cmd, sizeof(rt_view_cmd), NULL, 0);
	vmw_execbuf_append(cmd_buf, SVGA_3D_CMD_DX_DEFINE_RENDERTARGET_VIEW,
			   &rt_view_define_cmd, sizeof(rt_view_define_cmd),
			   NULL, 0);
	vmw_execbuf_submit(cmd_buf, NULL);
	vmw_execbuf_destroy(cmd_buf);

	vmw_bitvector_free_bit(device->rt_view_bv, objects->color_rt_id);
	objects->color_rt_id = rt_view_define_cmd.renderTargetViewId;
}

static void draw_triangle_map_gem(struct vmw_svga_device *mdevice,
				  struct vmw_svga_device *device, int32 cid)
{
	struct vmw_default_objects objects;
	void *ptr;
	bool save_status;
	int fd, imported_handle, gem_handle;
	uint64_t gem_size;

	gem_handle = kmstest_dumb_create(mdevice->drm_fd,
					 vmw_default_rect_size.width,
					 vmw_default_rect_size.height, 32, NULL,
					 &gem_size);
	fd = prime_handle_to_fd(mdevice->drm_fd, gem_handle);
	imported_handle = prime_fd_to_handle(device->drm_fd, fd);

	vmw_create_default_objects(device, cid, &objects,
				   &vmw_default_rect_size);
	replace_with_prime_rt(device, cid, fd, imported_handle, &objects,
			      &vmw_default_rect_size);
	vmw_triangle_draw(device, cid, &objects,
			  vmw_triangle_draw_flags_sync |
				  vmw_triangle_draw_flags_readback);

	ptr = kmstest_dumb_map_buffer(mdevice->drm_fd, gem_handle, gem_size,
				      PROT_READ);

	save_status = vmw_save_data_as_png(objects.color_rt, ptr,
					   "vmw_prime_tri1.png");
	igt_assert(save_status);

	munmap(ptr, gem_size);

	vmw_destroy_default_objects(device, &objects);
	kmstest_dumb_destroy(mdevice->drm_fd, gem_handle);
}

static void draw_triangle_map_dmabuf(struct vmw_svga_device *mdevice,
				     struct vmw_svga_device *device, int32 cid)
{
	struct vmw_default_objects objects;
	void *ptr;
	bool save_status;
	int fd, imported_handle, gem_handle;
	uint64_t gem_size;

	gem_handle = kmstest_dumb_create(mdevice->drm_fd,
					 vmw_default_rect_size.width,
					 vmw_default_rect_size.height, 32, NULL,
					 &gem_size);
	fd = prime_handle_to_fd_for_mmap(mdevice->drm_fd, gem_handle);
	kmstest_dumb_destroy(mdevice->drm_fd, gem_handle);
	imported_handle = prime_fd_to_handle(device->drm_fd, fd);

	vmw_create_default_objects(device, cid, &objects,
				   &vmw_default_rect_size);
	replace_with_prime_rt(device, cid, fd, imported_handle, &objects,
			      &vmw_default_rect_size);
	vmw_triangle_draw(device, cid, &objects,
			  vmw_triangle_draw_flags_sync |
				  vmw_triangle_draw_flags_readback);

	ptr = mmap(NULL, gem_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

	save_status = vmw_save_data_as_png(objects.color_rt, ptr,
					   "vmw_prime_tri2.png");
	igt_assert(save_status);

	munmap(ptr, gem_size);
	close(fd);

	vmw_destroy_default_objects(device, &objects);
}

struct gpu_process_t {
	struct vmw_svga_device mdevice;
	struct vmw_svga_device rdevice;
	int32 cid;
	igt_display_t display;
	struct igt_fb fb;
	struct vmw_surface *fb_surface;
	igt_output_t *output;
	igt_plane_t *primary;
	enum pipe pipe;
	igt_crc_t reference_tri_crc;
};

static void cleanup_crtc(struct gpu_process_t *gpu)
{
	igt_display_t *display = &gpu->display;
	igt_output_t *output = gpu->output;

	igt_plane_set_fb(gpu->primary, NULL);

	igt_output_set_pipe(output, PIPE_NONE);
	igt_display_commit(display);

	igt_remove_fb(gpu->mdevice.drm_fd, &gpu->fb);
	if (gpu->fb_surface) {
		vmw_ioctl_surface_unref(gpu->mdevice.drm_fd, gpu->fb_surface);
		gpu->fb_surface = NULL;
	}
}

static void prepare_crtc(struct gpu_process_t *gpu)
{
	igt_display_t *display = &gpu->display;
	igt_output_t *output = gpu->output;
	drmModeModeInfo *mode;
	int ret;

	/* select the pipe we want to use */
	igt_output_set_pipe(output, gpu->pipe);

	mode = igt_output_get_mode(output);

	/* create a white fb and flip to it */
	igt_create_color_fb(gpu->mdevice.drm_fd, mode->hdisplay, mode->vdisplay,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 1.0,
			    1.0, 1.0, &gpu->fb);

	gpu->primary =
		igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_plane_set_fb(gpu->primary, &gpu->fb);
	ret = igt_display_commit(display);
	igt_assert(ret == 0);
}

static void prepare_crtc_surface(struct gpu_process_t *gpu)
{
	igt_display_t *display = &gpu->display;
	igt_output_t *output = gpu->output;
	drmModeModeInfo *mode;
	int ret;
	int prime_fd;

	/* select the pipe we want to use */
	igt_output_set_pipe(output, gpu->pipe);

	mode = igt_output_get_mode(output);

	/* create a white fb and flip to it */
	igt_create_color_fb(gpu->mdevice.drm_fd, mode->hdisplay, mode->vdisplay,
			    DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR, 1.0,
			    1.0, 1.0, &gpu->fb);
	do_or_die(drmModeRmFB(gpu->mdevice.drm_fd, gpu->fb.fb_id));

	prime_fd = prime_handle_to_fd(gpu->mdevice.drm_fd, gpu->fb.gem_handle);
	gpu->fb_surface = vmw_ioctl_surface_ref(gpu->mdevice.drm_fd,
						prime_fd,
						DRM_VMW_HANDLE_PRIME);
	close(prime_fd);

	do_or_die(__kms_addfb(gpu->fb.fd, gpu->fb_surface->base.handle,
			      gpu->fb.width, gpu->fb.height, gpu->fb.drm_format,
			      gpu->fb.modifier, gpu->fb.strides,
			      gpu->fb.offsets, gpu->fb.num_planes, 0,
			      &gpu->fb.fb_id));

	gpu->primary =
		igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	igt_plane_set_fb(gpu->primary, &gpu->fb);
	ret = igt_display_commit(display);
	igt_assert(ret == 0);
}

static void run_renderer(struct vmw_svga_device *device, int prime_fd, int cid,
			 int fb_size, int width, int height,
			 uint32_t draw_flags)
{
	struct vmw_default_objects objects;
	int imported_handle;
	SVGA3dSize rt_size = { 0 };

	rt_size.width = width;
	rt_size.height = height;
	rt_size.depth = 1;

	imported_handle = prime_fd_to_handle(device->drm_fd, prime_fd);

	vmw_create_default_objects(device, cid, &objects, &rt_size);
	replace_with_prime_rt(device, cid, prime_fd, imported_handle, &objects,
			      &rt_size);
	vmw_triangle_draw(device, cid, &objects, draw_flags);

	vmw_destroy_default_objects(device, &objects);
}

static void draw_triangle_3d(struct gpu_process_t *gpu, uint32_t draw_flags)
{
	igt_display_t *display = &gpu->display;
	igt_output_t *output;
	enum pipe pipe;
	igt_pipe_crc_t *pipe_crc;
	igt_crc_t blank_crc, tri_crc;
	char *blank_crc_str, *tri_crc_str;
	bool crc_equal;

	for_each_pipe_with_valid_output(display, pipe, output) {
		int prime_fd;

		gpu->output = output;
		gpu->pipe = pipe;

		prepare_crtc(gpu);
		pipe_crc = igt_pipe_crc_new(gpu->mdevice.drm_fd, pipe,
					    IGT_PIPE_CRC_SOURCE_AUTO);
		igt_pipe_crc_collect_crc(pipe_crc, &blank_crc);

		prime_fd = prime_handle_to_fd_for_mmap(gpu->mdevice.drm_fd,
						       gpu->fb.gem_handle);
		igt_skip_on(prime_fd == -1 && errno == EINVAL);

		igt_fork(renderer_no, 1) {
			run_renderer(&gpu->rdevice, prime_fd, gpu->cid,
				     gpu->fb.size, gpu->fb.width,
				     gpu->fb.height, draw_flags);
		}
		igt_waitchildren();

		igt_plane_set_fb(gpu->primary, &gpu->fb);
		igt_display_commit(display);
		igt_pipe_crc_collect_crc(pipe_crc, &tri_crc);
		blank_crc_str = igt_crc_to_string(&blank_crc);
		tri_crc_str = igt_crc_to_string(&tri_crc);

		igt_debug("Blank crc = '%s', tri = '%s\n'", blank_crc_str,
			  tri_crc_str);
		crc_equal = igt_check_crc_equal(&blank_crc, &tri_crc);
		igt_assert_f(!crc_equal,
			     "Blank and rendered triangle CRCs should be different.\n");
		if (draw_flags == (vmw_triangle_draw_flags_sync |
				   vmw_triangle_draw_flags_readback)) {
			memcpy(&gpu->reference_tri_crc, &tri_crc,
			       sizeof(gpu->reference_tri_crc));
		} else if (gpu->reference_tri_crc.has_valid_frame) {
			igt_assert_crc_equal(&gpu->reference_tri_crc, &tri_crc);
		}

		igt_debug_wait_for_keypress("paint");

		close(prime_fd);
		igt_pipe_crc_free(pipe_crc);
		cleanup_crtc(gpu);
		free(blank_crc_str);
		free(tri_crc_str);
		/* once is enough */
		return;
	}

	igt_skip("no valid crtc/connector combinations found\n");
}

static void draw_dumb_buffer(struct gpu_process_t *gpu)
{
	igt_display_t *display = &gpu->display;
	igt_output_t *output;
	enum pipe pipe;
	igt_pipe_crc_t *pipe_crc;
	igt_crc_t blank_crc, red_crc, blue_crc, tri_crc, red2_crc;
	char *blank_crc_str, *red_crc_str, *red2_crc_str, *blue_crc_str,
		*tri_crc_str;
	bool crc_equal;
	cairo_t *cr;
	struct vmw_default_objects objects = { 0 };
	int32_t cid = vmw_ioctl_context_create(gpu->mdevice.drm_fd);

	for_each_pipe_with_valid_output(display, pipe, output) {
		gpu->output = output;
		gpu->pipe = pipe;

		prepare_crtc_surface(gpu);
		pipe_crc = igt_pipe_crc_new(gpu->mdevice.drm_fd, pipe,
					    IGT_PIPE_CRC_SOURCE_AUTO);
		igt_pipe_crc_collect_crc(pipe_crc, &blank_crc);

		cr = igt_get_cairo_ctx(gpu->mdevice.drm_fd, &gpu->fb);
		igt_paint_color(cr, 0, 0, gpu->fb.width, gpu->fb.height, 1.0, 0,
				0);
		igt_put_cairo_ctx(cr);
		igt_plane_set_fb(gpu->primary, &gpu->fb);
		igt_display_commit(display);
		igt_pipe_crc_collect_crc(pipe_crc, &red_crc);

		cr = igt_get_cairo_ctx(gpu->mdevice.drm_fd, &gpu->fb);
		igt_paint_color(cr, 0, 0, gpu->fb.width, gpu->fb.height, 0, 0,
				1.0);
		igt_put_cairo_ctx(cr);
		igt_plane_set_fb(gpu->primary, &gpu->fb);
		igt_display_commit(display);
		igt_pipe_crc_collect_crc(pipe_crc, &blue_crc);

		{
			const SVGA3dSize size = { gpu->fb.width, gpu->fb.height,
						  1 };
			vmw_create_default_objects(&gpu->mdevice, cid, &objects,
						   &size);
			replace_with_surface(&gpu->mdevice, cid,
					     gpu->fb_surface, &objects, &size);
			vmw_triangle_draw(&gpu->mdevice, cid, &objects, 0);
		}
		igt_plane_set_fb(gpu->primary, &gpu->fb);
		igt_display_commit(display);
		igt_pipe_crc_collect_crc(pipe_crc, &tri_crc);
		igt_debug_wait_for_keypress("paint");

		cr = igt_get_cairo_ctx(gpu->mdevice.drm_fd, &gpu->fb);
		igt_paint_color(cr, 0, 0, gpu->fb.width, gpu->fb.height, 1.0, 0,
				0);
		igt_put_cairo_ctx(cr);
		igt_plane_set_fb(gpu->primary, &gpu->fb);
		igt_display_commit(display);
		igt_pipe_crc_collect_crc(pipe_crc, &red2_crc);

		blank_crc_str = igt_crc_to_string(&blank_crc);
		red_crc_str = igt_crc_to_string(&red_crc);
		red2_crc_str = igt_crc_to_string(&red2_crc);
		blue_crc_str = igt_crc_to_string(&blue_crc);
		tri_crc_str = igt_crc_to_string(&tri_crc);

		igt_debug("Blank crc = '%s', red = '%s', red2 = '%s', blue = '%s', tri = '%s'\n",
			  blank_crc_str, red_crc_str, red2_crc_str, blue_crc_str,
			  tri_crc_str);
		crc_equal = igt_check_crc_equal(&blank_crc, &red_crc);
		igt_assert_f(!crc_equal,
			     "Blank and red CRCs should be different.\n");
		crc_equal = igt_check_crc_equal(&red_crc, &blue_crc);
		igt_assert_f(!crc_equal,
			     "Red and blue CRCs should be different.\n");
		crc_equal = igt_check_crc_equal(&red_crc, &tri_crc);
		igt_assert_f(!crc_equal,
			     "Red and tri CRCs should be different.\n");
		crc_equal = igt_check_crc_equal(&blue_crc, &tri_crc);
		igt_assert_f(!crc_equal,
			     "Blue and tri CRCs should be different.\n");

		crc_equal = igt_check_crc_equal(&red_crc, &red2_crc);
		igt_assert_f(crc_equal, "Red CRCs should be the same.\n");

		vmw_destroy_default_objects(&gpu->mdevice, &objects);
		vmw_ioctl_context_destroy(gpu->mdevice.drm_fd, cid);
		igt_pipe_crc_free(pipe_crc);
		cleanup_crtc(gpu);
		free(blank_crc_str);
		free(red_crc_str);
		free(blue_crc_str);
		/* once is enough */
		return;
	}
}

static const uint32_t pattern[] = {
	0xff000000, 0x00ff0000, 0x0000ff00, 0x000000ff, 0x000000ff, 0x0000ff00,
	0x00ff0000, 0xff000000, 0x00ffff00, 0xff0000ff, 0x00ff00ff, 0xff00ff00,
	0xff0000ff, 0x00ff00ff, 0x00ffff00, 0xff00ff00
};

static void test_vgem(struct gpu_process_t *gpu, int vgem_fd)
{
	int dma_buf_fd;
	uint32_t *ptr;
	struct dumb_buffer {
		uint32_t handle;
		uint32_t stride;
		uint64_t size;
	} vgem_buffer;
	uint32_t vmw_buffer_handle;

	vgem_buffer.handle = kmstest_dumb_create(vgem_fd, 64, 64, 32,
						 &vgem_buffer.stride,
						 &vgem_buffer.size);
	ptr = kmstest_dumb_map_buffer(vgem_fd, vgem_buffer.handle,
				      vgem_buffer.size, PROT_WRITE);
	igt_assert(ptr != MAP_FAILED);
	igt_assert(ptr);
	igt_assert(vgem_buffer.size > sizeof(pattern));
	memcpy(ptr, pattern, sizeof(pattern));
	munmap(ptr, vgem_buffer.size);

	dma_buf_fd = prime_handle_to_fd_for_mmap(vgem_fd, vgem_buffer.handle);

	/* Skip if DRM_RDWR is not supported */
	igt_skip_on(errno == EINVAL);

	/* Check correctness of map using write protection (PROT_WRITE) */
	ptr = mmap(NULL, vgem_buffer.size, PROT_READ, MAP_SHARED, dma_buf_fd,
		   0);
	igt_assert(ptr != MAP_FAILED);

	/* Check pattern correctness */
	igt_assert(memcmp(ptr, pattern, sizeof(pattern)) == 0);

	munmap(ptr, vgem_buffer.size);
	close(dma_buf_fd);

	dma_buf_fd = prime_handle_to_fd(vgem_fd, vgem_buffer.handle);
	vmw_buffer_handle = prime_fd_to_handle(gpu->mdevice.drm_fd, dma_buf_fd);
	igt_assert(vmw_buffer_handle >= 0);
	ptr = kmstest_dumb_map_buffer(gpu->mdevice.drm_fd, vmw_buffer_handle,
				      vgem_buffer.size, PROT_READ);
	igt_assert(ptr != MAP_FAILED);
	igt_assert(ptr);
	igt_assert(memcmp(ptr, pattern, sizeof(pattern)) == 0);
	munmap(ptr, vgem_buffer.size);
	close(dma_buf_fd);

	kmstest_dumb_destroy(vgem_fd, vgem_buffer.handle);
	gem_close(gpu->mdevice.drm_fd, vmw_buffer_handle);
}

igt_main
{
	struct gpu_process_t gpu = { 0 };
	int second_fd_vgem = -1;

	igt_fixture() {
		vmw_svga_device_init(&gpu.mdevice, vmw_svga_device_node_master);
		vmw_svga_device_init(&gpu.rdevice, vmw_svga_device_node_render);
		igt_require(gpu.mdevice.drm_fd != -1);
		igt_require(gpu.rdevice.drm_fd != -1);

		gpu.cid = vmw_ioctl_context_create(gpu.rdevice.drm_fd);
		igt_require(gpu.cid != SVGA3D_INVALID_ID);

		kmstest_set_vt_graphics_mode();

		igt_require_pipe_crc(gpu.mdevice.drm_fd);

		igt_display_require(&gpu.display, gpu.mdevice.drm_fd);
	}

	igt_describe("Tests prime rendering triangle with gem mmap.");
	igt_subtest("tri-map-gem") {
		draw_triangle_map_gem(&gpu.mdevice, &gpu.rdevice, gpu.cid);
	}

	igt_describe("Tests prime rendering triangle with dmabuf mmap.");
	igt_subtest("tri-map-dmabuf") {
		draw_triangle_map_dmabuf(&gpu.mdevice, &gpu.rdevice, gpu.cid);
	}

	igt_describe("Tests dumb buffer and fb synchronizations.");
	igt_subtest("draw-dumb-buffer") {
		draw_dumb_buffer(&gpu);
	}

	igt_describe("Tests synchronous/readback prime rendering triangle while buffer bound to fb");
	igt_subtest("buffer-surface-fb-sharing-sync-readback") {
		draw_triangle_3d(&gpu,
				 vmw_triangle_draw_flags_sync |
					 vmw_triangle_draw_flags_readback);
	}

	igt_describe("Tests synchronous prime rendering triangle while buffer bound to fb");
	igt_subtest("buffer-surface-fb-sharing-sync") {
		draw_triangle_3d(&gpu, vmw_triangle_draw_flags_sync);
	}

	igt_describe("Tests prime rendering triangle while buffer bound to fb");
	igt_subtest("buffer-surface-fb-sharing") {
		draw_triangle_3d(&gpu, vmw_triangle_draw_flags_none);
	}

	igt_describe("VGEM subtests");
	igt_subtest_group {
		igt_fixture() {
			second_fd_vgem =
				__drm_open_driver_another(1, DRIVER_VGEM);
			igt_require(second_fd_vgem >= 0);
		}

		igt_describe("Make a dumb color buffer, export to another device and"
			     " compare the CRCs with a buffer native to that device");
		igt_subtest("basic-vgem") {
			test_vgem(&gpu, second_fd_vgem);
		}

		igt_fixture() {
			drm_close_driver(second_fd_vgem);
		}
	}

	igt_fixture() {
		vmw_ioctl_context_destroy(gpu.rdevice.drm_fd, gpu.cid);
		igt_display_fini(&gpu.display);
		vmw_svga_device_fini(&gpu.rdevice);
		vmw_svga_device_fini(&gpu.mdevice);
	}
}
