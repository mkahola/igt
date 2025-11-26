#include "amdgpu/amd_memory.h"
#include "amdgpu/amd_PM4.h"
#include "amdgpu/amd_ip_blocks.h"
#include "amd_gca.h"
#include <amdgpu.h>
#include <amdgpu_drm.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <inttypes.h>

int read_gca_config_debugfs(int dri_index, uint32_t *config, size_t size)
{
	int fd;
	char path[128];
	ssize_t bytes_read;

	snprintf(path, sizeof(path), GCA_CONFIG_DEBUGFS_PATH, dri_index);

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		igt_info("Failed to open debugfs file: %s\n", path);
		return -1;
	}

	bytes_read = read(fd, config, size);
	close(fd);

	if (bytes_read < 0) {
		igt_info("Failed to read from debugfs file: %s\n", path);
		return -1;
	}

	return bytes_read;
}

void validate_gca_config_basic(const uint32_t *config, size_t size)
{
	struct gca_config *gca = (struct gca_config *)config;
	uint64_t full_cg_flags;

	igt_assert_f(size >= sizeof(uint32_t), "Config size too small: %zu\n", size);
	igt_assert_f(size >= sizeof(struct gca_config),
		     "Config size too small for full structure: %zu, need %zu\n",
		     size, sizeof(struct gca_config));

	/* Check version field */
	igt_info("GCA Config version: %u\n", gca->version);
	igt_assert_f(gca->version >= 1 && gca->version <= 5,
		     "Invalid GCA config version: %u\n", gca->version);

	/* GPU Architecture Configuration */
	igt_info("=== GPU Architecture Configuration ===\n");
	igt_info("Max shader engines: %u\n", gca->max_shader_engines);
	igt_info("Max tile pipes: %u\n", gca->max_tile_pipes);
	igt_info("Max CU per SH: %u\n", gca->max_cu_per_sh);
	igt_info("Max SH per SE: %u\n", gca->max_sh_per_se);
	igt_info("Max backends per SE: %u\n", gca->max_backends_per_se);
	igt_info("Max texture channel caches: %u\n", gca->max_texture_channel_caches);

	/* Shader Configuration */
	igt_info("=== Shader Configuration ===\n");
	igt_info("Max GPRs: %u\n", gca->max_gprs);
	igt_info("Max GS threads: %u\n", gca->max_gs_threads);
	igt_info("Max HW contexts: %u\n", gca->max_hw_contexts);

	/* Shader Controller FIFO Sizes */
	igt_info("=== Shader Controller FIFO Sizes ===\n");
	igt_info("SC prim fifo size frontend: %u\n", gca->sc_prim_fifo_size_frontend);
	igt_info("SC prim fifo size backend: %u\n", gca->sc_prim_fifo_size_backend);
	igt_info("SC HIZ tile fifo size: %u\n", gca->sc_hiz_tile_fifo_size);
	igt_info("SC earlyZ tile fifo size: %u\n", gca->sc_earlyz_tile_fifo_size);

	/* Tile Configuration */
	igt_info("=== Tile Configuration ===\n");
	igt_info("Number of tile pipes: %u\n", gca->num_tile_pipes);
	igt_info("Backend enable mask: 0x%x\n", gca->backend_enable_mask);
	igt_info("Shader engine tile size: %u\n", gca->shader_engine_tile_size);

	/* Memory Configuration */
	igt_info("=== Memory Configuration ===\n");
	igt_info("Memory max burst length: %u bytes\n", gca->mem_max_burst_length_bytes);
	igt_info("Memory row size: %u KB\n", gca->mem_row_size_in_kb);
	igt_info("GB address config: 0x%x\n", gca->gb_addr_config);
	igt_info("MC arb ramcfg: 0x%x\n", gca->mc_arb_ramcfg);
	igt_info("Number of RBs: %u\n", gca->num_rbs);

	/* Multi-GPU Configuration */
	igt_info("=== Multi-GPU Configuration ===\n");
	igt_info("Number of GPUs: %u\n", gca->num_gpus);
	igt_info("Multi-GPU tile size: %u\n", gca->multi_gpu_tile_size);

	/* Version 1 Fields - Basic Device Info */
	if (gca->version >= 1) {
		igt_info("=== Version 1 Fields ===\n");
		igt_info("Revision ID: %u\n", gca->rev_id);
		igt_info("Power Gating (PG) flags: 0x%x\n", gca->pg_flags);
		igt_info("Clock Gating (CG) flags lower: 0x%x\n", gca->cg_flags_lower);

		/* Sanity checks for version 1 fields */
		igt_assert(gca->rev_id != 0xFFFFFFFF); /* Should be valid revision */
	}

	/* Version 2 Fields - Family Info */
	if (gca->version >= 2) {
		igt_info("=== Version 2 Fields ===\n");
		igt_info("GPU family: %u\n", gca->family);
		igt_info("External revision ID: %u\n", gca->external_rev_id);

		/* Sanity checks for version 2 fields */
		igt_assert(gca->family != 0); /* Should be valid family ID */
		igt_assert(gca->external_rev_id != 0xFFFFFFFF);
	}

	/* Version 3 Fields - PCI Info */
	if (gca->version >= 3) {
		igt_info("=== Version 3 Fields ===\n");
		igt_info("PCI device: 0x%x\n", gca->pci_device);
		igt_info("PCI revision: 0x%x\n", gca->pci_revision);
		igt_info("PCI subsystem device: 0x%x\n", gca->pci_subsystem_device);
		igt_info("PCI subsystem vendor: 0x%x\n", gca->pci_subsystem_vendor);

		/* Sanity checks for version 3 fields */
		igt_assert(gca->pci_device != 0); /* Should be valid PCI device ID */
		igt_assert(gca->pci_revision < 0xFF); /* PCI revision is 8-bit */
		igt_assert(gca->pci_subsystem_vendor < 0xFFFF); /* Valid vendor ID range */
	}

	/* Version 4 Fields - APU Flag */
	if (gca->version >= 4) {
		igt_info("=== Version 4 Fields ===\n");
		igt_info("Is APU: %s\n", gca->is_apu ? "yes" : "no");

		/* Sanity check for version 4 field */
		igt_assert(gca->is_apu == 0 || gca->is_apu == 1); /* Should be boolean */
	}

	/* Version 5 Fields - Extended CG Flags */
	if (gca->version >= 5) {
		igt_info("=== Version 5 Fields ===\n");
		igt_info("Reserved field: 0x%x\n", gca->reserved);
		igt_info("Clock Gating (CG) flags upper: 0x%x\n", gca->cg_flags_upper);

		/* Combined CG flags for versions >= 5 */
		full_cg_flags = ((uint64_t)gca->cg_flags_upper << 32) | gca->cg_flags_lower;
		igt_info("Full CG flags: 0x%016llx\n", (unsigned long long)full_cg_flags);
	}

	/* Architecture-specific validation */
	igt_info("=== Architecture Validation ===\n");

	/* Validate shader engine configuration */
	if (gca->max_shader_engines > 0) {
		igt_assert(gca->max_shader_engines <= 8); /* Reasonable maximum */
		igt_assert(gca->max_sh_per_se <= gca->max_shader_engines);
	}

	/* Validate CU configuration */
	if (gca->max_cu_per_sh > 0) {
		igt_assert(gca->max_cu_per_sh <= 32); /* Reasonable maximum per SH */
	}

	/* Validate memory configuration */
	if (gca->mem_row_size_in_kb > 0) {
		igt_assert(gca->mem_row_size_in_kb <= 32768); /* 32MB maximum row size */
	}

	/* Validate backend configuration */
	if (gca->max_backends_per_se > 0) {
		igt_assert(gca->max_backends_per_se <= 16); /* Reasonable maximum */
	}

	/* Validate tile pipe configuration */
	if (gca->num_tile_pipes > 0) {
		igt_assert(gca->num_tile_pipes <= gca->max_tile_pipes);
	}

	/* Multi-GPU validation */
	if (gca->num_gpus > 0) {
		igt_assert(gca->num_gpus <= 8); /* Reasonable maximum for multi-GPU */
	}

	/* Print summary */
	igt_info("=== GCA Config Summary ===\n");
	igt_info("Total configuration size: %zu bytes\n", size);
	igt_info("Expected structure size: %zu bytes\n", sizeof(struct gca_config));
	igt_info("Configuration version: %u\n", gca->version);
}
