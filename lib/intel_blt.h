/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef __INTEL_BLT_H__
#define __INTEL_BLT_H__

/**
 * SECTION:intel_blt
 * @short_description: i915/xe blitter library
 * @title: Blitter library
 * @include: intel_blt.h
 *
 * # Introduction
 *
 * Gen12+ blitter commands like XY_BLOCK_COPY_BLT are quite long
 * and if we would like to provide all arguments to function,
 * list would be long, unreadable and error prone to invalid argument placement.
 * Providing objects (structs) seems more reasonable and opens some more
 * opportunities to share some object data across different blitter commands.
 *
 * Blitter library supports no-reloc (softpin) mode only (apart of TGL
 * there's no relocations enabled) thus ahnd is mandatory. Providing NULL ctx
 * means we use default context with I915_EXEC_BLT as an execution engine.
 *
 * Library introduces tiling enum which distinguishes tiling formats regardless
 * legacy I915_TILING_... definitions. This allows to control fully what tilings
 * are handled by command and skip/assert ones which are not supported.
 *
 * # Supported commands
 *
 * - XY_BLOCK_COPY_BLT - (block-copy) TGL/DG1 + DG2+ (ext version)
 * - XY_FAST_COPY_BLT - (fast-copy)
 * - XY_CTRL_SURF_COPY_BLT - (ctrl-surf-copy) DG2+
 *
 * # Usage details
 *
 * For block-copy and fast-copy @blt_copy_object struct is used to collect
 * data about source and destination objects. It contains handle, region,
 * size, etc...  which are using for blits. Some fields are not used for
 * fast-copy copy (like compression) and command which use this exclusively
 * is annotated in the comment.
 *
 */

#include <errno.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <malloc.h>
#include "drm.h"
#include "igt.h"
#include "intel_cmds_info.h"

#define CCS_RATIO(fd) (intel_gen(intel_get_drm_devid(fd)) >= 20 ? 512 : 256)
#define GEN12_MEM_COPY_MOCS_SHIFT		25
#define XE2_MEM_COPY_SRC_MOCS_SHIFT		28
#define XE2_MEM_COPY_DST_MOCS_SHIFT		3

enum blt_color_depth {
	CD_8bit,
	CD_16bit,
	CD_32bit,
	CD_64bit,
	CD_96bit,
	CD_128bit,
};

enum blt_compression {
	COMPRESSION_DISABLED,
	COMPRESSION_ENABLED,
};

enum blt_compression_type {
	COMPRESSION_TYPE_3D,
	COMPRESSION_TYPE_MEDIA,
};

/* BC - block-copy */
struct blt_copy_object {
	uint32_t handle;
	uint32_t region;
	uint64_t size;
	uint8_t mocs_index;
	uint8_t pat_index;
	enum blt_tiling_type tiling;
	enum blt_compression compression;  /* BC only */
	enum blt_compression_type compression_type; /* BC only */
	uint32_t pitch;
	uint16_t x_offset, y_offset;
	int16_t x1, y1, x2, y2;

	/* mapping or null */
	uint32_t *ptr;

	/* enable to use multiplane framebuffers */
	uint32_t plane_offset;
};

struct blt_mem_object {
	uint32_t handle;
	uint32_t region;
	uint64_t size;
	uint8_t mocs_index;
	uint8_t pat_index;
	enum blt_compression compression;
	uint32_t width;
	uint32_t height;
	uint32_t pitch;
	uint32_t *ptr;
};

struct blt_copy_batch {
	uint32_t handle;
	uint32_t region;
	uint64_t size;
};

/* Common for block-copy and fast-copy */
struct blt_copy_data {
	int fd;
	enum intel_driver driver;
	struct blt_copy_object src;
	struct blt_copy_object dst;
	struct blt_copy_batch bb;
	enum blt_color_depth color_depth;

	/* debug stuff */
	bool print_bb;
};

struct blt_mem_copy_data {
	int fd;
	enum intel_driver driver;
	enum blt_memop_mode mode;
	enum blt_memop_type copy_type;
	struct blt_mem_object src;
	struct blt_mem_object dst;
	struct blt_copy_batch bb;
	bool print_bb;
};

struct blt_mem_set_data {
	int fd;
	enum intel_driver driver;
	enum blt_memop_type fill_type;
	struct blt_mem_object dst;
	struct blt_copy_batch bb;
};

