#ifndef AMD_GCA_H
#define AMD_GCA_H

#define GCA_CONFIG_DEBUGFS_PATH "/sys/kernel/debug/dri/%d/amdgpu_gca_config"
#define MAX_CONFIG_SIZE 256

struct gca_config {
    uint32_t version;
    uint32_t max_shader_engines;
    uint32_t max_tile_pipes;
    uint32_t max_cu_per_sh;
    uint32_t max_sh_per_se;
    uint32_t max_backends_per_se;
    uint32_t max_texture_channel_caches;
    uint32_t max_gprs;
    uint32_t max_gs_threads;
    uint32_t max_hw_contexts;
    uint32_t sc_prim_fifo_size_frontend;
    uint32_t sc_prim_fifo_size_backend;
    uint32_t sc_hiz_tile_fifo_size;
    uint32_t sc_earlyz_tile_fifo_size;
    uint32_t num_tile_pipes;
    uint32_t backend_enable_mask;
    uint32_t mem_max_burst_length_bytes;
    uint32_t mem_row_size_in_kb;
    uint32_t shader_engine_tile_size;
    uint32_t num_gpus;
    uint32_t multi_gpu_tile_size;
    uint32_t mc_arb_ramcfg;
    uint32_t gb_addr_config;
    uint32_t num_rbs;
    uint32_t rev_id;
    uint32_t pg_flags;
    uint32_t cg_flags_lower;
    uint32_t family;
    uint32_t external_rev_id;
    uint32_t pci_device;
    uint32_t pci_revision;
    uint32_t pci_subsystem_device;
    uint32_t pci_subsystem_vendor;
    uint32_t is_apu;
    uint32_t reserved; /* was cg_flags_upper in newer versions */
    uint32_t cg_flags_upper;
};

int
read_gca_config_debugfs(int dri_index, uint32_t *config, size_t size);

void
validate_gca_config_basic(const uint32_t *config, size_t size);
#endif
