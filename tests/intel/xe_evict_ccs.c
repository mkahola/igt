// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

/**
 * TEST: Check flat-ccs eviction
 * Category: Core
 * Mega feature: Compression
 * Sub-category: Flat-ccs tests
 * Functionality: ccs-evict
 * GPU requirements: GPU needs to have dedicated VRAM
 */

#include "igt.h"
#include "igt_list.h"
#include "intel_blt.h"
#include "intel_mocs.h"
#include "lib/igt_syncobj.h"
#include "lib/intel_reg.h"
#include "xe_drm.h"

#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include <math.h>
#include <string.h>

#define OVERCOMMIT_VRAM_PERCENT 110
#define MIN_OBJ_KB 64
#define MAX_OBJ_KB (256 * 1024)
#define DUMP_FILENAME "/tmp/object.data"
#define DUMP_EXPFILENAME "/tmp/object.expected"

static struct param {
	bool print_bb;
	bool disable_compression;
	bool dump_corrupted_surface;
	int num_objs;
	int vram_percent;
	int min_size_kb;
	int max_size_kb;
	bool user_set_max_size;
	bool verify;
} params = {
	.num_objs = 0,
	.vram_percent = OVERCOMMIT_VRAM_PERCENT,
	.min_size_kb = MIN_OBJ_KB,
	.max_size_kb = MAX_OBJ_KB,
	.user_set_max_size = false,
};

struct object {
	uint64_t size;
	uint32_t start_value;
	struct blt_copy_object *blt_obj;
	struct igt_list_head link;
};

#define TEST_PARALLEL		(1 << 0)
#define TEST_INSTANTFREE	(1 << 1)
#define TEST_REOPEN		(1 << 2)
#define TEST_SIMPLE		(1 << 3)

#define MAX_NPROC 8
struct config {
	uint32_t flags;
	int nproc;
	int free_mb, total_mb;
	int test_mb, mb_per_proc;
	const struct param *param;
};

static void copy_obj(struct blt_copy_data *blt,
		     struct blt_copy_object *src_obj,
		     struct blt_copy_object *dst_obj,
		     intel_ctx_t *ctx,
		     uint64_t ahnd)
{
	struct blt_block_copy_data_ext ext = {};
	int fd = blt->fd;
	uint64_t bb_size = xe_bb_size(fd, SZ_4K);
	uint32_t bb;
	uint32_t w, h;

	w = src_obj->x2;
	h = src_obj->y2;

	bb = xe_bo_create(fd, 0, bb_size, vram_memory(fd, 0),
			  DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM);

	blt->color_depth = CD_32bit;
	blt->print_bb = params.print_bb;
	blt_set_copy_object(&blt->src, src_obj);
	blt_set_copy_object(&blt->dst, dst_obj);
	blt_set_object_ext(&ext.src, 0, w, h, SURFACE_TYPE_2D);
	blt_set_object_ext(&ext.dst, 0, w, h, SURFACE_TYPE_2D);
	blt_set_batch(&blt->bb, bb, bb_size, vram_if_possible(fd, 0));
	blt_block_copy(fd, ctx, NULL, ahnd, blt, &ext);
	intel_ctx_xe_sync(ctx, true);

	gem_close(fd, bb);
	put_offset(ahnd, bb);
	put_offset(ahnd, blt->src.handle);
	put_offset(ahnd, blt->dst.handle);
	intel_allocator_bind(ahnd, 0, 0);
}

static uint32_t rand_and_update(uint32_t *left, uint32_t min, uint32_t max)
{
	int left_bit, min_bit, max_bit, rand_id, rand_kb;

	left_bit = igt_fls(*left) - 1;
	min_bit = igt_fls(min) - 1;
	max_bit = max_t(int, min_t(int, igt_fls(max) - 1, left_bit), igt_fls(max));
	rand_id = rand() % (max_bit - min_bit);
	rand_kb = 1 << (rand_id + min_bit);

	if (*left >= rand_kb)
		*left -= rand_kb;
	else
		*left = 0;

	return rand_kb;
}

