// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Red Hat Inc.
 *
 * Authors: Dorinda Bassey <dbassey@redhat.com>
 */

/**
 * TEST: drm virtgpu ioctls
 * Description: Testing of the virtIO-GPU driver DRM ioctls
 * Category: Core
 * Mega feature: General Core features
 * Sub-category:  virtIO-GPU DRM ioctls
 * Functionality: drm_ioctls
 * Feature: Virtualization graphics support
 * Test category: functionality test
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>

#include "drm.h"
#include "virtgpu_drm.h"
#include "igt.h"

/**
 * SUBTEST: drm-virtgpu-map
 *
 * SUBTEST: drm-virtgpu-execbuffer
 *
 * SUBTEST: drm-virtgpu-resource-info
 *
 * SUBTEST: drm-virtgpu-3d-transfer-to-host
 *
 * SUBTEST: drm-virtgpu-3d-transfer-from-host
 *
 * SUBTEST: drm-virtgpu-3d-wait
 *
 * SUBTEST: drm-virtgpu-resource-create
 *
 * SUBTEST: drm-virtgpu-resource-create-blob
 *
 * SUBTEST: drm-virtgpu-get-caps

 * SUBTEST: drm-virtgpu-context-init
 *
 * SUBTEST: drm-virtgpu-getparam
 */

IGT_TEST_DESCRIPTION("Testing of the virtIO-GPU driver DRM ioctls");

#define CAPS_BUFFER_SIZE 4096
#define MAX_CARDS 16

static int drm_fd;
static struct drm_virtgpu_resource_create args;
static bool resource_created;
static bool test_gfxstream;

static void setup(void)
{
	const char *env = getenv("TEST_GFXSTREAM_CAPSET");

	if (env && strcmp(env, "1") == 0)
		test_gfxstream = true;
}

static int open_virtgpu_device(void)
{
	drmVersionPtr version;
	int opened_devices = 0;

	for (int i = 0; i < MAX_CARDS; i++) {
		char path[64];

		snprintf(path, sizeof(path), "/dev/dri/card%d", i);
		drm_fd = open(path, O_RDWR | O_CLOEXEC);
		if (drm_fd < 0)
			continue;

		opened_devices++;

		version = drmGetVersion(drm_fd);
		if (version && strcmp(version->name, "virtio_gpu") == 0) {
			drmFreeVersion(version);
			igt_info("Found virtio_gpu device: %s\n", path);

			return drm_fd;
		}

		drmFreeVersion(version);
		close(drm_fd);
	}
	igt_info("No virtio_gpu device found, total DRM devices opened: %d\n", opened_devices);

	return -1;
}

static int test_capset(int fd, uint32_t capset_id)
{
	u8 *caps_buf;
	struct drm_virtgpu_get_caps caps;
	int ret;
	int i;

	caps_buf = calloc(1, CAPS_BUFFER_SIZE);
	igt_require(caps_buf);

	memset(&caps, 0, sizeof(caps));
	caps.cap_set_id = capset_id;
	caps.size = CAPS_BUFFER_SIZE;
	caps.addr = (uintptr_t)caps_buf;

	ret = ioctl(fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &caps);
	if (ret == 0) {
		igt_info("Capset ID %u: SUCCESS\n", capset_id);
		igt_info("  Reported size: %u\n", caps.size);
		igt_info("  First 16 bytes: ");
		for (i = 0; i < 16; i++)
			igt_info("%02x ", caps_buf[i]);
		igt_info("\n");
		free(caps_buf);

		return 0;
	}
	igt_info("Capset ID %u: FAILED - %m\n", capset_id);
	free(caps_buf);

	return -errno;
}

static void create_resource_if_needed(void)
{
	int ret;

	if (resource_created)
		return;

	memset(&args, 0, sizeof(args));
	args.target = 2;
	args.format = 67;
	args.bind = (1 << 0);
	args.width = 64;
	args.height = 64;
	args.depth = 1;
	args.array_size = 1;

	ret = ioctl(drm_fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE, &args);
	igt_assert_f(ret == 0, "RESOURCE_CREATE failed: %s\n", strerror(errno));
	igt_assert_neq(args.res_handle, 0);

	resource_created = true;
	igt_info("Created resource: res_handle=%u, bo_handle=%u\n",
		 args.res_handle, args.bo_handle);
}