enum blt_surface_type {
	SURFACE_TYPE_1D,
	SURFACE_TYPE_2D,
	SURFACE_TYPE_3D,
	SURFACE_TYPE_CUBE,
};

struct blt_block_copy_object_ext {
	uint8_t compression_format;
	bool clear_value_enable;
	uint64_t clear_address;
	uint16_t surface_width;
	uint16_t surface_height;
	enum blt_surface_type surface_type;
	uint16_t surface_qpitch;
	uint16_t surface_depth;
	uint8_t lod;
	uint8_t horizontal_align;
	uint8_t vertical_align;
	uint8_t mip_tail_start_lod;
	bool depth_stencil_resource;
	uint16_t array_index;
};

struct blt_block_copy_data_ext {
	struct blt_block_copy_object_ext src;
	struct blt_block_copy_object_ext dst;
};

enum blt_access_type {
	BLT_INDIRECT_ACCESS,
	BLT_DIRECT_ACCESS,
};

struct blt_ctrl_surf_copy_object {
	uint32_t handle;
	uint32_t region;
	uint64_t size;
	uint8_t mocs_index;
	uint8_t pat_index;
	enum blt_access_type access_type;
};

struct blt_ctrl_surf_copy_data {
	int fd;
	enum intel_driver driver;
	struct blt_ctrl_surf_copy_object src;
	struct blt_ctrl_surf_copy_object dst;
	struct blt_copy_batch bb;

	/* debug stuff */
	bool print_bb;
};

bool blt_supports_command(const struct intel_cmds_info *cmds_info,
			  enum blt_cmd_type cmd);
bool blt_cmd_supports_tiling(const struct intel_cmds_info *cmds_info,
			     enum blt_cmd_type cmd,
			     enum blt_tiling_type tiling);
bool blt_cmd_has_property(const struct intel_cmds_info *cmds_info,
			  enum blt_cmd_type cmd,
			  uint32_t prop);

bool blt_has_block_copy(int fd);
bool blt_has_mem_copy(int fd);
bool blt_has_mem_set(int fd);
bool blt_has_fast_copy(int fd);
bool blt_has_xy_src_copy(int fd);
bool blt_has_xy_color(int fd);

bool blt_fast_copy_supports_tiling(int fd, enum blt_tiling_type tiling);
bool blt_block_copy_supports_tiling(int fd, enum blt_tiling_type tiling);
bool blt_xy_src_copy_supports_tiling(int fd, enum blt_tiling_type tiling);
bool blt_block_copy_supports_compression(int fd);
bool blt_platform_has_flat_ccs_enabled(int fd);
bool blt_uses_extended_block_copy(int fd);
bool render_supports_tiling(int fd, enum blt_tiling_type tiling, bool compression);

const char *blt_tiling_name(enum blt_tiling_type tiling);
int blt_tile_to_i915_tile(enum blt_tiling_type tiling);
enum blt_tiling_type i915_tile_to_blt_tile(uint32_t tiling);

uint32_t blt_get_min_stride(uint32_t width, uint32_t bpp,
			    enum blt_tiling_type tiling);
uint32_t blt_get_aligned_height(uint32_t height, uint32_t bpp,
				enum blt_tiling_type tiling);

void blt_copy_init(int fd, struct blt_copy_data *blt);

uint64_t emit_blt_block_copy(int fd,
			     uint64_t ahnd,
			     const struct blt_copy_data *blt,
			     const struct blt_block_copy_data_ext *ext,
			     uint64_t bb_pos,
			     bool emit_bbe);

int blt_block_copy(int fd,
		   const intel_ctx_t *ctx,
		   const struct intel_execution_engine2 *e,
		   uint64_t ahnd,
		   const struct blt_copy_data *blt,
		   const struct blt_block_copy_data_ext *ext);

uint64_t emit_blt_ctrl_surf_copy(int fd,
				 uint64_t ahnd,
				 const struct blt_ctrl_surf_copy_data *surf,
				 uint64_t bb_pos,
				 bool emit_bbe);

void blt_ctrl_surf_copy_init(int fd, struct blt_ctrl_surf_copy_data *surf);

int blt_ctrl_surf_copy(int fd,
		       const intel_ctx_t *ctx,
		       const struct intel_execution_engine2 *e,
		       uint64_t ahnd,
		       const struct blt_ctrl_surf_copy_data *surf);