static struct object *create_obj(struct blt_copy_data *blt,
				 intel_ctx_t *ctx, uint64_t ahnd,
				 uint64_t size, int start_value,
				 bool disable_compression)
{
	int fd = blt->fd;
	struct object *obj;
	uint32_t w, h;
	uint8_t uc_mocs = intel_get_uc_mocs_index(fd);
	int i;
	struct blt_copy_object *src;

	obj = calloc(1, sizeof(*obj));
	igt_assert(obj);
	obj->size = size;
	obj->start_value = start_value;

	w = max_t(int, 1024, roundup_power_of_two(sqrt(size/4)));
	h = size / w / 4; /* /4 - 32bpp */

	igt_debug("[%8d] Obj size: %"PRId64"KiB (%"PRId64"MiB) <w: %d, h: %d>\n",
		  getpid(), size / SZ_1K, size / SZ_1M, w, h);

	src = blt_create_object(blt,
				system_memory(fd),
				w, h, 32, uc_mocs,
				T_LINEAR, COMPRESSION_DISABLED,
				COMPRESSION_TYPE_3D, true);

	obj->blt_obj = blt_create_object(blt, vram_memory(fd, 0),
					 w, h, 32, uc_mocs,
					 T_LINEAR,
					 disable_compression ? COMPRESSION_DISABLED :
							       COMPRESSION_ENABLED,
					 COMPRESSION_TYPE_3D, true);

	for (i = 0; i < size / sizeof(uint32_t); i++)
		src->ptr[i] = start_value++;

	copy_obj(blt, src, obj->blt_obj, ctx, ahnd);

	blt_destroy_object_and_alloc_free(fd, ahnd, src);
	intel_allocator_bind(ahnd, 0, 0);

	return obj;
}

static void dump_obj(const struct blt_copy_object *obj, int start_value)
{
	FILE *out;

	if (!params.dump_corrupted_surface)
		return;

	out = fopen(DUMP_FILENAME, "wb");
	fwrite(obj->ptr, obj->size, 1, out);
	fclose(out);

	out = fopen(DUMP_EXPFILENAME, "wb");
	for (int i = 0; i < obj->size / 4; i++) {
		int v = start_value + i;

		fwrite(&v, sizeof(int), 1, out);
	}
	fclose(out);
}

static void check_obj(const char *check_mode,
		      const struct blt_copy_object *obj, uint64_t size,
		      int start_value, int num_obj)
{
	int i, idx;

	if (obj->ptr[0] != start_value ||
	    (obj->ptr[size/4 - 1] != start_value + size/4 - 1)) {
		igt_info("[%s] Failed object w: %d, h: %d, size: %"PRId64"KiB (%"PRId64"MiB)\n",
			 check_mode, obj->x2, obj->y2, obj->size / SZ_1K, obj->size / SZ_1M);
		dump_obj(obj, start_value);
	}

	igt_assert_eq(obj->ptr[0], start_value);
	igt_assert_eq(obj->ptr[size/4 - 1], start_value + size/4 - 1);

	/* Couple of checks of random indices */
	for (i = 0; i < 128; i++) {
		idx = rand() % (size/4);

		if (obj->ptr[idx] != start_value + idx) {
			igt_info("[%s] Failed object w: %d, h: %d, size: %"PRId64"KiB (%"PRId64"MiB)\n",
				 check_mode, obj->x2, obj->y2,
				 obj->size / SZ_1K, obj->size / SZ_1M);
			dump_obj(obj, start_value);
		}

		igt_assert_f(obj->ptr[idx] == start_value + idx,
			     "[%s] Object number %d doesn't contain valid data",
			     check_mode, num_obj);
	}
}

