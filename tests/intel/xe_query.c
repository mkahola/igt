// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

/**
 * TEST: Check device configuration query
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: uapi
 * Functionality: ioctl
 * Description: Acquire configuration data for xe device
 */

#include <string.h>

#include "igt.h"
#include "linux_scaffold.h"
#include "xe_drm.h"
#include "xe/xe_ioctl.h"
#include "xe/xe_query.h"
#include "intel_hwconfig_types.h"

void dump_hex(void *buffer, int len);
void dump_hex_debug(void *buffer, int len);
const char *get_hwconfig_name(int param);
const char *get_topo_name(int value);
void process_hwconfig(void *data, uint32_t len);

void dump_hex(void *buffer, int len)
{
	unsigned char *data = (unsigned char*)buffer;
	int k = 0;
	for (int i = 0; i < len; i++) {
		igt_info(" %02x", data[i]);
		if (++k > 15) {
			k = 0;
			igt_info("\n");
		}
	}
	if (k)
		igt_info("\n");
}

void dump_hex_debug(void *buffer, int len)
{
	if (igt_log_level == IGT_LOG_DEBUG)
		dump_hex(buffer, len);
}

/* Please reflect intel_hwconfig_types.h changes below
 * static_asserti_value + get_hwconfig_name
 *   Thanks :-) */
static_assert(INTEL_HWCONFIG_NUM_XECU + 1 == __INTEL_HWCONFIG_KEY_LIMIT, "");

#define CASE_STRINGIFY(A) case INTEL_HWCONFIG_##A: return #A;
const char* get_hwconfig_name(int param)
{
	switch(param) {
	CASE_STRINGIFY(MAX_SLICES_SUPPORTED);
	CASE_STRINGIFY(MAX_DUAL_SUBSLICES_SUPPORTED);
	CASE_STRINGIFY(MAX_NUM_EU_PER_DSS);
	CASE_STRINGIFY(NUM_PIXEL_PIPES);
	CASE_STRINGIFY(DEPRECATED_MAX_NUM_GEOMETRY_PIPES);
	CASE_STRINGIFY(DEPRECATED_L3_CACHE_SIZE_IN_KB);
	CASE_STRINGIFY(DEPRECATED_L3_BANK_COUNT);
	CASE_STRINGIFY(L3_CACHE_WAYS_SIZE_IN_BYTES);
	CASE_STRINGIFY(L3_CACHE_WAYS_PER_SECTOR);
	CASE_STRINGIFY(MAX_MEMORY_CHANNELS);
	CASE_STRINGIFY(MEMORY_TYPE);
	CASE_STRINGIFY(CACHE_TYPES);
	CASE_STRINGIFY(LOCAL_MEMORY_PAGE_SIZES_SUPPORTED);
	CASE_STRINGIFY(DEPRECATED_SLM_SIZE_IN_KB);
	CASE_STRINGIFY(NUM_THREADS_PER_EU);
	CASE_STRINGIFY(TOTAL_VS_THREADS);
	CASE_STRINGIFY(TOTAL_GS_THREADS);
	CASE_STRINGIFY(TOTAL_HS_THREADS);
	CASE_STRINGIFY(TOTAL_DS_THREADS);
	CASE_STRINGIFY(TOTAL_VS_THREADS_POCS);
	CASE_STRINGIFY(TOTAL_PS_THREADS);
	CASE_STRINGIFY(DEPRECATED_MAX_FILL_RATE);
	CASE_STRINGIFY(MAX_RCS);
	CASE_STRINGIFY(MAX_CCS);
	CASE_STRINGIFY(MAX_VCS);
	CASE_STRINGIFY(MAX_VECS);
	CASE_STRINGIFY(MAX_COPY_CS);
	CASE_STRINGIFY(DEPRECATED_URB_SIZE_IN_KB);
	CASE_STRINGIFY(MIN_VS_URB_ENTRIES);
	CASE_STRINGIFY(MAX_VS_URB_ENTRIES);
	CASE_STRINGIFY(MIN_PCS_URB_ENTRIES);
	CASE_STRINGIFY(MAX_PCS_URB_ENTRIES);
	CASE_STRINGIFY(MIN_HS_URB_ENTRIES);
	CASE_STRINGIFY(MAX_HS_URB_ENTRIES);
	CASE_STRINGIFY(MIN_GS_URB_ENTRIES);
	CASE_STRINGIFY(MAX_GS_URB_ENTRIES);
	CASE_STRINGIFY(MIN_DS_URB_ENTRIES);
	CASE_STRINGIFY(MAX_DS_URB_ENTRIES);
	CASE_STRINGIFY(PUSH_CONSTANT_URB_RESERVED_SIZE);
	CASE_STRINGIFY(POCS_PUSH_CONSTANT_URB_RESERVED_SIZE);
	CASE_STRINGIFY(URB_REGION_ALIGNMENT_SIZE_IN_BYTES);
	CASE_STRINGIFY(URB_ALLOCATION_SIZE_UNITS_IN_BYTES);
	CASE_STRINGIFY(MAX_URB_SIZE_CCS_IN_BYTES);
	CASE_STRINGIFY(VS_MIN_DEREF_BLOCK_SIZE_HANDLE_COUNT);
	CASE_STRINGIFY(DS_MIN_DEREF_BLOCK_SIZE_HANDLE_COUNT);
	CASE_STRINGIFY(NUM_RT_STACKS_PER_DSS);
	CASE_STRINGIFY(MAX_URB_STARTING_ADDRESS);
	CASE_STRINGIFY(MIN_CS_URB_ENTRIES);
	CASE_STRINGIFY(MAX_CS_URB_ENTRIES);
	CASE_STRINGIFY(L3_ALLOC_PER_BANK_URB);
	CASE_STRINGIFY(L3_ALLOC_PER_BANK_REST);
	CASE_STRINGIFY(L3_ALLOC_PER_BANK_DC);
	CASE_STRINGIFY(L3_ALLOC_PER_BANK_RO);
	CASE_STRINGIFY(L3_ALLOC_PER_BANK_Z);
	CASE_STRINGIFY(L3_ALLOC_PER_BANK_COLOR);
	CASE_STRINGIFY(L3_ALLOC_PER_BANK_UNIFIED_TILE_CACHE);
	CASE_STRINGIFY(L3_ALLOC_PER_BANK_COMMAND_BUFFER);
	CASE_STRINGIFY(L3_ALLOC_PER_BANK_RW);
	CASE_STRINGIFY(MAX_NUM_L3_CONFIGS);
	CASE_STRINGIFY(BINDLESS_SURFACE_OFFSET_BIT_COUNT);
	CASE_STRINGIFY(RESERVED_CCS_WAYS);
	CASE_STRINGIFY(CSR_SIZE_IN_MB);
	CASE_STRINGIFY(GEOMETRY_PIPES_PER_SLICE);
	CASE_STRINGIFY(L3_BANK_SIZE_IN_KB);
	CASE_STRINGIFY(SLM_SIZE_PER_DSS);
	CASE_STRINGIFY(MAX_PIXEL_FILL_RATE_PER_SLICE);
	CASE_STRINGIFY(MAX_PIXEL_FILL_RATE_PER_DSS);
	CASE_STRINGIFY(URB_SIZE_PER_SLICE_IN_KB);
	CASE_STRINGIFY(URB_SIZE_PER_L3_BANK_COUNT_IN_KB);
	CASE_STRINGIFY(MAX_SUBSLICE);
	CASE_STRINGIFY(MAX_EU_PER_SUBSLICE);
	CASE_STRINGIFY(RAMBO_L3_BANK_SIZE_IN_KB);
	CASE_STRINGIFY(SLM_SIZE_PER_SS_IN_KB);
	CASE_STRINGIFY(NUM_HBM_STACKS_PER_TILE);
	CASE_STRINGIFY(NUM_CHANNELS_PER_HBM_STACK);
	CASE_STRINGIFY(HBM_CHANNEL_WIDTH_IN_BYTES);
	CASE_STRINGIFY(MIN_TASK_URB_ENTRIES);
	CASE_STRINGIFY(MAX_TASK_URB_ENTRIES);
	CASE_STRINGIFY(MIN_MESH_URB_ENTRIES);
	CASE_STRINGIFY(MAX_MESH_URB_ENTRIES);
	CASE_STRINGIFY(MAX_GSC);
	CASE_STRINGIFY(SYNC_NUM_RT_STACKS_PER_DSS);
	CASE_STRINGIFY(NUM_XECU);
	}
	igt_assert_lt(param, __INTEL_HWCONFIG_KEY_LIMIT);
	igt_assert(!"Missing config table enum");
}
#undef CASE_STRINGIFY