int igt_main() {
	void *caps_buf = NULL;

	igt_fixture() {
		drm_fd = open_virtgpu_device();
		igt_require(drm_fd >= 0);

		caps_buf = calloc(1, CAPS_BUFFER_SIZE);
		igt_require(caps_buf);
	}

	igt_describe
	    ("Maps a buffer object and tests read/write access via mmap.");
	igt_subtest("drm-virtgpu-map") {
		struct drm_virtgpu_map map = { };
		void *map_ptr;
		int ret;

		create_resource_if_needed();
		memset(&map, 0, sizeof(map));
		map.handle = args.bo_handle;

		/* Request mmap offset */
		ret = ioctl(drm_fd, DRM_IOCTL_VIRTGPU_MAP, &map);
		igt_assert_f(!ret, "MAP ioctl failed: %m\n");
		igt_assert(map.offset);

		/* Try mmap */
		map_ptr =
		    mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd,
			 map.offset);
		igt_assert(map_ptr != MAP_FAILED);

		igt_info("Successfully mmap'ed BO: offset=0x%" PRIx64 "\n",
			 (unsigned long)map.offset);

		/* Simple test: write and read */
		memset(map_ptr, 0xaa, 4096);
		igt_assert(((uint8_t *)map_ptr)[0] == 0xaa);

		munmap(map_ptr, 4096);
	}

	igt_describe("Submits a dummy execbuffer to the GPU.");
	igt_subtest("drm-virtgpu-execbuffer") {
		struct drm_virtgpu_execbuffer execbuf = { };
		u32 handles[1];
		int ret;

		create_resource_if_needed();
		handles[0] = args.bo_handle;

		/* Submit dummy execbuffer */
		execbuf.flags = 0;
		execbuf.size = 0;       /* No command */
		execbuf.command = 0;    /* No command buffer */
		execbuf.bo_handles = (uintptr_t)handles;
		execbuf.num_bo_handles = 1;
		execbuf.fence_fd = -1;  /* No fence */
		execbuf.ring_idx = 0;   /* Default ring */

		ret = ioctl(drm_fd, DRM_IOCTL_VIRTGPU_EXECBUFFER, &execbuf);
		igt_assert_f(!ret, "EXECBUFFER ioctl failed: %m\n");

		igt_info("EXECBUFFER submitted successfully.\n");
	}

	igt_describe
	    ("Validates that the GPU resource info ioctl returns expected metadata.");
	igt_subtest("drm-virtgpu-resource-info") {
		struct drm_virtgpu_resource_info info;
		int ret;

		create_resource_if_needed();
		memset(&info, 0, sizeof(info));
		info.bo_handle = args.bo_handle;

		ret = ioctl(drm_fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &info);
		igt_assert_f(!ret, "RESOURCE_INFO failed: %m\n");
		igt_assert_eq(info.res_handle, args.res_handle);
		igt_assert(info.size > 0);

		igt_info("Queried resource info:\n");
		igt_info("  size:      %u bytes\n", info.size);
		igt_info("  res_handle %u\n", info.res_handle);
		igt_info("  blob_mem:  %u\n", info.blob_mem);
	}

	igt_describe
	    ("Transfers buffer contents from guest memory to the host.");
	igt_subtest("drm-virtgpu-3d-transfer-to-host") {
		int ret;
		struct drm_virtgpu_3d_transfer_to_host xfer;

		create_resource_if_needed();

		memset(&xfer, 0, sizeof(xfer));
		xfer.bo_handle = args.bo_handle;
		xfer.box.x = 0;
		xfer.box.y = 0;
		xfer.box.z = 0;
		xfer.box.w = args.width;
		xfer.box.h = args.height;
		xfer.box.d = 1;
		xfer.level = 0;
		xfer.offset = 0;
		xfer.stride = 0;
		xfer.layer_stride = 0;

		ret = ioctl(drm_fd, DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST, &xfer);
		igt_assert_f(!ret, "TRANSFER_TO_HOST failed: %m\n");
		igt_info("TRANSFER_TO_HOST completed\n");
	}

	igt_describe("Transfers buffer contents from the host to guest memory.");
	igt_subtest("drm-virtgpu-3d-transfer-from-host")
	{
		int ret;
		struct drm_virtgpu_3d_transfer_from_host xfer_in;

		create_resource_if_needed();
		memset(&xfer_in, 0, sizeof(xfer_in));
		xfer_in.bo_handle = args.bo_handle;
		xfer_in.box.x = 0;
		xfer_in.box.y = 0;
		xfer_in.box.z = 0;
		xfer_in.box.w = args.width;
		xfer_in.box.h = args.height;
		xfer_in.box.d = 1;
		xfer_in.level = 0;
		xfer_in.offset = 0;
		xfer_in.stride = 0;
		xfer_in.layer_stride = 0;

		ret = ioctl(drm_fd, DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST, &xfer_in);
		igt_assert_f(!ret, "TRANSFER_FROM_HOST failed: %m\n");
		igt_info("TRANSFER_FROM_HOST completed\n");
	}

	igt_describe
	    ("Waits for a GPU operation to complete on a specific resource.");
	igt_subtest("drm-virtgpu-3d-wait") {
		struct drm_virtgpu_3d_wait wait = {.handle = args.bo_handle };

		int ret = ioctl(drm_fd, DRM_IOCTL_VIRTGPU_WAIT, &wait);

		if (ret == 0)
			igt_info("DRM_IOCTL_VIRTGPU_WAIT succeeded: GPU operations on resource handle %u have completed.\n",
				 wait.handle);
		else
			igt_info("DRM_IOCTL_VIRTGPU_WAIT failed on resource handle %u as expected: %m\n",
				 wait.handle);
	}

	igt_describe
		("Creates a standard 2D GPU resource using RESOURCE_CREATE ioctl.");
	igt_subtest("drm-virtgpu-resource-create") {
		create_resource_if_needed();
	}

	igt_describe
	    ("Creates a GPU resource using the blob interface with memory flags.");
	igt_subtest("drm-virtgpu-resource-create-blob") {
		struct drm_virtgpu_resource_create_blob blob = {
			.blob_mem = VIRTGPU_BLOB_MEM_GUEST,
			.blob_flags =
			    VIRTGPU_BLOB_FLAG_USE_MAPPABLE |
			    VIRTGPU_BLOB_FLAG_USE_SHAREABLE,
			.size = 4096,
			.blob_id = 0,
			.cmd_size = 0,
			.cmd = (uintptr_t)NULL,
		};

		int ret =
		    ioctl(drm_fd, DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB,
			  &blob);
		igt_assert_f(!ret, "Blob creation ioctl failed: %m\n");
		igt_assert_neq(blob.res_handle, 0);
	}

	igt_describe
	    ("Queries different GPU capsets and prints the response payload.");
	igt_subtest("drm-virtgpu-get-caps") {
		setup();
		/* Test multiple capsets */
		igt_assert_eq(test_capset(drm_fd, 1), 0); /* VirGL */
		igt_assert_eq(test_capset(drm_fd, 2), 0); /* VirGL2 */
		if (test_gfxstream)
			igt_assert_eq(test_capset(drm_fd, 3), 0); /* GFXSTREAM_VULKAN */
		else
			/* Expect failure if not gfxstream backend */
			igt_assert(test_capset(drm_fd, 3) != 0);

		igt_assert(test_capset(drm_fd, 9999) != 0); /* Invalid (expect failure) */
	}

	igt_describe
	    ("Initializes a GPU context with parameters like capset ID and debug name.");
	igt_subtest("drm-virtgpu-context-init") {
		struct drm_virtgpu_context_set_param ctx_params[2];
		struct drm_virtgpu_context_init ctx_init;
		int ret;

		memset(ctx_params, 0, sizeof(ctx_params));

		ctx_params[0].param = VIRTGPU_CONTEXT_PARAM_CAPSET_ID;
		ctx_params[0].value = 1;

		ctx_params[1].param = VIRTGPU_CONTEXT_PARAM_DEBUG_NAME;
		ctx_params[1].value = (uintptr_t)"IGT-Test-Context";

		memset(&ctx_init, 0, sizeof(ctx_init));
		ctx_init.num_params = 2;
		ctx_init.ctx_set_params = (uintptr_t)&ctx_params[0];

		ret = ioctl(drm_fd, DRM_IOCTL_VIRTGPU_CONTEXT_INIT, &ctx_init);
		if (ret == -1 && errno == EEXIST) {
			igt_info
			    ("CONTEXT_INIT with params failed as expected (already initialized)\n");
		} else {
			igt_assert_f(!ret, "CONTEXT_INIT with params ioctl failed: %m\n");
			igt_info("CONTEXT_INIT with parameters succeeded\n");
		}
	}

	igt_describe
	    ("Verifies which VirtIO-GPU features are supported by querying driver parameters.");
	igt_subtest("drm-virtgpu-getparam") {
		static const struct {
			const char *name;
			u64 id;
		} params[] = {
			{"3D_FEATURES", VIRTGPU_PARAM_3D_FEATURES},
			{"CAPSET_QUERY_FIX", VIRTGPU_PARAM_CAPSET_QUERY_FIX},
			{"RESOURCE_BLOB", VIRTGPU_PARAM_RESOURCE_BLOB},
			{"HOST_VISIBLE", VIRTGPU_PARAM_HOST_VISIBLE},
			{"CROSS_DEVICE", VIRTGPU_PARAM_CROSS_DEVICE},
			{"CONTEXT_INIT", VIRTGPU_PARAM_CONTEXT_INIT},
			{"SUPPORTED_CAPSET_IDs", VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs},
			{"EXPLICIT_DEBUG_NAME", VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME},
		};
		bool found_supported = false;
		u64 actual_value = 0;

		for (size_t i = 0; i < ARRAY_SIZE(params); i++) {
			struct drm_virtgpu_getparam gp = {
				.param = params[i].id,
				.value = (uintptr_t)&actual_value,
			};

			int ret =
			    ioctl(drm_fd, DRM_IOCTL_VIRTGPU_GETPARAM, &gp);
			if (ret == 0) {
				found_supported = true;
				igt_info("GETPARAM %s (ID=%lu): value = %llu\n",
					 params[i].name, params[i].id,
					 gp.value);
			} else {
				igt_info("GETPARAM %s (ID=%lu): failed - %m\n",
					 params[i].name, params[i].id);
			}
		}

		igt_assert_f(found_supported,
			     "No GETPARAM query returned a value.");
	}

	igt_fixture() {
		free(caps_buf);
		close(drm_fd);
	}
}