static void evict_single(int fd, int child, const struct config *config)
{
	struct blt_copy_data blt = {};
	struct blt_copy_object *orig_obj;
	uint32_t kb_left = config->mb_per_proc * SZ_1K;
	uint32_t min_alloc_kb = config->param->min_size_kb;
	uint32_t max_alloc_kb = config->param->max_size_kb;
	uint32_t vm = xe_vm_create(fd, 0, 0);
	uint64_t ahnd = intel_allocator_open(fd, vm, INTEL_ALLOCATOR_RELOC);
	uint8_t uc_mocs = intel_get_uc_mocs_index(fd);
	struct object *obj, *tmp;
	struct igt_list_head list;
	struct drm_xe_engine_class_instance inst = {
		.engine_class = DRM_XE_ENGINE_CLASS_COPY,
	};
	intel_ctx_t *ctx;
	uint32_t exec_queue, big_obj;
	int num_obj = 0;

	srandom(time(NULL) + getpid());
	IGT_INIT_LIST_HEAD(&list);
	igt_debug("[%2d] child : to allocate: %uMiB\n", child, kb_left/SZ_1K);

	blt_copy_init(fd, &blt);

	exec_queue = xe_exec_queue_create(fd, vm, &inst, 0);
	ctx = intel_ctx_xe(fd, vm, exec_queue, 0, 0, 0);

	while (kb_left) {
		struct blt_copy_object *verify_obj;
		uint64_t obj_size = rand_and_update(&kb_left, min_alloc_kb, max_alloc_kb) * SZ_1K;
		int start_value = rand();

		if (config->flags & TEST_SIMPLE)
			obj_size = max_alloc_kb * SZ_1K;

		obj = create_obj(&blt, ctx, ahnd, obj_size, start_value,
				 config->param->disable_compression);
		igt_list_add(&obj->link, &list);

		if (config->param->verify) {
			verify_obj = blt_create_object(&blt, system_memory(fd),
						       obj->blt_obj->x2,
						       obj->blt_obj->y2,
						       32, uc_mocs,
						       T_LINEAR, COMPRESSION_DISABLED,
						       0, true);
			copy_obj(&blt, obj->blt_obj, verify_obj, ctx, ahnd);
			check_obj("Verify", verify_obj, obj->blt_obj->size,
				  obj->start_value, num_obj++);
			blt_destroy_object_and_alloc_free(fd, ahnd, verify_obj);
			intel_allocator_bind(ahnd, 0, 0);
		}

		if (config->flags & TEST_SIMPLE) {
			big_obj = xe_bo_create(fd, vm, kb_left * SZ_1K,
					       vram_memory(fd, 0), 0);
			break;
		}

		if (config->param->num_objs && ++num_obj == config->param->num_objs)
			break;
	}

	if (config->param->verify)
		igt_info("[%8d] Verify ok\n", getpid());

	num_obj = 0;
	igt_list_for_each_entry_safe(obj, tmp, &list, link) {
		orig_obj = blt_create_object(&blt, system_memory(fd),
					     obj->blt_obj->x2,
					     obj->blt_obj->y2,
					     32, uc_mocs,
					     T_LINEAR, COMPRESSION_DISABLED,
					     0, true);
		copy_obj(&blt, obj->blt_obj, orig_obj, ctx, ahnd);
		check_obj("Check", orig_obj, obj->blt_obj->size, obj->start_value, num_obj++);
		blt_destroy_object_and_alloc_free(fd, ahnd, orig_obj);

		if (config->flags & TEST_INSTANTFREE) {
			igt_list_del(&obj->link);
			blt_destroy_object_and_alloc_free(fd, ahnd, obj->blt_obj);
			free(obj);
		}
		intel_allocator_bind(ahnd, 0, 0);
	}

	if (!(config->flags & TEST_INSTANTFREE))
		igt_list_for_each_entry_safe(obj, tmp, &list, link) {
			igt_list_del(&obj->link);
			blt_destroy_object_and_alloc_free(fd, ahnd, obj->blt_obj);
			free(obj);
		}

	if (config->flags & TEST_SIMPLE)
		gem_close(fd, big_obj);
}

static void set_config(int fd, uint32_t flags, const struct param *param,
		       struct config *config)
{
	int nproc = 1;