void process_hwconfig(void *data, uint32_t len)
{

	uint32_t *d = (uint32_t*)data;
	uint32_t l = len / 4;
	uint32_t pos = 0;
	while (pos + 2 < l) {
		if (d[pos+1] == 1) {
			igt_info("%-37s (%3d) L:%d V: %d/0x%x\n",
				 get_hwconfig_name(d[pos]), d[pos], d[pos+1],
				 d[pos+2], d[pos+2]);
		} else {
			igt_info("%-37s (%3d) L:%d\n", get_hwconfig_name(d[pos]), d[pos], d[pos+1]);
			dump_hex(&d[pos+2], d[pos+1]);
		}
		pos += 2 + d[pos+1];
	}
}


const char *get_topo_name(int value)
{
	switch(value) {
	case DRM_XE_TOPO_DSS_GEOMETRY: return "DSS_GEOMETRY";
	case DRM_XE_TOPO_DSS_COMPUTE: return "DSS_COMPUTE";
	case DRM_XE_TOPO_EU_PER_DSS: return "EU_PER_DSS";
	case DRM_XE_TOPO_L3_BANK: return "L3_BANK";
	case DRM_XE_TOPO_SIMD16_EU_PER_DSS: return "SIMD16_EU_PER_DSS";
	}
	return "??";
}

/**
 * SUBTEST: query-engines
 * Description: Display engine classes available for xe device
 * Test category: functionality test
 *
 * SUBTEST: multigpu-query-engines
 * Mega feature: MultiGPU
 * Description: Display engine classes available for all Xe devices.
 * Test category: functionality test
 */
static void
test_query_engines(int fd)
{
	struct drm_xe_engine_class_instance *hwe;
	int i = 0;

	xe_for_each_engine(fd, hwe) {
		igt_assert(hwe);
		igt_info("engine %d: %s, engine instance: %d, gt: GT-%d, tile: TILE-%d\n",
			i++, xe_engine_class_string(hwe->engine_class),
			hwe->engine_instance, hwe->gt_id, xe_gt_get_tile_id(fd, hwe->gt_id));
	}

	igt_assert_lt(0, i);
}