uint64_t emit_xe_flush_dw(int fd, const struct blt_copy_data *blt,
			  uint64_t bb_pos);

uint64_t emit_blt_fast_copy(int fd,
			    uint64_t ahnd,
			    const struct blt_copy_data *blt,
			    uint64_t bb_pos,
			    bool emit_bbe);

int blt_fast_copy(int fd,
		  const intel_ctx_t *ctx,
		  const struct intel_execution_engine2 *e,
		  uint64_t ahnd,
		  const struct blt_copy_data *blt);

void blt_mem_copy_init(int fd, struct blt_mem_copy_data *mem,
		       enum blt_memop_mode mode,
		       enum blt_memop_type copy_type);

void blt_mem_set_init(int fd, struct blt_mem_set_data *mem,
		      enum blt_memop_type fill_type);

int blt_mem_copy(int fd, const intel_ctx_t *ctx,
			 const struct intel_execution_engine2 *e,
			 uint64_t ahnd,
			 const struct blt_mem_copy_data *mem);

int blt_mem_set(int fd, const intel_ctx_t *ctx,
			const struct intel_execution_engine2 *e, uint64_t ahnd,
			const struct blt_mem_set_data *mem, uint8_t fill_data);

void blt_set_geom(struct blt_copy_object *obj, uint32_t pitch,
		  int16_t x1, int16_t y1, int16_t x2, int16_t y2,
		  uint16_t x_offset, uint16_t y_offset);
void blt_set_batch(struct blt_copy_batch *batch,
		   uint32_t handle, uint64_t size, uint32_t region);

struct blt_copy_object *
blt_create_object(const struct blt_copy_data *blt, uint32_t region,
		  uint32_t width, uint32_t height, uint32_t bpp, uint8_t mocs_index,
		  enum blt_tiling_type tiling,
		  enum blt_compression compression,
		  enum blt_compression_type compression_type,
		  bool create_mapping);
void blt_destroy_object(int fd, struct blt_copy_object *obj);
void blt_destroy_object_and_alloc_free(int fd, uint64_t ahnd,
				       struct blt_copy_object *obj);
void blt_set_object(struct blt_copy_object *obj,
		    uint32_t handle, uint64_t size, uint32_t region,
		    uint8_t mocs_index, uint8_t pat_index, enum blt_tiling_type tiling,
		    enum blt_compression compression,
		    enum blt_compression_type compression_type);

void blt_set_mem_object(struct blt_mem_object *obj,
			uint32_t handle, uint64_t size, uint32_t pitch,
			uint32_t width, uint32_t height, uint32_t region,
			uint8_t mocs_index, uint8_t pat_index,
			enum blt_compression compression);

void blt_set_object_ext(struct blt_block_copy_object_ext *obj,
			uint8_t compression_format,
			uint16_t surface_width, uint16_t surface_height,
			enum blt_surface_type surface_type);
void blt_set_copy_object(struct blt_copy_object *obj,
			 const struct blt_copy_object *orig);
void blt_set_ctrl_surf_object(struct blt_ctrl_surf_copy_object *obj,
			      uint32_t handle, uint32_t region, uint64_t size,
			      uint8_t mocs_index, uint8_t pat_index,
			      enum blt_access_type access_type);

void blt_surface_get_flatccs_data(int fd,
				  intel_ctx_t *ctx,
				  const struct intel_execution_engine2 *e,
				  uint64_t ahnd,
				  const struct blt_copy_object *obj,
				  uint32_t **ccsptr, uint64_t *sizeptr);
bool blt_surface_is_compressed(int fd,
			       intel_ctx_t *ctx,
			       const struct intel_execution_engine2 *e,
			       uint64_t ahnd,
			       const struct blt_copy_object *obj);
void blt_surface_info(const char *info,
		      const struct blt_copy_object *obj);
void blt_surface_fill_rect(int fd, const struct blt_copy_object *obj,
			   uint32_t width, uint32_t height);
void blt_surface_to_png(int fd, uint32_t run_id, const char *fileid,
			const struct blt_copy_object *obj,
			uint32_t width, uint32_t height, uint32_t bpp);
void blt_dump_corruption_info_32b(const struct blt_copy_object *surf1,
				  const struct blt_copy_object *surf2);

#endif