	config->param = param;
	config->flags = flags;
	config->free_mb = xe_visible_vram_size(fd, 0) / SZ_1M;
	config->total_mb = xe_available_vram_size(fd, 0) / SZ_1M;
	config->test_mb = min_t(int, config->free_mb * config->param->vram_percent / 100,
				config->total_mb * config->param->vram_percent / 100);

	igt_debug("VRAM memory size: %dMB/%dMB (use %dMB), overcommit perc: %d\n",
		  config->free_mb, config->total_mb,
		  config->test_mb, config->param->vram_percent);

	if (flags & TEST_PARALLEL)
		nproc = min_t(int, sysconf(_SC_NPROCESSORS_ONLN), MAX_NPROC);
	config->nproc = nproc;
	config->mb_per_proc = config->test_mb / nproc;

	igt_debug("nproc: %d, mem per proc: %dMB\n", nproc, config->mb_per_proc);
}

static void adjust_params_for_vram_size(uint64_t vram_sz)
{
	int recommended_max_size_kb;
	int max_object_mb = vram_sz / MIN_OBJ_KB;

	/* max_object_mb clamped between 2MB and 256MB */
	max_object_mb = max_t(int, 2, min_t(int, max_object_mb, 256));
	recommended_max_size_kb = max_object_mb * 1024;

	igt_info("Detected VRAM (%"PRIu64"MB): Using %d%% for test, max object size: %dMB ",
		 vram_sz, params.vram_percent, max_object_mb);

	if (params.user_set_max_size) {
		if (params.max_size_kb > recommended_max_size_kb) {
			igt_warn("User specified size (%dMB) may not "
				 "be optimal for %"PRIu64"MB VRAM "
				 "(recommended size: %dMB)", params.max_size_kb / 1024, vram_sz,
				 recommended_max_size_kb / 1024);
		} else {
			igt_info("Using user max object size: %dMB (recommended: %dMB)",
				 params.max_size_kb / 1024, recommended_max_size_kb / 1024);
		}
	} else {
		params.max_size_kb = recommended_max_size_kb;
		igt_info("Adjusted settings: %d%% VRAM, %dMB max object\n",
			 params.vram_percent, params.max_size_kb / 1024);
	}
}

static void evict_ccs(int fd, uint32_t flags, const struct param *param)
{
	struct config config;
	char numstr[32];

	igt_info("Test mode <parallel: %d, instant free: %d, reopen: %d, simple: %d>\n",
		 !!(flags & TEST_PARALLEL),
		 !!(flags & TEST_INSTANTFREE),
		 !!(flags & TEST_REOPEN),
		 !!(flags & TEST_SIMPLE));
	if (param->num_objs)
		snprintf(numstr, sizeof(numstr), "%d", param->num_objs);
	else
		strncpy(numstr, "limited to vram", sizeof(numstr));
	igt_info("Params: compression: %s, num objects: %s, vram percent: %d, kb <min: %d, max: %d>\n",
		 param->disable_compression ? "disabled" : "enabled",
		 numstr, param->vram_percent,
		 param->min_size_kb, param->max_size_kb);

	set_config(fd, flags, param, &config);

	if (flags & TEST_PARALLEL) {
		igt_fork(n, config.nproc) {
			if (flags & TEST_REOPEN) {
				fd = drm_reopen_driver(fd);
				intel_allocator_init();
			}
			evict_single(fd, n, &config);
		}
		igt_waitchildren();
	} else {
		if (flags & TEST_REOPEN)
			fd = drm_reopen_driver(fd);
		evict_single(fd, 0, &config);
	}
}

/**
 *
 * SUBTEST: evict-overcommit-simple
 * Description: Eviction test - exercises FlatCCS if possible.
 * Feature: flatccs
 * Test category: stress test
 */
/**
 *
 * SUBTEST: evict-overcommit-%s-%s-%s
 * Description: Eviction test - exercises FlatCCS if possible.
 * Feature: flatccs
 * Test category: stress test
 *
 * arg[1]:
 *
 * @standalone:			single process
 * @parallel:			multiple processes
 *
 * arg[2]:
 *
 * @nofree:			keep objects till the end of the test
 * @instantfree:		free object after it was verified and it won't
 *				be used anymore
 *
 * arg[3]:
 *
 * @samefd:			operate on same opened drm fd
 * @reopen:			use separately opened drm fds
 *
 */