/**
 * SUBTEST: query-mem-usage
 * Description: Display memory information like memory class, size
 *	and alignment.
 * Test category: functionality test
 *
 * SUBTEST: multigpu-query-mem-usage
 * Mega feature: MultiGPU
 * Description: Display memory information for all Xe devices.
 * Test category: functionality test
 */
static void
test_query_mem_regions(int fd)
{
	struct drm_xe_query_mem_regions *mem_regions;
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_MEM_REGIONS,
		.size = 0,
		.data = 0,
	};
	int i;

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);
	igt_assert_neq(query.size, 0);

	mem_regions = malloc(query.size);
	igt_assert(mem_regions);

	query.data = to_user_pointer(mem_regions);
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	for (i = 0; i < mem_regions->num_mem_regions; i++) {
		igt_info("mem region %d: %s\t%#llx / %#llx\n", i,
			mem_regions->mem_regions[i].mem_class ==
			DRM_XE_MEM_REGION_CLASS_SYSMEM ? "SYSMEM"
			:mem_regions->mem_regions[i].mem_class ==
			DRM_XE_MEM_REGION_CLASS_VRAM ? "VRAM" : "?",
			mem_regions->mem_regions[i].used,
			mem_regions->mem_regions[i].total_size
		);
		igt_info("min_page_size=0x%x\n",
		       mem_regions->mem_regions[i].min_page_size);

		igt_info("visible size=%lluMiB\n",
			 mem_regions->mem_regions[i].cpu_visible_size >> 20);
		igt_info("visible used=%lluMiB\n",
			 mem_regions->mem_regions[i].cpu_visible_used >> 20);

		igt_assert_lte_u64(mem_regions->mem_regions[i].cpu_visible_size,
				   mem_regions->mem_regions[i].total_size);
		igt_assert_lte_u64(mem_regions->mem_regions[i].cpu_visible_used,
				   mem_regions->mem_regions[i].cpu_visible_size);
		igt_assert_lte_u64(mem_regions->mem_regions[i].cpu_visible_used,
				   mem_regions->mem_regions[i].used);
		igt_assert_lte_u64(mem_regions->mem_regions[i].used,
				   mem_regions->mem_regions[i].total_size);
		igt_assert_lte_u64(mem_regions->mem_regions[i].used -
				   mem_regions->mem_regions[i].cpu_visible_used,
				   mem_regions->mem_regions[i].total_size);
	}
	dump_hex_debug(mem_regions, query.size);
	free(mem_regions);
}

/**
 * SUBTEST: query-gt-list
 * Description: Display information about available GT components for xe device.
 * Test category: functionality test
 *
 * SUBTEST: multigpu-query-gt-list
 * Mega feature: MultiGPU
 * Description: Display information about GT components for all Xe devices.
 * Test category: functionality test
 */
static void
test_query_gt_list(int fd)
{
	uint16_t dev_id = intel_get_drm_devid(fd);
	struct drm_xe_query_gt_list *gt_list;
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_GT_LIST,
		.size = 0,
		.data = 0,
	};
	int i;

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);
	igt_assert_neq(query.size, 0);

	gt_list = malloc(query.size);
	igt_assert(gt_list);

	query.data = to_user_pointer(gt_list);
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	for (i = 0; i < gt_list->num_gt; i++) {
		int verx100 = 100 * gt_list->gt_list[i].ip_ver_major +
			gt_list->gt_list[i].ip_ver_minor;

		igt_info("type: %d\n", gt_list->gt_list[i].type);
		igt_info("gt_id: %d\n", gt_list->gt_list[i].gt_id);
		igt_info("IP version: %d.%02d, stepping %d\n",
			 gt_list->gt_list[i].ip_ver_major,
			 gt_list->gt_list[i].ip_ver_minor,
			 gt_list->gt_list[i].ip_ver_rev);
		igt_info("reference_clock: %u\n", gt_list->gt_list[i].reference_clock);
		igt_info("near_mem_regions: 0x%016llx\n",
		       gt_list->gt_list[i].near_mem_regions);
		igt_info("far_mem_regions: 0x%016llx\n",
		       gt_list->gt_list[i].far_mem_regions);

		/* Sanity check IP version. */
		if (verx100) {
			/*
			 * First GMD_ID platforms had graphics 12.70 and media
			 * 13.00 so we should never see non-zero values lower
			 * than those.
			 */
			if (gt_list->gt_list[i].type == DRM_XE_QUERY_GT_TYPE_MEDIA)
				igt_assert_lte(1300, verx100);
			else
				igt_assert_lte(1270, verx100);

			/*
			 * Aside from MTL/ARL and media on BMG, all version
			 * numbers should be 20.00 or higher.
			 */
			if (IS_METEORLAKE(dev_id))
				continue;
			if (gt_list->gt_list[i].type == DRM_XE_QUERY_GT_TYPE_MEDIA &&
			    IS_BATTLEMAGE(dev_id))
				continue;

			igt_assert_lte(20, gt_list->gt_list[i].ip_ver_major);
		}
	}
}

/**
 * SUBTEST: query-topology
 * Description: Display topology information of GT.
 * Test category: functionality test
 *
 * SUBTEST: multigpu-query-topology
 * Mega feature: MultiGPU
 * Description: Display topology information of GT for all Xe devices.
 * Test category: functionality test
 */
static void
test_query_gt_topology(int fd)
{
	uint16_t dev_id = intel_get_drm_devid(fd);
	struct drm_xe_query_topology_mask *topology, *topo;
	uint32_t topo_types = 0, size;

	topology = xe_query_device(fd, DRM_XE_DEVICE_QUERY_GT_TOPOLOGY, &size);

	igt_info("size: %d\n", size);

	dump_hex_debug(topology, size);

	xe_for_each_topology_mask(topology, size, topo) {
		igt_info(" gt_id: %2d type: %-12s (%d) n:%d [%zd] ", topo->gt_id,
			 get_topo_name(topo->type), topo->type, topo->num_bytes,
			 sizeof(struct drm_xe_query_topology_mask) + topo->num_bytes);

		for (int j=0; j< topo->num_bytes; j++)
			igt_info(" %02x", topo->mask[j]);

		topo_types = 1 << topo->type;
		igt_info("\n");
	}

	/* sanity check EU type */
	if (IS_PONTEVECCHIO(dev_id) || intel_gen(dev_id) >= 20) {
		igt_assert(topo_types & (1 << DRM_XE_TOPO_SIMD16_EU_PER_DSS));
		igt_assert_eq(topo_types & (1 << DRM_XE_TOPO_EU_PER_DSS), 0);
	} else {
		igt_assert(topo_types & (1 << DRM_XE_TOPO_EU_PER_DSS));
		igt_assert_eq(topo_types & (1 << DRM_XE_TOPO_SIMD16_EU_PER_DSS), 0);
	}

	free(topology);
}

/**
 * SUBTEST: query-topology-l3-bank-mask
 * Description: Check the value of the l3 bank mask
 * Test category: functionality test
 *
 * SUBTEST: multigpu-query-topology-l3-bank-mask
 * Mega feature: MultiGPU
 * Description: Check the value of the l3 bank mask for all Xe devices.
 * Test category: functionality test
 */
static void
test_query_gt_topology_l3_bank_mask(int fd)
{
	uint16_t dev_id = intel_get_drm_devid(fd);
	struct drm_xe_query_topology_mask *topology, *topo;
	uint32_t size;

	topology = xe_query_device(fd, DRM_XE_DEVICE_QUERY_GT_TOPOLOGY, &size);

	igt_info("size: %d\n", size);

	xe_for_each_topology_mask(topology, size, topo) {
		if (topo->type == DRM_XE_TOPO_L3_BANK) {
			int count = 0;

			igt_info(" gt_id: %2d type: %-12s (%d) n:%d [%zd] ", topo->gt_id,
				 get_topo_name(topo->type), topo->type, topo->num_bytes,
				 sizeof(struct drm_xe_query_topology_mask) + topo->num_bytes);
			for (int j = 0; j < topo->num_bytes; j++)
				igt_info(" %02x", topo->mask[j]);

			for (int j = 0; j < topo->num_bytes; j++) {
				for (int k = 0; k < 8; k++)
					count += (topo->mask[j] & (1 << k)) ? 1 : 0;
			}

			igt_info(" count: %d\n", count);
			if (intel_get_device_info(dev_id)->graphics_ver < 20) {
				igt_assert_lt(0, count);
			}

			if (IS_METEORLAKE(dev_id))
				igt_assert_eq((count % 2), 0);
			else if (IS_PONTEVECCHIO(dev_id))
				igt_assert_eq((count % 4), 0);
			else if (IS_DG2(dev_id))
				igt_assert_eq((count % 8), 0);
		}
	}

	free(topology);
}


/**
 * SUBTEST: query-config
 * Description: Display xe device id, revision and configuration.
 * Test category: functionality test
 *
 * SUBTEST: multigpu-query-config
 * Mega feature: MultiGPU
 * Description: Display config information for all Xe devices.
 * Test category: functionality test
 */
static void
test_query_config(int fd)
{
	struct drm_xe_query_config *config;
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_CONFIG,
		.size = 0,
		.data = 0,
	};

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);
	igt_assert_neq(query.size, 0);

	config = malloc(query.size);
	igt_assert(config);

	query.data = to_user_pointer(config);
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	igt_assert(config->num_params > 0);

	igt_info("DRM_XE_QUERY_CONFIG_REV_AND_DEVICE_ID\t%#llx\n",
		config->info[DRM_XE_QUERY_CONFIG_REV_AND_DEVICE_ID]);
	igt_info("  REV_ID\t\t\t\t%#llx\n",
		config->info[DRM_XE_QUERY_CONFIG_REV_AND_DEVICE_ID] >> 16);
	igt_info("  DEVICE_ID\t\t\t\t%#llx\n",
		config->info[DRM_XE_QUERY_CONFIG_REV_AND_DEVICE_ID] & 0xffff);
	igt_info("DRM_XE_QUERY_CONFIG_FLAGS\t\t\t%#llx\n",
		config->info[DRM_XE_QUERY_CONFIG_FLAGS]);
	igt_info("  DRM_XE_QUERY_CONFIG_FLAG_HAS_VRAM\t%s\n",
		config->info[DRM_XE_QUERY_CONFIG_FLAGS] &
		DRM_XE_QUERY_CONFIG_FLAG_HAS_VRAM ? "ON":"OFF");
	igt_info("  DRM_XE_QUERY_CONFIG_FLAG_HAS_LOW_LATENCY\t%s\n",
		 config->info[DRM_XE_QUERY_CONFIG_FLAGS] &
		 DRM_XE_QUERY_CONFIG_FLAG_HAS_LOW_LATENCY ? "ON":"OFF");
	igt_info("DRM_XE_QUERY_CONFIG_MIN_ALIGNMENT\t\t%#llx\n",
		config->info[DRM_XE_QUERY_CONFIG_MIN_ALIGNMENT]);
	igt_info("DRM_XE_QUERY_CONFIG_VA_BITS\t\t\t%llu\n",
		config->info[DRM_XE_QUERY_CONFIG_VA_BITS]);
	igt_info("DRM_XE_QUERY_CONFIG_MAX_EXEC_QUEUE_PRIORITY\t%llu\n",
		config->info[DRM_XE_QUERY_CONFIG_MAX_EXEC_QUEUE_PRIORITY]);
	dump_hex_debug(config, query.size);

	free(config);
}