static int opt_handler(int opt, int opt_index, void *data)
{
	switch (opt) {
	case 'b':
		params.print_bb = true;
		igt_debug("Print bb: %d\n", params.print_bb);
		break;
	case 'd':
		params.disable_compression = true;
		igt_debug("Print bb: %d\n", params.disable_compression);
		break;
	case 'D':
		params.dump_corrupted_surface = true;
		igt_debug("Print bb: %d\n", params.dump_corrupted_surface);
		break;
	case 'n':
		params.num_objs = atoi(optarg);
		igt_debug("Number objects: %d\n", params.num_objs);
		break;
	case 'p':
		params.vram_percent = atoi(optarg);
		igt_debug("Percent vram: %d\n", params.vram_percent);
		break;
	case 's':
		params.min_size_kb = atoi(optarg);
		igt_debug("Min size kb: %d\n", params.min_size_kb);
		break;
	case 'S':
		params.max_size_kb = atoi(optarg);
		params.user_set_max_size = true;
		igt_debug("Max size kb: %d\n", params.max_size_kb);
		break;
	case 'V':
		params.verify = true;
		igt_debug("Verify: %d\n", params.verify);
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =
	"  -b\tPrint bb\n"
	"  -d\tDisable compression (don't use flatccs area)\n"
	"  -D\tDump surface which doesn't match\n"
	"  -e\tAdd temporary object which enforce eviction\n"
	"  -n\tNumber of objects to create (0 - 31)\n"
	"  -p\tPercent of VRAM to alloc\n"
	"  -s\tMinimum size of object in kb\n"
	"  -S\tMaximum size of object in kb\n"
	"  -V\tVerify object after compressing\n"
	;

igt_main_args("bdDn:p:s:S:V", NULL, help_str, opt_handler, NULL)
{
	const struct ccs {
		const char *name;
		uint32_t flags;
	} ccs[] = {
		{ "simple",
			TEST_SIMPLE },
		{ "standalone-nofree-samefd",
			0 },
		{ "standalone-nofree-reopen",
			TEST_REOPEN },
		{ "standalone-instantfree-samefd",
			TEST_INSTANTFREE },
		{ "standalone-instantfree-reopen",
			TEST_INSTANTFREE | TEST_REOPEN },
		{ "parallel-nofree-samefd",
			TEST_PARALLEL },
		{ "parallel-nofree-reopen",
			TEST_PARALLEL | TEST_REOPEN },
		{ "parallel-instantfree-samefd",
			TEST_PARALLEL | TEST_INSTANTFREE },
		{ "parallel-instantfree-reopen",
			TEST_PARALLEL | TEST_INSTANTFREE | TEST_REOPEN },
		{ },
	};
	uint64_t vram_size;
	uint64_t vram_mb;
	bool has_flatccs;
	int fd;

	igt_fixture() {
		fd = drm_open_driver(DRIVER_XE);
		igt_require(xe_has_vram(fd));
		vram_size = xe_visible_vram_size(fd, 0);
		igt_assert(vram_size);
		vram_mb = vram_size / (1024 * 1024);
		/* Adjust when resizable-BAR is turned off in BIOS */
		if (vram_mb <= 256)
			adjust_params_for_vram_size(vram_mb);

		has_flatccs = HAS_FLATCCS(intel_get_drm_devid(fd));
	}

	igt_fixture()
		intel_allocator_multiprocess_start();

	for (const struct ccs *s = ccs; s->name; s++) {
		igt_subtest_f("evict-overcommit-%s", s->name) {
			if (!params.disable_compression && !has_flatccs) {
				igt_info("Device has no flatccs, disabling compression\n");
				params.disable_compression = true;
			}
			evict_ccs(fd, s->flags, &params);
		}
	}

	igt_fixture() {
		intel_allocator_multiprocess_stop();
		drm_close_driver(fd);
	}
}