/**
 * SUBTEST: query-hwconfig
 * Description: Display hardware configuration of xe device.
 * Test category: functionality test
 *
 * SUBTEST: multigpu-query-hwconfig
 * Mega feature: MultiGPU
 * Description: Display hardware configuration for all Xe devices.
 * Test category: functionality test
 */
static void
test_query_hwconfig(int fd)
{
	void *hwconfig;
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_HWCONFIG,
		.size = 0,
		.data = 0,
	};

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	igt_info("HWCONFIG_SIZE\t%u\n", query.size);
	if (!query.size)
		return;

	hwconfig = malloc(query.size);
	igt_assert(hwconfig);

	query.data = to_user_pointer(hwconfig);
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	dump_hex_debug(hwconfig, query.size);
	process_hwconfig(hwconfig, query.size);

	free(hwconfig);
}

/**
 * SUBTEST: query-invalid-query
 * Description: Check query with invalid arguments returns expected error code.
 * Test category: negative test
 *
 * SUBTEST: multigpu-query-invalid-query
 * Mega feature: MultiGPU
 * Description: Check query with invalid arguments for all Xe devices.
 * Test category: negative test
 */
static void
test_query_invalid_query(int fd)
{
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = UINT32_MAX,
		.size = 0,
		.data = 0,
	};

	do_ioctl_err(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query, EINVAL);
}

/**
 * SUBTEST: query-invalid-size
 * Description: Check query with invalid size returns expected error code.
 * Test category: negative test
 *
 * SUBTEST: multigpu-query-invalid-size
 * Mega feature: MultiGPU
 * Description: Check query with invalid size for all Xe devices.
 * Test category: negative test
 */
static void
test_query_invalid_size(int fd)
{
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_CONFIG,
		.size = UINT32_MAX,
		.data = 0,
	};

	do_ioctl_err(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query, EINVAL);
}

/**
 * SUBTEST: query-invalid-extension
 * Description: Check query with invalid extension returns expected error code.
 * Test category: negative test
 *
 * SUBTEST: multigpu-query-invalid-extension
 * Mega feature: MultiGPU
 * Description: Check query with invalid extension for all Xe devices.
 * Test category: negative test
 */
static void
test_query_invalid_extension(int fd)
{
	struct drm_xe_device_query query = {
		.extensions = -1,
		.query = DRM_XE_DEVICE_QUERY_CONFIG,
		.size = 0,
		.data = 0,
	};

	do_ioctl_err(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query, EINVAL);
}

static bool
query_engine_cycles_supported(int fd)
{
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_ENGINE_CYCLES,
		.size = 0,
		.data = 0,
	};

	return igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query) == 0;
}

static void
query_engine_cycles(int fd, struct drm_xe_query_engine_cycles *resp)
{
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_ENGINE_CYCLES,
		.size = sizeof(*resp),
		.data = to_user_pointer(resp),
	};

	do_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query);
	igt_assert(query.size);
}

static uint32_t
__engine_reference_clock(int fd, int gt_id)
{
	struct xe_device *xe_dev = xe_device_get(fd);
	const struct drm_xe_gt *gt = drm_xe_get_gt(xe_dev, gt_id);

	igt_assert(gt);
	igt_assert(gt->reference_clock);

	return gt->reference_clock;
}

static void
__engine_cycles(int fd, struct drm_xe_engine_class_instance *hwe)
{
	struct drm_xe_query_engine_cycles ts1 = {};
	struct drm_xe_query_engine_cycles ts2 = {};
	uint64_t delta_cpu, delta_cs, delta_delta, calc_freq;
	unsigned int exec_queue;
	int i, usable = 0;
	igt_spin_t *spin;
	uint64_t ahnd;
	uint32_t vm, eng_ref_clock;
	struct {
		int32_t id;
		const char *name;
	} clock[] = {
		{ CLOCK_MONOTONIC, "CLOCK_MONOTONIC" },
		{ CLOCK_MONOTONIC_RAW, "CLOCK_MONOTONIC_RAW" },
		{ CLOCK_REALTIME, "CLOCK_REALTIME" },
		{ CLOCK_BOOTTIME, "CLOCK_BOOTTIME" },
		{ CLOCK_TAI, "CLOCK_TAI" },
	};

	igt_debug("engine[%u:%u]\n",
		  hwe->engine_class,
		  hwe->engine_instance);

	vm = xe_vm_create(fd, 0, 0);
	exec_queue = xe_exec_queue_create(fd, vm, hwe, 0);
	ahnd = intel_allocator_open(fd, 0, INTEL_ALLOCATOR_RELOC);
	spin = igt_spin_new(fd, .ahnd = ahnd, .engine = exec_queue, .vm = vm);

	/* Try a new clock every 10 iterations. */
#define NUM_SNAPSHOTS 10
	for (i = 0; i < NUM_SNAPSHOTS * ARRAY_SIZE(clock); i++) {
		int index = i / NUM_SNAPSHOTS;
		uint64_t width_mask;

		ts1.eci = *hwe;
		ts1.clockid = clock[index].id;

		ts2.eci = *hwe;
		ts2.clockid = clock[index].id;

		query_engine_cycles(fd, &ts1);
		query_engine_cycles(fd, &ts2);
		eng_ref_clock = __engine_reference_clock(fd, hwe->gt_id);

		igt_debug("[1] cpu_ts before %llu, reg read time %llu\n",
			  ts1.cpu_timestamp,
			  ts1.cpu_delta);
		igt_debug("[1] engine_ts %llu, width %u\n",
			  ts1.engine_cycles, ts1.width);

		igt_debug("[2] cpu_ts before %llu, reg read time %llu\n",
			  ts2.cpu_timestamp,
			  ts2.cpu_delta);
		igt_debug("[2] engine_ts %llu, width %u\n",
			  ts2.engine_cycles, ts2.width);

		delta_cpu = ts2.cpu_timestamp - ts1.cpu_timestamp;

		igt_assert_eq(ts1.width, ts2.width);
		width_mask = GENMASK_ULL(ts1.width - 1, 0);
		ts1.engine_cycles &= width_mask;
		ts2.engine_cycles &= width_mask;
		delta_cs = ((ts2.engine_cycles - ts1.engine_cycles) & width_mask) *
			NSEC_PER_SEC / eng_ref_clock;

		calc_freq = (ts2.engine_cycles - ts1.engine_cycles) * NSEC_PER_SEC / delta_cpu;

		igt_debug("freq %u Hz, calc_freq %"PRIu64" Hz, err %.3f%%\n", eng_ref_clock,
			  calc_freq, fabs((double)calc_freq - eng_ref_clock) * 100 / eng_ref_clock);
		igt_debug("delta_cpu[%"PRIu64"], delta_cs[%"PRIu64"]\n",
			  delta_cpu, delta_cs);

		delta_delta = delta_cpu > delta_cs ?
			       delta_cpu - delta_cs :
			       delta_cs - delta_cpu;
		igt_debug("delta_delta %"PRIu64"\n", delta_delta);

		if (delta_delta < 5000)
			usable++;

		/*
		 * User needs few good snapshots of the timestamps to
		 * synchronize cpu time with cs time. Check if we have enough
		 * usable values before moving to the next clockid.
		 */
		if (!((i + 1) % NUM_SNAPSHOTS)) {
			igt_debug("clock %s\n", clock[index].name);
			igt_debug("usable %d\n", usable);
			igt_assert_lt(2, usable);
			usable = 0;
		}
	}

	igt_spin_free(fd, spin);
	xe_exec_queue_destroy(fd, exec_queue);
	xe_vm_destroy(fd, vm);
	put_ahnd(ahnd);
}

/**
 * SUBTEST: query-cs-cycles
 * Description: Query CPU-GPU timestamp correlation
 *
 * SUBTEST: multigpu-query-cs-cycles
 * Description: Query CPU-GPU timestamp correlation for all Xe devices.
 * Category: Core
 * Mega feature: MultiGPU
 */
static void test_query_engine_cycles(int fd)
{
	struct drm_xe_engine_class_instance *hwe;

	igt_require(query_engine_cycles_supported(fd));

	xe_for_each_engine(fd, hwe) {
		igt_assert(hwe);
		__engine_cycles(fd, hwe);
	}
}

/**
 * SUBTEST: query-invalid-cs-cycles
 * Description: Check query with invalid arguments returns expected error code.
 *
 * SUBTEST: multigpu-query-invalid-cs-cycles
 * Description: Check query with invalid arguments for all Xe devices.
 * Category: Core
 * Mega feature: MultiGPU
 */
static void test_engine_cycles_invalid(int fd)
{
	struct drm_xe_engine_class_instance *hwe;
	struct drm_xe_query_engine_cycles ts = {};
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_ENGINE_CYCLES,
		.size = sizeof(ts),
		.data = to_user_pointer(&ts),
	};

	igt_require(query_engine_cycles_supported(fd));

	/* get one engine */
	xe_for_each_engine(fd, hwe)
		break;

	/* sanity check engine selection is valid */
	ts.eci = *hwe;
	query_engine_cycles(fd, &ts);

	/* bad instance */
	ts.eci = *hwe;
	ts.eci.engine_instance = 0xffff;
	do_ioctl_err(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query, EINVAL);
	ts.eci = *hwe;

	/* bad class */
	ts.eci.engine_class = 0xffff;
	do_ioctl_err(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query, EINVAL);
	ts.eci = *hwe;

	/* bad gt */
	ts.eci.gt_id = 0xffff;
	do_ioctl_err(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query, EINVAL);
	ts.eci = *hwe;

	/* bad clockid */
	ts.clockid = -1;
	do_ioctl_err(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query, EINVAL);
	ts.clockid = 0;

	/* sanity check */
	query_engine_cycles(fd, &ts);
}

static void
test_query_uc_fw_version(int fd, uint32_t uc_type)
{
	struct drm_xe_query_uc_fw_version *uc_fw_version;
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_UC_FW_VERSION,
		.size = 0,
		.data = 0,
	};
	int ret;

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	uc_fw_version = malloc(query.size);
	igt_assert(uc_fw_version);

	memset(uc_fw_version, 0, sizeof(*uc_fw_version));
	uc_fw_version->uc_type = uc_type;
	query.data = to_user_pointer(uc_fw_version);

	ret = igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query);

	switch (uc_type) {
	case XE_QUERY_UC_TYPE_GUC_SUBMISSION:
		igt_assert_eq(ret, 0);
		igt_info("XE_QUERY_UC_TYPE_GUC_SUBMISSION %u.%u.%u.%u\n",
			 uc_fw_version->branch_ver,
			 uc_fw_version->major_ver,
			 uc_fw_version->minor_ver,
			 uc_fw_version->patch_ver);
		igt_assert(uc_fw_version->major_ver > 0 ||
			   uc_fw_version->minor_ver > 0);
		break;
	case XE_QUERY_UC_TYPE_HUC:
		if (ret == 0) {
			igt_info("XE_QUERY_UC_TYPE_HUC %u.%u.%u.%u\n",
				 uc_fw_version->branch_ver,
				 uc_fw_version->major_ver,
				 uc_fw_version->minor_ver,
				 uc_fw_version->patch_ver);
			igt_assert(uc_fw_version->major_ver > 0);
		} else {
			igt_assert_eq(errno, ENODEV);
			/*
			 * No HuC was found, either because it is not running
			 * yet or there is no media IP.
			 */
			igt_info("XE_QUERY_UC_TYPE_HUC No HuC is running\n");
		}
		break;
	default:
		igt_assert(false);
	}

	free(uc_fw_version);
}

/**
 * SUBTEST: query-uc-fw-version-guc
 * Test category: functionality test
 * Description: Display the GuC firmware submission version
 *
 * SUBTEST: multigpu-query-uc-fw-version-guc
 * Description: Display GuC firmware submission version for all Xe devices.
 * Mega feature: MultiGPU
 * Test category: functionality test
 */
static void
test_query_uc_fw_version_guc(int fd)
{
	test_query_uc_fw_version(fd, XE_QUERY_UC_TYPE_GUC_SUBMISSION);
}

/**
 * SUBTEST: query-invalid-uc-fw-version-mbz
 * Test category: functionality test
 * Description: Check query with invalid arguments returns expected error code.
 *
 * SUBTEST: multigpu-query-invalid-uc-fw-version-mbz
 * Description: Check query with invalid arguments for all Xe devices.
 * Mega feature: MultiGPU
 * Test category: functionality test
 */
static void
test_query_uc_fw_version_invalid_mbz(int fd)
{
	struct drm_xe_query_uc_fw_version *uc_fw_version;
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_UC_FW_VERSION,
		.size = 0,
		.data = 0,
	};

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	uc_fw_version = malloc(query.size);
	igt_assert(uc_fw_version);

	memset(uc_fw_version, 0, sizeof(*uc_fw_version));
	uc_fw_version->uc_type = XE_QUERY_UC_TYPE_GUC_SUBMISSION;
	query.data = to_user_pointer(uc_fw_version);

	/* Make sure the baseline passes */
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	/* Make sure KMD rejects non-zero padding/reserved fields */
	uc_fw_version->pad = -1;
	do_ioctl_err(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query, EINVAL);
	uc_fw_version->pad = 0;

	uc_fw_version->pad2 = -1;
	do_ioctl_err(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query, EINVAL);
	uc_fw_version->pad2 = 0;

	uc_fw_version->reserved = -1;
	do_ioctl_err(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query, EINVAL);
	uc_fw_version->reserved = 0;

	free(uc_fw_version);
}

/**
 * SUBTEST: query-uc-fw-version-huc
 * Test category: functionality test
 * Description: Display the HuC firmware version
 *
 * SUBTEST: multigpu-query-uc-fw-version-huc
 * Description: Display HuC firmware version for all Xe devices.
 * Mega feature: MultiGPU
 * Test category: functionality test
 */
static void
test_query_uc_fw_version_huc(int fd)
{
	test_query_uc_fw_version(fd, XE_QUERY_UC_TYPE_HUC);
}

/**
 * SUBTEST: query-oa-units
 * Description: Display fields for OA unit query
 *
 * SUBTEST: multigpu-query-oa-units
 * Description: Display fields for OA unit query for all GPU devices
 * Mega feature: MultiGPU
 */
static void test_query_oa_units(int fd)
{
	struct drm_xe_query_oa_units *qoa;
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_OA_UNITS,
		.size = 0,
		.data = 0,
	};
	struct drm_xe_oa_unit *oau;
	int i, j;
	u8 *poau;

	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	qoa = malloc(query.size);
	igt_assert(qoa);

	query.data = to_user_pointer(qoa);
	igt_assert_eq(igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query), 0);

	igt_info("num_oa_units %d\n", qoa->num_oa_units);

	poau = (u8 *)&qoa->oa_units[0];
	for (i = 0; i < qoa->num_oa_units; i++) {
		oau = (struct drm_xe_oa_unit *)poau;

		igt_info("-------------------------------\n");
		igt_info("oa_unit %d\n", i);
		igt_info("-------------------------------\n");
		igt_info("oa_unit_id %d\n", oau->oa_unit_id);
		igt_info("oa_unit_type %d\n", oau->oa_unit_type);
		igt_info("capabilities %#llx\n", oau->capabilities);
		igt_info("oa_timestamp_freq %lld\n", oau->oa_timestamp_freq);
		igt_info("num_engines %lld\n", oau->num_engines);
		igt_info("Engines:");
		for (j = 0; j < oau->num_engines; j++)
			igt_info(" (%d, %d)", oau->eci[j].engine_class,
				 oau->eci[j].engine_instance);
		igt_info("\n");
		poau += sizeof(*oau) + j * sizeof(oau->eci[0]);
	}
}

/**
 * SUBTEST: query-pxp-status
 * Description: Display PXP supported types and current status
 *
 * SUBTEST: multigpu-query-pxp-status
 * Description: Display fields for PXP unit query for all Xe devices
 * Mega feature: MultiGPU
 */
static void test_query_pxp_status(int fd)
{
	struct drm_xe_query_pxp_status *qpxp;
	struct drm_xe_device_query query = {
		.extensions = 0,
		.query = DRM_XE_DEVICE_QUERY_PXP_STATUS,
		.size = 0,
		.data = 0,
	};
	int ret;

	/*
	 * if we run this test on an older kernel that doesn't have the PXP
	 * query, the ioctl will return -EINVAL.
	 */
	errno = 0;
	ret = igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query);
	igt_require(errno != EINVAL);
	igt_assert_eq(ret, 0);

	/* make sure the returned size is big enough */
	igt_assert(query.size >= sizeof(*qpxp));

	qpxp = malloc(query.size);
	igt_assert(qpxp);

	memset(qpxp, 0, query.size);

	query.data = to_user_pointer(qpxp);

	errno = 0;
	ret = igt_ioctl(fd, DRM_IOCTL_XE_DEVICE_QUERY, &query);
	if (errno == ENODEV) {
		igt_info("PXP not supported\n");
		free(qpxp);
		return;
	}

	igt_assert_eq(ret, 0);
	igt_assert_neq(qpxp->supported_session_types, 0);

	switch (qpxp->status) {
	case 0:
		igt_info("PXP initialization still in progress\n");
		break;
	case 1:
		igt_info("PXP initialization complete\n");
		break;
	default:
		igt_assert_f(0, "unexpected PXP status %u\n", qpxp->status);
	}

	igt_info("PXP supported types mask 0x%x\n", qpxp->supported_session_types);
	free(qpxp);
}

igt_main
{
	const struct {
		const char *name;
		void (*func)(int);
	} funcs[] = {
		{ "query-engines", test_query_engines },
		{ "query-mem-usage", test_query_mem_regions },
		{ "query-gt-list", test_query_gt_list },
		{ "query-config", test_query_config },
		{ "query-hwconfig", test_query_hwconfig },
		{ "query-topology", test_query_gt_topology },
		{ "query-topology-l3-bank-mask", test_query_gt_topology_l3_bank_mask },
		{ "query-cs-cycles", test_query_engine_cycles },
		{ "query-uc-fw-version-guc", test_query_uc_fw_version_guc },
		{ "query-uc-fw-version-huc", test_query_uc_fw_version_huc },
		{ "query-oa-units", test_query_oa_units },
		{ "query-pxp-status", test_query_pxp_status },
		{ "query-invalid-cs-cycles", test_engine_cycles_invalid },
		{ "query-invalid-query", test_query_invalid_query },
		{ "query-invalid-size", test_query_invalid_size },
		{ "query-invalid-extension", test_query_invalid_extension },
		{ "query-invalid-uc-fw-version-mbz", test_query_uc_fw_version_invalid_mbz },
		{ }
	}, *f;
	int xe, gpu_count;

	igt_fixture()
		xe = drm_open_driver(DRIVER_XE);

	for (f = funcs; f->name; f++) {
		igt_subtest_f("%s", f->name)
			f->func(xe);
	}

	igt_fixture() {
		drm_close_driver(xe);
		gpu_count = drm_prepare_filtered_multigpu(DRIVER_XE);
	}

	for (f = funcs; f->name; f++) {
		igt_subtest_f("multigpu-%s", f->name) {
			igt_require(gpu_count >= 2);
			intel_allocator_multiprocess_start(); /* for multigpu-query-cs-cycles */

			igt_multi_fork(child, gpu_count) {
				xe = drm_open_filtered_card(child);
				igt_assert_f(xe > 0, "cannot open gpu-%d, errno=%d\n", child, errno);
				igt_assert(is_xe_device(xe));

				f->func(xe);
				drm_close_driver(xe);
			}
			igt_waitchildren();
			intel_allocator_multiprocess_stop();
		}
	}
}
