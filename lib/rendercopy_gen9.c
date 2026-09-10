#include <assert.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <fcntl.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <getopt.h>

#include <drm.h>
#include <i915_drm.h>

#include "drmtest.h"
#include "intel_aux_pgtable.h"
#include "intel_bufops.h"
#include "intel_batchbuffer.h"
#include "intel_io.h"
#include "intel_mocs.h"
#include "rendercopy.h"
#include "surfaceformat.h"
#include "intel_reg.h"
#include "igt_aux.h"
#include "intel_chipset.h"
#include "intel/genxml/igt_genxml.h"
#include "gen90_pack.h"
#include "gen110_pack.h"
#include "gen120_pack.h"
#include "gen125_pack.h"
#include "xe2_pack.h"

#define VERTEX_SIZE (3*4)

#if DEBUG_RENDERCPY
static void dump_batch(struct intel_bb *ibb)
{
	intel_bb_dump(ibb, "/tmp/gen9-batchbuffers.dump", true);
}
#else
#define dump_batch(x) do { } while(0)
#endif

static struct {
	uint32_t cc_state;
	uint32_t blend_state;
} cc;

static struct {
	uint32_t cc_state;
	uint32_t sf_clip_state;
} viewport;

/* see lib/i915/shaders/ps/blit.g7a */
static const uint32_t ps_kernel_gen9[][4] = {
#if 1
	{ 0x0080005a, 0x2f403ae8, 0x3a0000c0, 0x008d0040 },
	{ 0x0080005a, 0x2f803ae8, 0x3a0000d0, 0x008d0040 },
	{ 0x02800031, 0x2e203a48, 0x0e8d0f40, 0x08840001 },
	{ 0x05800031, 0x20003a40, 0x0e8d0e20, 0x90031000 },
#else
	/* Write all -1 */
	{ 0x00600001, 0x2e000608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2e200608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2e400608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2e600608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2e800608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2ea00608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2ec00608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2ee00608, 0x00000000, 0x3f800000 },
	{ 0x05800031, 0x200022e0, 0x0e000e00, 0x90031000 },
#endif
};

/* see lib/i915/shaders/ps/blit.g11a */
static const uint32_t ps_kernel_gen11[][4] = {
#if 1
	{ 0x0060005b, 0x2000c01c, 0x07206601, 0x01800404 },
	{ 0x0060005b, 0x7100480c, 0x0722003b, 0x01880406 },
	{ 0x0060005b, 0x2000c01c, 0x07206601, 0x01800408 },
	{ 0x0060005b, 0x7200480c, 0x0722003b, 0x0188040a },
	{ 0x0060005b, 0x2000c01c, 0x07206e01, 0x01a00404 },
	{ 0x0060005b, 0x7300480c, 0x0722003b, 0x01a80406 },
	{ 0x0060005b, 0x2000c01c, 0x07206e01, 0x01a00408 },
	{ 0x0060005b, 0x7400480c, 0x0722003b, 0x01a8040a },
	{ 0x02800031, 0x21804a4c, 0x06000e20, 0x08840001 },
	{ 0x00800001, 0x2e204b28, 0x008d0180, 0x00000000 },
	{ 0x00800001, 0x2e604b28, 0x008d01c0, 0x00000000 },
	{ 0x00800001, 0x2ea04b28, 0x008d0200, 0x00000000 },
	{ 0x00800001, 0x2ee04b28, 0x008d0240, 0x00000000 },
	{ 0x05800031, 0x20004a44, 0x06000e20, 0x90031000 },
#else
	/* Write all -1 */
	{ 0x00600001, 0x2e000608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2e200608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2e400608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2e600608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2e800608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2ea00608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2ec00608, 0x00000000, 0x3f800000 },
	{ 0x00600001, 0x2ee00608, 0x00000000, 0x3f800000 },
	{ 0x05800031, 0x200022e0, 0x0e000e00, 0x90031000 },
#endif
};

/* see lib/i915/shaders/ps/gen12_render_copy.asm */
static const uint32_t gen12_render_copy[][4] = {
	{ 0x8003005b, 0x200002f0, 0x0a0a0664, 0x06040205 },
	{ 0x8003005b, 0x71040fa8, 0x0a0a2001, 0x06240305 },
	{ 0x8003005b, 0x200002f0, 0x0a0a0664, 0x06040405 },
	{ 0x8003005b, 0x72040fa8, 0x0a0a2001, 0x06240505 },
	{ 0x8003005b, 0x200002f0, 0x0a0a06e4, 0x06840205 },
	{ 0x8003005b, 0x73040fa8, 0x0a0a2001, 0x06a40305 },
	{ 0x8003005b, 0x200002f0, 0x0a0a06e4, 0x06840405 },
	{ 0x8003005b, 0x74040fa8, 0x0a0a2001, 0x06a40505 },
	{ 0x80049031, 0x0c440000, 0x20027124, 0x01000000 },
	{ 0x00042061, 0x71050aa0, 0x00460c05, 0x00000000 },
	{ 0x00040061, 0x73050aa0, 0x00460e05, 0x00000000 },
	{ 0x00040061, 0x75050aa0, 0x00461005, 0x00000000 },
	{ 0x00040061, 0x77050aa0, 0x00461205, 0x00000000 },
	{ 0x80040131, 0x00000004, 0x50007144, 0x00c40000 },
};

/* see lib/i915/shaders/ps/gen12p71_render_copy.asm */
static const uint32_t gen12p71_render_copy[][4] = {
	{ 0x8003005b, 0x200002a0, 0x0a0a0664, 0x06040205 },
	{ 0x8003005b, 0x71040aa8, 0x0a0a2001, 0x06240305 },
	{ 0x8003005b, 0x200002a0, 0x0a0a0664, 0x06040405 },
	{ 0x8003005b, 0x72040aa8, 0x0a0a2001, 0x06240505 },
	{ 0x8003005b, 0x200002a0, 0x0a0a06e4, 0x06840205 },
	{ 0x8003005b, 0x73040aa8, 0x0a0a2001, 0x06a40305 },
	{ 0x8003005b, 0x200002a0, 0x0a0a06e4, 0x06840405 },
	{ 0x8003005b, 0x74040aa8, 0x0a0a2001, 0x06a40505 },
	{ 0x80031101, 0x00010000, 0x00000000, 0x00000000 },
	{ 0x80044031, 0x0c440000, 0x20027124, 0x01000000 },
	{ 0x00042061, 0x71050aa0, 0x00460c05, 0x00000000 },
	{ 0x00040061, 0x73050aa0, 0x00460e05, 0x00000000 },
	{ 0x00040061, 0x75050aa0, 0x00461005, 0x00000000 },
	{ 0x00040061, 0x77050aa0, 0x00461205, 0x00000000 },
	{ 0x80041131, 0x00000004, 0x50007144, 0x00c40000 },
};

static const uint32_t xe2_render_copy[][4] = {
	{ 0x8010005b, 0x200002a0, 0x020a0604, 0x06640104 },
	{ 0x8010005b, 0x710402a8, 0x020a2000, 0x06140104 },
	{ 0x8010005b, 0x200002a0, 0x020a0634, 0x06440104 },
	{ 0x8010005b, 0x720402a8, 0x020a2000, 0x06540204 },
	{ 0x80122031, 0x0c240000, 0x20027114, 0x00800000 },
	{ 0x8010c031, 0x00000004, 0x58000c24, 0x00c40000 },
};

static const uint32_t xe3p_render_copy[][4] = {
	{ 0x8010005b, 0x200002a0, 0x02020604, 0x06640104 },
	{ 0x8010005b, 0x710402a8, 0x02022000, 0x06140104 },
	{ 0x8010005b, 0x200002a0, 0x02020634, 0x06440104 },
	{ 0x8010005b, 0x720402a8, 0x02022000, 0x06540204 },
	{ 0x80122031, 0x0c240000, 0x20027114, 0x00800000 },
	{ 0x8010c031, 0x00000004, 0x58000c24, 0x00c40000 },
};

static uint32_t lnl_compression_format(const struct intel_buf *buf)
{
	switch (buf->bpp) {
	case 64:
		return 0x7; /* CMF_R16_G16_B16_A16 */
	case 32:
		if (buf->depth == 30)
			return 0x3; /* CMF_R10_G10_B10_A2 */
		else
			return 0x2; /* CMF_R8_G8_B8_A8 */
	default:
		igt_assert(0);
		return 0;
	}
}

static uint32_t dg2_compression_format(const struct intel_buf *buf)
{
	switch (buf->bpp) {
	case 64:
		return 0x5;
	case 32:
		if (buf->depth == 30)
			return 0xc;
		else
			return 0x8;
	default:
		igt_assert(0);
		return 0;
	}
}

/*
 * IGT_RSS_COMMON - set RENDER_SURFACE_STATE fields shared across all gens.
 * Works via C preprocessor structural typing: all gen-specific structs
 * have identical field names for these members.
 */
#define IGT_RSS_COMMON(ss, buf, mocs_val, rd, wd)                          \
	do {                                                               \
		ss.SurfaceType = GFX9_SURFTYPE_2D;                        \
		ss.SurfaceFormat = gen4_surface_format((buf)->bpp,         \
					       (buf)->depth);      \
		ss.SurfaceVerticalAlignment = GFX9_VALIGN_4;              \
		ss.MOCS = (mocs_val);                                      \
		ss.Width = intel_buf_width(buf) - 1;                       \
		ss.Height = intel_buf_height(buf) - 1;                     \
		ss.SurfacePitch = (buf)->surface[0].stride - 1;            \
		ss.ShaderChannelSelectRed = (int)GFX9_SCS_RED;                 \
		ss.ShaderChannelSelectGreen = (int)GFX9_SCS_GREEN;             \
		ss.ShaderChannelSelectBlue = (int)GFX9_SCS_BLUE;               \
		ss.ShaderChannelSelectAlpha = (int)GFX9_SCS_ALPHA;             \
		ss.SurfaceBaseAddress =                                    \
			igt_address_of((buf), (buf)->surface[0].offset,    \
				       (rd), (wd));                        \
	} while (0)

/*
 * IGT_RSS_TILING - set TileMode from buf->tiling.  The numeric encoding is
 * identical across gen9/gen12/gen12.5/xe2, but the enum names differ per gen.
 * We use xe2 (GFX20) names as they best reflect the modern tile semantics;
 * (int) casts suppress -Wenum-conversion when used with older-gen structs.
 */
#define IGT_RSS_TILING(ss, buf)                                            \
	do {                                                               \
		switch ((buf)->tiling) {                                    \
		case I915_TILING_NONE:                                     \
			ss.TileMode = (int)GFX20_LINEAR;                  \
			break;                                             \
		case I915_TILING_X:                                        \
			ss.TileMode = (int)GFX20_XMAJOR;                  \
			break;                                             \
		case I915_TILING_64:                                       \
			ss.TileMode = (int)GFX20_TILE64;                  \
			ss.MipTailStartLOD = 0xf;                          \
			break;                                             \
		default:                                                   \
			ss.TileMode = (int)GFX20_TILE4;                   \
		}                                                          \
	} while (0)

static uint32_t
gen9_bind_buf(struct intel_bb *ibb, const struct intel_buf *buf, int is_dst) {
	uint32_t write_domain, read_domain;
	unsigned int gen = intel_gen(ibb->devid);
	uint32_t mocs;
	void *ss_ptr;

	igt_assert_lte(buf->surface[0].stride, 256*1024);
	igt_assert_lte(intel_buf_width(buf), 16384);
	igt_assert_lte(intel_buf_height(buf), 16384);

	if (is_dst) {
		write_domain = read_domain = I915_GEM_DOMAIN_RENDER;
	} else {
		write_domain = 0;
		read_domain = I915_GEM_DOMAIN_SAMPLER;
	}

	mocs = intel_buf_mocs(buf);

	ss_ptr = intel_bb_ptr_align(ibb, 64);

	if (gen >= 20) {
		/* -- Xe2 (LNL+) -- */
		igt_genxml_pack_state(ibb, GFX20_RENDER_SURFACE_STATE, ss_ptr, ss) {
			IGT_RSS_COMMON(ss, buf, mocs, read_domain, write_domain);
			IGT_RSS_TILING(ss, buf);
			ss.SurfaceHorizontalAlignment = GFX20_HALIGN_32;

			if (buf->compression == I915_COMPRESSION_RENDER) {
				ss.AuxiliarySurfaceMode = GFX20_AUX_NONE;
				ss.CompressionFormat = lnl_compression_format(buf);
			}
		}

	} else if (HAS_4TILE(ibb->devid)) {
		/* -- Gen12.5 / DG2 -- */
		igt_genxml_pack_state(ibb, GFX125_RENDER_SURFACE_STATE, ss_ptr, ss) {
			IGT_RSS_COMMON(ss, buf, mocs, read_domain, write_domain);
			IGT_RSS_TILING(ss, buf);
			ss.SurfaceHorizontalAlignment = GFX125_HALIGN_32;

			if (buf->compression == I915_COMPRESSION_MEDIA) {
				ss.MemoryCompressionEnable = true;
				ss.MemoryCompressionMode = GFX125_MEDIACOMPRESSION;
			} else if (buf->compression == I915_COMPRESSION_RENDER) {
				ss.AuxiliarySurfaceMode = GFX125_AUX_CCS_E;
				ss.CompressionFormat = dg2_compression_format(buf);

				if (buf->cc.offset) {
					struct igt_address clear_addr =
						igt_address_of(buf, buf->cc.offset,
							       read_domain, write_domain);

					/*
					 * If this assert doesn't hold the clear
					 * address will be packed wrong; it must be
					 * 64-byte aligned and fit the field.
					 */
					igt_assert(__builtin_ctzl(clear_addr.offset) >= 6 &&
						   __builtin_clzl(clear_addr.offset) >= 16);

					ss.ClearValueAddressEnable = true;
					ss.ClearValueAddress = clear_addr;
				}
			}
		}

	} else if (gen >= 12) {
		/* -- Gen12 / TGL -- */
		igt_genxml_pack_state(ibb, GFX12_RENDER_SURFACE_STATE, ss_ptr, ss) {
			IGT_RSS_COMMON(ss, buf, mocs, read_domain, write_domain);
			IGT_RSS_TILING(ss, buf);
			ss.SurfaceHorizontalAlignment = GFX12_HALIGN_4;
			ss.RenderCacheReadWriteMode = GFX12_READWRITECACHE;
			ss.MipTailStartLOD = 1;

			if (buf->compression == I915_COMPRESSION_MEDIA) {
				ss.MemoryCompressionEnable = true;
				ss.MemoryCompressionMode = GFX12_HORIZONTAL;
			} else if (buf->compression == I915_COMPRESSION_RENDER) {
				ss.AuxiliarySurfaceMode = GFX12_AUX_CCS_E;

				if (buf->cc.offset) {
					struct igt_address clear_addr =
						igt_address_of(buf, buf->cc.offset,
							       read_domain, write_domain);

					/*
					 * If this assert doesn't hold the clear
					 * address will be packed wrong; it must be
					 * 64-byte aligned and fit the field.
					 */
					igt_assert(__builtin_ctzl(clear_addr.offset) >= 6 &&
						   __builtin_clzl(clear_addr.offset) >= 16);

					ss.ClearValueAddressEnable = true;
					ss.ClearValueAddress = clear_addr;
				}
			}
		}

	} else {
		/* -- Gen9 / Gen11 -- */
		igt_genxml_pack_state(ibb, GFX9_RENDER_SURFACE_STATE, ss_ptr, ss) {
			IGT_RSS_COMMON(ss, buf, mocs, read_domain, write_domain);
			IGT_RSS_TILING(ss, buf);
			ss.SurfaceHorizontalAlignment = GFX9_HALIGN_4;
			ss.RenderCacheReadWriteMode = GFX9_READWRITECACHE;
			ss.MipTailStartLOD = 1;

			if (buf->tiling == I915_TILING_Yf)
				ss.TiledResourceMode = GFX9_TILEYF;
			else if (buf->tiling == I915_TILING_Ys)
				ss.TiledResourceMode = GFX9_TILEYS;

			if (buf->compression == I915_COMPRESSION_MEDIA) {
				ss.MemoryCompressionEnable = true;
				ss.MemoryCompressionMode = GFX9_HORIZONTAL;
			} else if (buf->compression == I915_COMPRESSION_RENDER) {
				ss.AuxiliarySurfaceMode = GFX9_AUX_CCS_E;

				if (buf->ccs[0].stride) {
					ss.AuxiliarySurfacePitch =
						(buf->ccs[0].stride / 128) - 1;
					ss.AuxiliarySurfaceBaseAddress =
						igt_address_of(buf, buf->ccs[0].offset,
							       read_domain, write_domain);
				}
			}
		}
	}

	return intel_bb_ptr_add_return_prev_offset(ibb,
		GFX9_RENDER_SURFACE_STATE_length * 4);
}

static uint32_t
gen8_bind_surfaces(struct intel_bb *ibb,
		   const struct intel_buf *src,
		   const struct intel_buf *dst)
{
	uint32_t *binding_table, binding_table_offset;

	binding_table = intel_bb_ptr_align(ibb, 32);
	binding_table_offset = intel_bb_ptr_add_return_prev_offset(ibb, 32);

	binding_table[0] = gen9_bind_buf(ibb, dst, 1);

	if (src != NULL)
		binding_table[1] = gen9_bind_buf(ibb, src, 0);

	return binding_table_offset;
}

/* Mostly copy+paste from gen6, except wrap modes moved */
static uint32_t
gen8_create_sampler(struct intel_bb *ibb) {
	void *ptr = intel_bb_ptr_align(ibb, 64);

	igt_genxml_pack_state(ibb, GFX9_SAMPLER_STATE, ptr, ss) {
		ss.MinModeFilter = GFX9_MAPFILTER_NEAREST;
		ss.MagModeFilter = GFX9_MAPFILTER_NEAREST;
		ss.TCZAddressControlMode = GFX9_TCM_CLAMP;
		ss.TCYAddressControlMode = GFX9_TCM_CLAMP;
		ss.TCXAddressControlMode = GFX9_TCM_CLAMP;
	}

	return intel_bb_ptr_add_return_prev_offset(ibb,
						   GFX9_SAMPLER_STATE_length * 4);
}

static uint32_t
gen8_fill_ps(struct intel_bb *ibb,
	     const uint32_t kernel[][4],
	     size_t size)
{
	return intel_bb_copy_data(ibb, kernel, size, 64);
}

static void fast_clear_scale(const struct intel_buf *buf,
			     int *x_scale, int *y_scale)
{
	switch (buf->tiling) {
	case I915_TILING_4:
		*x_scale = 1024 * 8 / buf->bpp;
		*y_scale = 16;
		break;
	case I915_TILING_64:
		switch (buf->bpp) {
		case 8:
			*x_scale = 128;
			*y_scale = 128;
			break;
		case 16:
			*x_scale = 128;
			*y_scale = 64;
			break;
		case 32:
			*x_scale = 64;
			*y_scale = 64;
			break;
		case 64:
			*x_scale = 64;
			*y_scale = 32;
			break;
		case 128:
			*x_scale = 32;
			*y_scale = 32;
			break;
		}
		break;
	case I915_TILING_Y:
		*x_scale = 256 * 8 / buf->bpp;
		*y_scale = 16;
		break;
	case I915_TILING_Yf:
		switch (buf->bpp) {
		case 8:
			*x_scale = 128;
			*y_scale = 32;
			break;
		case 16:
			*x_scale = 128;
			*y_scale = 16;
			break;
		case 32:
			*x_scale = 64;
			*y_scale = 16;
			break;
		case 64:
			*x_scale = 64;
			*y_scale = 8;
			break;
		case 128:
			*x_scale = 32;
			*y_scale = 8;
			break;
		}
		break;
	case I915_TILING_Ys:
		switch (buf->bpp) {
		case 8:
			*x_scale = 64;
			*y_scale = 64;
			break;
		case 16:
			*x_scale = 64;
			*y_scale = 32;
			break;
		case 32:
			*x_scale = 32;
			*y_scale = 32;
			break;
		case 64:
			*x_scale = 32;
			*y_scale = 16;
			break;
		case 128:
			*x_scale = 16;
			*y_scale = 16;
			break;
		}
		break;
	default:
		igt_assert(0);
	}
}

/*
 * gen7_fill_vertex_buffer_data populate vertex buffer with data.
 *
 * The vertex buffer consists of 3 vertices to construct a RECTLIST. The 4th
 * vertex is implied (automatically derived by the HW). Each element has the
 * destination offset, and the normalized texture offset (src). The rectangle
 * itself will span the entire subsurface to be copied.
 *
 * see gen6_emit_vertex_elements
 */
static uint32_t
gen7_fill_vertex_buffer_data(struct intel_bb *ibb,
			     const struct intel_buf *src,
			     uint32_t src_x, uint32_t src_y,
			     const struct intel_buf *dst,
			     uint32_t dst_x, uint32_t dst_y,
			     uint32_t width, uint32_t height)
{
	uint32_t offset;

	intel_bb_ptr_align(ibb, 8);
	offset = intel_bb_offset(ibb);

	if (src != NULL) {
		emit_vertex_2s(ibb, dst_x + width, dst_y + height);

		emit_vertex_normalized(ibb, src_x + width, intel_buf_width(src));
		emit_vertex_normalized(ibb, src_y + height, intel_buf_height(src));

		emit_vertex_2s(ibb, dst_x, dst_y + height);

		emit_vertex_normalized(ibb, src_x, intel_buf_width(src));
		emit_vertex_normalized(ibb, src_y + height, intel_buf_height(src));

		emit_vertex_2s(ibb, dst_x, dst_y);

		emit_vertex_normalized(ibb, src_x, intel_buf_width(src));
		emit_vertex_normalized(ibb, src_y, intel_buf_height(src));
	} else {
		int x_scale, y_scale;

		fast_clear_scale(dst, &x_scale, &y_scale);

		emit_vertex_2s(ibb, DIV_ROUND_UP(dst_x + width, x_scale), DIV_ROUND_UP(dst_y + height, y_scale));

		emit_vertex_normalized(ibb, 0, 0);
		emit_vertex_normalized(ibb, 0, 0);

		emit_vertex_2s(ibb, dst_x/x_scale, DIV_ROUND_UP(dst_y + height, y_scale));

		emit_vertex_normalized(ibb, 0, 0);
		emit_vertex_normalized(ibb, 0, 0);

		emit_vertex_2s(ibb, dst_x/x_scale, dst_y/y_scale);

		emit_vertex_normalized(ibb, 0, 0);
		emit_vertex_normalized(ibb, 0, 0);
	}

	return offset;
}

/*
 * gen6_emit_vertex_elements - The vertex elements describe the contents of the
 * vertex buffer. We pack the vertex buffer in a semi weird way, conforming to
 * what gen6_rendercopy did. The most straightforward would be to store
 * everything as floats.
 *
 * see gen7_fill_vertex_buffer_data() for where the corresponding elements are
 * packed.
 */
static void
gen6_emit_vertex_elements(struct intel_bb *ibb) {
	void *ve_ptr;

	/*
	 * The VUE layout
	 *    dword 0-3: pad (0, 0, 0. 0)
	 *    dword 4-7: position (x, y, 0, 1.0),
	 *    dword 8-11: texture coordinate 0 (u0, v0, 0, 1.0)
	 */
	{
		struct GFX9_3DSTATE_VERTEX_ELEMENTS ves = { GFX9_3DSTATE_VERTEX_ELEMENTS_header };
		ves.DWordLength = 3 * GFX9_VERTEX_ELEMENT_STATE_length - 1;
		GFX9_3DSTATE_VERTEX_ELEMENTS_pack(ibb, intel_bb_ptr(ibb), &ves);
		intel_bb_ptr_add(ibb, 4);
	}

	/* Element state 0. These are 4 dwords of 0 required for the VUE format. */
	ve_ptr = intel_bb_ptr(ibb);
	igt_genxml_pack_state(ibb, GFX9_VERTEX_ELEMENT_STATE, ve_ptr, ve0) {
		ve0.VertexBufferIndex = 0;
		ve0.Valid = true;
		ve0.SourceElementFormat = SURFACEFORMAT_R32G32B32A32_FLOAT;
		ve0.SourceElementOffset = 0;
		ve0.Component0Control = GFX9_VFCOMP_STORE_0;
		ve0.Component1Control = GFX9_VFCOMP_STORE_0;
		ve0.Component2Control = GFX9_VFCOMP_STORE_0;
		ve0.Component3Control = GFX9_VFCOMP_STORE_0;
	}
	intel_bb_ptr_add(ibb, GFX9_VERTEX_ELEMENT_STATE_length * 4);

	/* Element state 1 - destination vertices (16-bit signed/scaled). */
	ve_ptr = intel_bb_ptr(ibb);
	igt_genxml_pack_state(ibb, GFX9_VERTEX_ELEMENT_STATE, ve_ptr, ve1) {
		ve1.VertexBufferIndex = 0;
		ve1.Valid = true;
		ve1.SourceElementFormat = SURFACEFORMAT_R16G16_SSCALED;
		ve1.SourceElementOffset = 0;
		ve1.Component0Control = GFX9_VFCOMP_STORE_SRC;
		ve1.Component1Control = GFX9_VFCOMP_STORE_SRC;
		ve1.Component2Control = GFX9_VFCOMP_STORE_0;
		ve1.Component3Control = GFX9_VFCOMP_STORE_1_FP;
	}
	intel_bb_ptr_add(ibb, GFX9_VERTEX_ELEMENT_STATE_length * 4);

	/* Element state 2 - texture coordinates (normalized floats). */
	ve_ptr = intel_bb_ptr(ibb);
	igt_genxml_pack_state(ibb, GFX9_VERTEX_ELEMENT_STATE, ve_ptr, ve2) {
		ve2.VertexBufferIndex = 0;
		ve2.Valid = true;
		ve2.SourceElementFormat = SURFACEFORMAT_R32G32_FLOAT;
		ve2.SourceElementOffset = 4;
		ve2.Component0Control = GFX9_VFCOMP_STORE_SRC;
		ve2.Component1Control = GFX9_VFCOMP_STORE_SRC;
		ve2.Component2Control = GFX9_VFCOMP_STORE_0;
		ve2.Component3Control = GFX9_VFCOMP_STORE_1_FP;
	}
	intel_bb_ptr_add(ibb, GFX9_VERTEX_ELEMENT_STATE_length * 4);
}

/*
 * gen7_emit_vertex_buffer emit the vertex buffers command
 *
 * @batch
 * @offset - bytw offset within the @batch where the vertex buffer starts.
 */
static void gen7_emit_vertex_buffer(struct intel_bb *ibb, uint32_t offset)
{
	struct GFX9_3DSTATE_VERTEX_BUFFERS vbs = { GFX9_3DSTATE_VERTEX_BUFFERS_header };
	void *vb_ptr;

	/*
	 * Variable-length: 1 header dword + VERTEX_BUFFER_STATE element.
	 * Default DWordLength=3 is correct for 1 element (1 + 4 - 2 = 3).
	 */
	GFX9_3DSTATE_VERTEX_BUFFERS_pack(ibb, intel_bb_ptr(ibb), &vbs);
	intel_bb_ptr_add(ibb, 4);

	vb_ptr = intel_bb_ptr(ibb);
	igt_genxml_pack_state(ibb, GFX9_VERTEX_BUFFER_STATE, vb_ptr, vb) {
		vb.VertexBufferIndex = 0;
		vb.AddressModifyEnable = true;
		vb.MOCS = intel_get_wb_mocs(ibb->fd);
		vb.BufferPitch = VERTEX_SIZE;
		vb.BufferStartingAddress = (struct igt_address){
			.offset = ibb->batch_offset + offset,
			.handle = ibb->handle,
			.read_domains = I915_GEM_DOMAIN_VERTEX,
			.write_domain = 0,
			.presumed_offset = ibb->batch_offset,
		};
		vb.BufferSize = 3 * VERTEX_SIZE;
	}
	intel_bb_ptr_add(ibb, GFX9_VERTEX_BUFFER_STATE_length * 4);
}

static uint32_t
gen6_create_cc_state(struct intel_bb *ibb)
{
	void *ptr = intel_bb_ptr_align(ibb, 64);

	igt_genxml_pack_state(ibb, GFX9_COLOR_CALC_STATE, ptr, cc_state) { }

	return intel_bb_ptr_add_return_prev_offset(ibb,
						   GFX9_COLOR_CALC_STATE_length * 4);
}

static uint32_t
gen8_create_blend_state(struct intel_bb *ibb)
{
	void *ptr = intel_bb_ptr_align(ibb, 64);

	/* Blend state header (1 dword) - all defaults (zeros) */
	igt_genxml_pack_state(ibb, GFX9_BLEND_STATE, ptr, bs) { }
	ptr += GFX9_BLEND_STATE_length * 4;

	/* 16 per-RT blend state entries */
	for (int i = 0; i < 16; i++) {
		igt_genxml_pack_state(ibb, GFX9_BLEND_STATE_ENTRY, ptr, entry) {
			entry.DestinationBlendFactor = GFX9_BLENDFACTOR_ZERO;
			entry.SourceBlendFactor = GFX9_BLENDFACTOR_ONE;
			entry.ColorBlendFunction = GFX9_BLENDFUNCTION_ADD;
			entry.PreBlendColorClampEnable = true;
		}
		ptr += GFX9_BLEND_STATE_ENTRY_length * 4;
	}

	return intel_bb_ptr_add_return_prev_offset(ibb,
		(GFX9_BLEND_STATE_length + 16 * GFX9_BLEND_STATE_ENTRY_length) * 4);
}

static uint32_t
gen6_create_cc_viewport(struct intel_bb *ibb)
{
	void *ptr = intel_bb_ptr_align(ibb, 32);

	igt_genxml_pack_state(ibb, GFX9_CC_VIEWPORT, ptr, vp) {
		vp.MinimumDepth = -1.e35;
		vp.MaximumDepth = 1.e35;
	}

	return intel_bb_ptr_add_return_prev_offset(ibb,
						   GFX9_CC_VIEWPORT_length * 4);
}

static uint32_t
gen7_create_sf_clip_viewport(struct intel_bb *ibb) {
	void *ptr = intel_bb_ptr_align(ibb, 64);

	igt_genxml_pack_state(ibb, GFX9_SF_CLIP_VIEWPORT, ptr, scv) {
		scv.XMinClipGuardband = 0;
		scv.XMaxClipGuardband = 1.0f;
		scv.YMinClipGuardband = 0;
		scv.YMaxClipGuardband = 1.0f;
	}

	return intel_bb_ptr_add_return_prev_offset(ibb,
						   GFX9_SF_CLIP_VIEWPORT_length * 4);
}

static uint32_t
gen6_create_scissor_rect(struct intel_bb *ibb)
{
	void *ptr = intel_bb_ptr_align(ibb, 64);

	igt_genxml_pack_state(ibb, GFX9_SCISSOR_RECT, ptr, sr) { }

	return intel_bb_ptr_add_return_prev_offset(ibb,
						   GFX9_SCISSOR_RECT_length * 4);
}

static void
gen8_emit_sip(struct intel_bb *ibb) {
	igt_genxml_emit(ibb, GFX9_STATE_SIP, sip) {
		/* SystemInstructionPointer left as zero */
	}
}

static void
gen7_emit_push_constants(struct intel_bb *ibb) {
	igt_genxml_emit(ibb, GFX9_3DSTATE_PUSH_CONSTANT_ALLOC_VS, vs) { }
	igt_genxml_emit(ibb, GFX9_3DSTATE_PUSH_CONSTANT_ALLOC_HS, hs) { }
	igt_genxml_emit(ibb, GFX9_3DSTATE_PUSH_CONSTANT_ALLOC_DS, ds) { }
	igt_genxml_emit(ibb, GFX9_3DSTATE_PUSH_CONSTANT_ALLOC_GS, gs) { }
	igt_genxml_emit(ibb, GFX9_3DSTATE_PUSH_CONSTANT_ALLOC_PS, ps) { }
}

/*
 * IGT_SBA_COMMON - shared STATE_BASE_ADDRESS fields across gen9/gen11/gen125.
 * All three variants have identical field names for the fields we set.
 */
#define IGT_SBA_COMMON(sba, mocs_val, surf, dyn, inst)                     \
	do {                                                               \
		sba.GeneralStateBaseAddressModifyEnable = true;            \
		sba.GeneralStateMOCS = (mocs_val);                         \
		sba.StatelessDataPortAccessMOCS = (mocs_val);              \
		sba.SurfaceStateBaseAddressModifyEnable = true;            \
		sba.SurfaceStateMOCS = (mocs_val);                         \
		sba.SurfaceStateBaseAddress = (surf);                      \
		sba.DynamicStateBaseAddressModifyEnable = true;            \
		sba.DynamicStateMOCS = (mocs_val);                         \
		sba.DynamicStateBaseAddress = (dyn);                       \
		sba.IndirectObjectMOCS = (mocs_val);                       \
		sba.InstructionBaseAddressModifyEnable = true;             \
		sba.InstructionMOCS = (mocs_val);                          \
		sba.InstructionBaseAddress = (inst);                       \
		sba.GeneralStateBufferSizeModifyEnable = true;             \
		sba.GeneralStateBufferSize = 0xfffff;                      \
		sba.DynamicStateBufferSizeModifyEnable = true;             \
		sba.DynamicStateBufferSize = 1;                            \
		sba.IndirectObjectBufferSizeModifyEnable = true;           \
		sba.IndirectObjectBufferSize = 0xfffff;                    \
		sba.InstructionBuffersizeModifyEnable = true;              \
		sba.InstructionBufferSize = 1;                             \
		sba.BindlessSurfaceStateMOCS = (mocs_val);                 \
	} while (0)

static void
gen9_emit_state_base_address(struct intel_bb *ibb) {
	uint8_t mocs = intel_get_wb_mocs(ibb->fd);

	struct igt_address surf_base =
		igt_address_of_batch(ibb, I915_GEM_DOMAIN_SAMPLER, 0);
	struct igt_address dyn_base =
		igt_address_of_batch(ibb,
				     I915_GEM_DOMAIN_RENDER | I915_GEM_DOMAIN_INSTRUCTION, 0);
	struct igt_address inst_base =
		igt_address_of_batch(ibb, I915_GEM_DOMAIN_INSTRUCTION, 0);

	if (HAS_4TILE(ibb->devid) || intel_gen(ibb->devid) > 12) {
		igt_genxml_emit(ibb, GFX125_STATE_BASE_ADDRESS, sba) {
			IGT_SBA_COMMON(sba, mocs, surf_base, dyn_base, inst_base);
			/* WBP (0) and UC (1) are marked dont_use in the XML for this field. */
			sba.L1CacheControl = GFX125_L1CC_WB;
			sba.BindlessSamplerStateBaseAddressModifyEnable = true;
			sba.BindlessSamplerStateMOCS = mocs;
		}
	} else if (intel_gen(ibb->devid) >= 11) {
		igt_genxml_emit(ibb, GFX11_STATE_BASE_ADDRESS, sba) {
			IGT_SBA_COMMON(sba, mocs, surf_base, dyn_base, inst_base);
			sba.BindlessSamplerStateBaseAddressModifyEnable = true;
			sba.BindlessSamplerStateMOCS = mocs;
		}
	} else {
		igt_genxml_emit(ibb, GFX9_STATE_BASE_ADDRESS, sba) {
			IGT_SBA_COMMON(sba, mocs, surf_base, dyn_base, inst_base);
		}
	}
}

static void
gen7_emit_urb(struct intel_bb *ibb) {
	/* XXX: Min valid values from mesa */
	const int vs_entries = intel_gen(ibb->devid) >= 35 ? 128 : 64;
	const int vs_size = 2;
	const int vs_start = 4;

	igt_genxml_emit(ibb, GFX9_3DSTATE_URB_VS, urb) {
		urb.VSNumberofURBEntries = vs_entries;
		urb.VSURBEntryAllocationSize = vs_size - 1;
		urb.VSURBStartingAddress = vs_start;
	}
	igt_genxml_emit(ibb, GFX9_3DSTATE_URB_GS, urb) {
		urb.GSURBStartingAddress = vs_start;
	}
	igt_genxml_emit(ibb, GFX9_3DSTATE_URB_HS, urb) {
		urb.HSURBStartingAddress = vs_start;
	}
	igt_genxml_emit(ibb, GFX9_3DSTATE_URB_DS, urb) {
		urb.DSURBStartingAddress = vs_start;
	}
}

static void
gen8_emit_cc(struct intel_bb *ibb) {
	igt_genxml_emit(ibb, GFX9_3DSTATE_BLEND_STATE_POINTERS, bsp) {
		bsp.BlendStatePointer = cc.blend_state;
		bsp.BlendStatePointerValid = true;
	}

	igt_genxml_emit(ibb, GFX9_3DSTATE_CC_STATE_POINTERS, ccp) {
		ccp.ColorCalcStatePointer = cc.cc_state;
		ccp.ColorCalcStatePointerValid = true;
	}
}

static void
gen8_emit_multisample(struct intel_bb *ibb) {
	igt_genxml_emit(ibb, GFX9_3DSTATE_MULTISAMPLE, ms) { }

	igt_genxml_emit(ibb, GFX9_3DSTATE_SAMPLE_MASK, sm) {
		sm.SampleMask = 1;
	}
}

static void
gen8_emit_vs(struct intel_bb *ibb) {
	igt_genxml_emit(ibb, GFX9_3DSTATE_CONSTANT_VS, cvs) {
		cvs.MOCS = intel_get_wb_mocs(ibb->fd);
	}

	igt_genxml_emit(ibb, GFX9_3DSTATE_BINDING_TABLE_POINTERS_VS, bt) { }

	igt_genxml_emit(ibb, GFX9_3DSTATE_SAMPLER_STATE_POINTERS_VS, sp) { }

	igt_genxml_emit(ibb, GFX9_3DSTATE_VS, vs) { }
}

static void
gen8_emit_hs(struct intel_bb *ibb) {
	igt_genxml_emit(ibb, GFX9_3DSTATE_CONSTANT_HS, chs) {
		chs.MOCS = intel_get_wb_mocs(ibb->fd);
	}

	if (intel_gen(ibb->devid) >= 20)
		igt_genxml_emit(ibb, GFX20_3DSTATE_HS, hs) { }
	else
		igt_genxml_emit(ibb, GFX9_3DSTATE_HS, hs) { }

	igt_genxml_emit(ibb, GFX9_3DSTATE_BINDING_TABLE_POINTERS_HS, bt) { }

	igt_genxml_emit(ibb, GFX9_3DSTATE_SAMPLER_STATE_POINTERS_HS, sp) { }
}

static void
gen8_emit_gs(struct intel_bb *ibb) {
	igt_genxml_emit(ibb, GFX9_3DSTATE_CONSTANT_GS, cgs) {
		cgs.MOCS = intel_get_wb_mocs(ibb->fd);
	}

	igt_genxml_emit(ibb, GFX9_3DSTATE_GS, gs) { }

	igt_genxml_emit(ibb, GFX9_3DSTATE_BINDING_TABLE_POINTERS_GS, bt) { }

	igt_genxml_emit(ibb, GFX9_3DSTATE_SAMPLER_STATE_POINTERS_GS, sp) { }
}

static void
gen9_emit_ds(struct intel_bb *ibb) {
	igt_genxml_emit(ibb, GFX9_3DSTATE_CONSTANT_DS, cds) {
		cds.MOCS = intel_get_wb_mocs(ibb->fd);
	}

	igt_genxml_emit(ibb, GFX9_3DSTATE_DS, ds) { }

	igt_genxml_emit(ibb, GFX9_3DSTATE_BINDING_TABLE_POINTERS_DS, bt) { }

	igt_genxml_emit(ibb, GFX9_3DSTATE_SAMPLER_STATE_POINTERS_DS, sp) { }
}


static void
gen8_emit_wm_hz_op(struct intel_bb *ibb) {
	if (intel_gen(intel_get_drm_devid(ibb->fd)) >= 20) {
		igt_genxml_emit(ibb, GFX20_3DSTATE_WM_HZ_OP, hz) { }
	} else {
		igt_genxml_emit(ibb, GFX9_3DSTATE_WM_HZ_OP, hz) { }
	}
}

static void
gen8_emit_null_state(struct intel_bb *ibb) {
	gen8_emit_wm_hz_op(ibb);
	gen8_emit_hs(ibb);

	if (intel_gen(ibb->devid) >= 12)
		igt_genxml_emit(ibb, GFX12_3DSTATE_TE, te) { }
	else
		igt_genxml_emit(ibb, GFX9_3DSTATE_TE, te) { }

	gen8_emit_gs(ibb);
	gen9_emit_ds(ibb);
	gen8_emit_vs(ibb);
}

static void
gen7_emit_clip(struct intel_bb *ibb) {
	igt_genxml_emit(ibb, GFX9_3DSTATE_CLIP, clip) {
		/* All fields zero = pass-through */
	}
}

static void
gen8_emit_sf(struct intel_bb *ibb)
{
	igt_genxml_emit(ibb, GFX9_3DSTATE_SBE, sbe) {
		sbe.NumberofSFOutputAttributes = 1;
		sbe.ForceVertexURBEntryReadLength = true;
		sbe.ForceVertexURBEntryReadOffset = true;
		sbe.VertexURBEntryReadLength = 1;
		sbe.VertexURBEntryReadOffset = 1;
		sbe.AttributeActiveComponentFormat[0] = GFX9_ACTIVE_COMPONENT_XYZW;
	}

	igt_genxml_emit(ibb, GFX9_3DSTATE_SBE_SWIZ, swiz) { }

	igt_genxml_emit(ibb, GFX9_3DSTATE_RASTER, raster) {
		raster.FrontWinding = 1; /* CCW */
		raster.CullMode = GFX9_CULLMODE_NONE;
	}

	igt_genxml_emit(ibb, GFX9_3DSTATE_SF, sf) { }
}

static void
gen8_emit_ps(struct intel_bb *ibb, uint32_t kernel, bool fast_clear) {
	const int max_threads = 63;

	igt_genxml_emit(ibb, GFX9_3DSTATE_WM, wm) {
		wm.BarycentricInterpolationMode = GFX9_BIM_PERSPECTIVE_PIXEL;
	}

	igt_genxml_emit(ibb, GFX9_3DSTATE_CONSTANT_PS, cps) {
		cps.MOCS = intel_get_wb_mocs(ibb->fd);
	}

	if (intel_gen(intel_get_drm_devid(ibb->fd)) >= 20) {
		igt_genxml_emit(ibb, GFX20_3DSTATE_PS, ps) {
			ps.KernelStartPointer0 = kernel;
			ps.Kernel0Enable = true;
			ps.BindingTableEntryCount = fast_clear ? 1 : 2;
			ps.SamplerCount = fast_clear ? 0 : 1;
			ps.Kernel0SIMDWidth = GFX20_PS_SIMD16;
			ps.RenderTargetFastClearEnable = fast_clear;
			ps.MaximumNumberofThreadsPerPSD = max_threads - 1;
			ps.DispatchGRFStartRegisterForConstantSetupData0 = 6;
			ps.Kernel0PolyPackingPolicy = GFX20_POLY_PACK16_FIXED;
		}
	} else {
		igt_genxml_emit(ibb, GFX9_3DSTATE_PS, ps) {
			ps.KernelStartPointer0 = kernel;
			ps.BindingTableEntryCount = fast_clear ? 1 : 2;
			ps.SamplerCount = fast_clear ? 0 : 1;
			ps._16PixelDispatchEnable = true;
			ps.RenderTargetFastClearEnable = fast_clear;
			ps.MaximumNumberofThreadsPerPSD = max_threads - 1;
			ps.DispatchGRFStartRegisterForConstantSetupData0 = 6;
		}
	}

	igt_genxml_emit(ibb, GFX9_3DSTATE_PS_BLEND, blend) {
		blend.HasWriteableRT = true;
	}

	if (intel_gen(intel_get_drm_devid(ibb->fd)) >= 20) {
		igt_genxml_emit(ibb, GFX20_3DSTATE_PS_EXTRA, extra) {
			extra.PixelShaderValid = true;
		}
	} else {
		igt_genxml_emit(ibb, GFX9_3DSTATE_PS_EXTRA, extra) {
			extra.PixelShaderValid = true;
			extra.AttributeEnable = true;
		}
	}
}

static void
gen9_emit_depth(struct intel_bb *ibb)
{
	uint8_t mocs = intel_get_wb_mocs(ibb->fd);

	igt_genxml_emit(ibb, GFX9_3DSTATE_WM_DEPTH_STENCIL, wds) { }

	if (intel_gen(ibb->devid) >= 20) {
		igt_genxml_emit(ibb, GFX20_3DSTATE_DEPTH_BUFFER, db) {
			db.MOCS = mocs;
		}
	} else if (HAS_4TILE(ibb->devid)) {
		igt_genxml_emit(ibb, GFX125_3DSTATE_DEPTH_BUFFER, db) {
			db.MOCS = mocs;
		}
	} else {
		igt_genxml_emit(ibb, GFX9_3DSTATE_DEPTH_BUFFER, db) {
			db.MOCS = mocs;
		}
	}

	igt_genxml_emit(ibb, GFX9_3DSTATE_HIER_DEPTH_BUFFER, hdb) {
		hdb.MOCS = mocs;
	}

	igt_genxml_emit(ibb, GFX9_3DSTATE_STENCIL_BUFFER, sb) {
		sb.MOCS = mocs;
	}
}

static void
gen7_emit_clear(struct intel_bb *ibb) {
	if (intel_gen(intel_get_drm_devid(ibb->fd)) >= 20)
		return;

	igt_genxml_emit(ibb, GFX9_3DSTATE_CLEAR_PARAMS, cp) {
		cp.DepthClearValueValid = true;
	}
}

static void
gen6_emit_drawing_rectangle(struct intel_bb *ibb, const struct intel_buf *dst)
{
	if (intel_gen(intel_get_drm_devid(ibb->fd)) >= 20) {
		igt_genxml_emit(ibb, GFX20_3DSTATE_DRAWING_RECTANGLE_FAST, dr) {
			dr.ClippedDrawingRectangleXMax = intel_buf_width(dst) - 1;
			dr.ClippedDrawingRectangleYMax = intel_buf_height(dst) - 1;
		}
	} else {
		igt_genxml_emit(ibb, GFX9_3DSTATE_DRAWING_RECTANGLE, dr) {
			dr.ClippedDrawingRectangleXMax = intel_buf_width(dst) - 1;
			dr.ClippedDrawingRectangleYMax = intel_buf_height(dst) - 1;
		}
	}
}

static void gen8_emit_vf_topology(struct intel_bb *ibb)
{
	igt_genxml_emit(ibb, GFX9_3DSTATE_VF_TOPOLOGY, vft) {
		vft.PrimitiveTopologyType = GFX9_3DPRIM_RECTLIST;
	}
}

/* Vertex elements MUST be defined before this according to spec */
static void gen8_emit_primitive(struct intel_bb *ibb, uint32_t offset)
{
	igt_genxml_emit(ibb, GFX9_3DSTATE_VF, vf) { }

	igt_genxml_emit(ibb, GFX9_3DSTATE_VF_INSTANCING, vfi) { }

	igt_genxml_emit(ibb, GFX9_3DPRIMITIVE, prim) {
		prim.VertexCountPerInstance = 3;
		prim.InstanceCount = 1;
	}
}

#define PIPE_CONTROL_RENDER_TARGET_FLUSH    (1 << 12)
#define PIPE_CONTROL_DATA_CACHE_INVALIDATE  (1 << 5)
#define PIPE_CONTROL_PROTECTEDPATH_DISABLE  (1 << 27)
#define PIPE_CONTROL_PROTECTEDPATH_ENABLE   (1 << 22)
#define PIPE_CONTROL_POST_SYNC_OP           (1 << 14)
#define PIPE_CONTROL_POST_SYNC_OP_STORE_DW_IDX (1 << 21)
#define PS_OP_TAG_START                     0x1234fed0
#define PS_OP_TAG_END                       0x5678cbaf
static void gen12_emit_pxp_state(struct intel_bb *ibb, bool enable,
		 uint32_t pxp_write_op_offset)
{
	uint32_t pipe_ctl_flags;
	uint32_t set_app_id, ps_op_id;

	if (enable) {
		pipe_ctl_flags = PIPE_CONTROL_FLUSH_ENABLE;
		intel_bb_out(ibb, GFX_OP_PIPE_CONTROL(2));
		intel_bb_out(ibb, pipe_ctl_flags);

		set_app_id =  MI_SET_APPID |
			      APPTYPE(intel_bb_pxp_apptype(ibb)) |
			      APPID(intel_bb_pxp_appid(ibb));
		intel_bb_out(ibb, set_app_id);

		pipe_ctl_flags = PIPE_CONTROL_PROTECTEDPATH_ENABLE;
		ps_op_id = PS_OP_TAG_START;
	} else {
		pipe_ctl_flags = PIPE_CONTROL_PROTECTEDPATH_DISABLE;
		ps_op_id = PS_OP_TAG_END;
	}

	pipe_ctl_flags |= (PIPE_CONTROL_CS_STALL |
			   PIPE_CONTROL_RENDER_TARGET_FLUSH |
			   PIPE_CONTROL_DATA_CACHE_INVALIDATE |
			   PIPE_CONTROL_POST_SYNC_OP);
	intel_bb_out(ibb, GFX_OP_PIPE_CONTROL(6));
	intel_bb_out(ibb, pipe_ctl_flags);
	intel_bb_emit_reloc(ibb, ibb->handle, 0, I915_GEM_DOMAIN_COMMAND,
			    (enable ? pxp_write_op_offset : (pxp_write_op_offset+8)),
			    ibb->batch_offset);
	intel_bb_out(ibb, ps_op_id);
	intel_bb_out(ibb, ps_op_id);
}

/* The general rule is if it's named gen6 it is directly copied from
 * gen6_render_copyfunc.
 *
 * This sets up most of the 3d pipeline, and most of that to NULL state. The
 * docs aren't specific about exactly what must be set up NULL, but the general
 * rule is we could be run at any time, and so the most state we set to NULL,
 * the better our odds of success.
 *
 * +---------------+ <---- 4096
 * |       ^       |
 * |       |       |
 * |    various    |
 * |      state    |
 * |       |       |
 * |_______|_______| <---- 2048 + ?
 * |       ^       |
 * |       |       |
 * |   batch       |
 * |    commands   |
 * |       |       |
 * |       |       |
 * +---------------+ <---- 0 + ?
 *
 * The batch commands point to state within tthe batch, so all state offsets should be
 * 0 < offset < 4096. Both commands and state build upwards, and are constructed
 * in that order. This means too many batch commands can delete state if not
 * careful.
 *
 */

#define BATCH_STATE_SPLIT 2048

static
void _gen9_render_op(struct intel_bb *ibb,
		     struct intel_buf *src,
		     unsigned int src_x, unsigned int src_y,
		     unsigned int width, unsigned int height,
		     struct intel_buf *dst,
		     unsigned int dst_x, unsigned int dst_y,
		     struct intel_buf *aux_pgtable_buf,
		     const float clear_color[4],
		     const uint32_t ps_kernel[][4],
		     uint32_t ps_kernel_size)
{
	uint32_t ps_sampler_state, ps_kernel_off, ps_binding_table;
	uint32_t scissor_state;
	uint32_t vertex_buffer;
	uint32_t aux_pgtable_state;
	bool fast_clear = !src;
	uint32_t pxp_scratch_offset;

	if (!fast_clear)
		igt_assert(src->bpp == dst->bpp);

	intel_bb_flush_render(ibb);

	intel_bb_add_intel_buf(ibb, dst, true);

	if (!fast_clear)
		intel_bb_add_intel_buf(ibb, src, false);

	intel_bb_ptr_set(ibb, BATCH_STATE_SPLIT);

	ps_binding_table  = gen8_bind_surfaces(ibb, src, dst);
	ps_sampler_state  = gen8_create_sampler(ibb);
	ps_kernel_off = gen8_fill_ps(ibb, ps_kernel, ps_kernel_size);
	vertex_buffer = gen7_fill_vertex_buffer_data(ibb, src, src_x, src_y,
						     dst, dst_x, dst_y,
						     width, height);
	cc.cc_state = gen6_create_cc_state(ibb);
	cc.blend_state = gen8_create_blend_state(ibb);
	viewport.cc_state = gen6_create_cc_viewport(ibb);
	viewport.sf_clip_state = gen7_create_sf_clip_viewport(ibb);
	scissor_state = gen6_create_scissor_rect(ibb);
	aux_pgtable_state = gen12_create_aux_pgtable_state(ibb, aux_pgtable_buf);

	/* TODO: there is other state which isn't setup */
	pxp_scratch_offset = intel_bb_offset(ibb);
	intel_bb_ptr_set(ibb, 0);

	if (intel_bb_pxp_enabled(ibb))
		gen12_emit_pxp_state(ibb, true, pxp_scratch_offset);

	/* Start emitting the commands. The order roughly follows the mesa blorp
	 * order */
	igt_genxml_emit(ibb, GFX9_PIPELINE_SELECT, ps) {
		ps.PipelineSelection = GFX9_3D;
		/* MaskBits 15:8 is a write-enable mask for bits 5:4 (Force Media
		 * Awake and Media Sampler DOP Clock Gate Enable).  Value 0x3
		 * enables writes to both bits so PipelineSelection takes effect. */
		ps.MaskBits = 3;
	}

	gen12_emit_aux_pgtable_state(ibb, aux_pgtable_state, true);

	if (fast_clear || dst->cc.disable) {
		for (int i = 0; i < 4; i++) {
			intel_bb_out(ibb, MI_STORE_DWORD_IMM_GEN4);
			intel_bb_emit_reloc(ibb, dst->handle,
					    I915_GEM_DOMAIN_RENDER, I915_GEM_DOMAIN_RENDER,
                                            dst->cc.offset + i*sizeof(float),
					    dst->addr.offset);
			if (fast_clear) {
				intel_bb_out(ibb, *(uint32_t*)&clear_color[i]);
			} else {
				/*
				 * Emit NaNs so it'll never match and thus prevent TGL/DG1
				 * from doing "Fast clear optimization (FCV)" tricks.
				 */
				intel_bb_out(ibb, 0xffffffff);
			}
		}
       }


	gen8_emit_sip(ibb);

	gen7_emit_push_constants(ibb);

	gen9_emit_state_base_address(ibb);

	if (HAS_4TILE(ibb->devid) || intel_gen(ibb->devid) > 12) {
		igt_genxml_emit(ibb, GFX9_3DSTATE_BINDING_TABLE_POOL_ALLOC, btpa) {
			btpa.MOCS = intel_get_wb_mocs(ibb->fd);
			btpa.BindingTablePoolBaseAddress =
				igt_address_of_batch(ibb,
					I915_GEM_DOMAIN_RENDER | I915_GEM_DOMAIN_INSTRUCTION, 0);
			btpa.BindingTablePoolBufferSize = 1;
		}
	}

	igt_genxml_emit(ibb, GFX9_3DSTATE_VIEWPORT_STATE_POINTERS_CC, vp) {
		vp.CCViewportPointer = viewport.cc_state;
	}
	igt_genxml_emit(ibb, GFX9_3DSTATE_VIEWPORT_STATE_POINTERS_SF_CLIP, vp) {
		vp.SFClipViewportPointer = viewport.sf_clip_state;
	}

	gen7_emit_urb(ibb);

	gen8_emit_cc(ibb);

	gen8_emit_multisample(ibb);

	gen8_emit_null_state(ibb);

	igt_genxml_emit(ibb, GFX9_3DSTATE_STREAMOUT, so) { }

	gen7_emit_clip(ibb);

	gen8_emit_sf(ibb);

	gen8_emit_ps(ibb, ps_kernel_off, fast_clear);

	igt_genxml_emit(ibb, GFX9_3DSTATE_BINDING_TABLE_POINTERS_PS, bt) {
		bt.PointertoPSBindingTable = ps_binding_table;
	}

	igt_genxml_emit(ibb, GFX9_3DSTATE_SAMPLER_STATE_POINTERS_PS, sp) {
		sp.PointertoPSSamplerState = ps_sampler_state;
	}

	igt_genxml_emit(ibb, GFX9_3DSTATE_SCISSOR_STATE_POINTERS, ssp) {
		ssp.ScissorRectPointer = scissor_state;
	}

	gen9_emit_depth(ibb);

	gen7_emit_clear(ibb);

	gen6_emit_drawing_rectangle(ibb, dst);

	gen7_emit_vertex_buffer(ibb, vertex_buffer);
	gen6_emit_vertex_elements(ibb);

	gen8_emit_vf_topology(ibb);
	gen8_emit_primitive(ibb, vertex_buffer);

	if (intel_bb_pxp_enabled(ibb))
		gen12_emit_pxp_state(ibb, false, pxp_scratch_offset);

	intel_bb_emit_bbe(ibb);
	intel_bb_exec(ibb, intel_bb_offset(ibb),
		      I915_EXEC_RENDER | I915_EXEC_NO_RELOC, false);
	dump_batch(ibb);
	intel_bb_reset(ibb, false);
}

void gen9_render_copyfunc(struct intel_bb *ibb,
			  struct intel_buf *src,
			  unsigned int src_x, unsigned int src_y,
			  unsigned int width, unsigned int height,
			  struct intel_buf *dst,
			  unsigned int dst_x, unsigned int dst_y)

{
	_gen9_render_op(ibb, src, src_x, src_y,
		        width, height, dst, dst_x, dst_y, NULL, NULL,
		        ps_kernel_gen9, sizeof(ps_kernel_gen9));
}

void gen11_render_copyfunc(struct intel_bb *ibb,
			   struct intel_buf *src,
			   unsigned int src_x, unsigned int src_y,
			   unsigned int width, unsigned int height,
			   struct intel_buf *dst,
			   unsigned int dst_x, unsigned int dst_y)
{
	_gen9_render_op(ibb, src, src_x, src_y,
		        width, height, dst, dst_x, dst_y, NULL, NULL,
		        ps_kernel_gen11, sizeof(ps_kernel_gen11));
}

void gen12_render_copyfunc(struct intel_bb *ibb,
			   struct intel_buf *src,
			   unsigned int src_x, unsigned int src_y,
			   unsigned int width, unsigned int height,
			   struct intel_buf *dst,
			   unsigned int dst_x, unsigned int dst_y)
{
	struct aux_pgtable_info pgtable_info = { };

	gen12_aux_pgtable_init(&pgtable_info, ibb, src, dst);

	_gen9_render_op(ibb, src, src_x, src_y,
		        width, height, dst, dst_x, dst_y,
		        pgtable_info.pgtable_buf,
		        NULL,
		        gen12_render_copy,
		        sizeof(gen12_render_copy));

	gen12_aux_pgtable_cleanup(ibb, &pgtable_info);
}

void gen12p71_render_copyfunc(struct intel_bb *ibb,
			      struct intel_buf *src,
			      unsigned int src_x, unsigned int src_y,
			      unsigned int width, unsigned int height,
			      struct intel_buf *dst,
			      unsigned int dst_x, unsigned int dst_y)
{
	_gen9_render_op(ibb, src, src_x, src_y,
			width, height, dst, dst_x, dst_y,
			NULL,
			NULL,
			gen12p71_render_copy,
			sizeof(gen12p71_render_copy));
}

void xe2_render_copyfunc(struct intel_bb *ibb,
			 struct intel_buf *src, uint32_t src_x, uint32_t src_y,
			 uint32_t width, uint32_t height,
			 struct intel_buf *dst, uint32_t dst_x, uint32_t dst_y)
{
	_gen9_render_op(ibb, src, src_x, src_y,
			width, height, dst, dst_x, dst_y,
			NULL,
			NULL,
			xe2_render_copy,
			sizeof(xe2_render_copy));
}

void xe3p_render_copyfunc(struct intel_bb *ibb,
			  struct intel_buf *src, uint32_t src_x, uint32_t src_y,
			  uint32_t width, uint32_t height,
			  struct intel_buf *dst, uint32_t dst_x, uint32_t dst_y)
{
	_gen9_render_op(ibb, src, src_x, src_y,
			  width, height, dst, dst_x, dst_y,
			  NULL,
			  NULL,
			  xe3p_render_copy,
			  sizeof(xe3p_render_copy));
}

void mtl_render_copyfunc(struct intel_bb *ibb,
			 struct intel_buf *src,
			 unsigned int src_x, unsigned int src_y,
			 unsigned int width, unsigned int height,
			 struct intel_buf *dst,
			 unsigned int dst_x, unsigned int dst_y)
{
	struct aux_pgtable_info pgtable_info = { };

	gen12_aux_pgtable_init(&pgtable_info, ibb, src, dst);

	_gen9_render_op(ibb, src, src_x, src_y,
			width, height, dst, dst_x, dst_y,
			pgtable_info.pgtable_buf,
			NULL,
			gen12p71_render_copy,
			sizeof(gen12p71_render_copy));

	gen12_aux_pgtable_cleanup(ibb, &pgtable_info);
}

void gen12_render_clearfunc(struct intel_bb *ibb,
			    struct intel_buf *dst,
			    unsigned int dst_x, unsigned int dst_y,
			    unsigned int width, unsigned int height,
			    const float clear_color[4])
{
	struct aux_pgtable_info pgtable_info = { };

	gen12_aux_pgtable_init(&pgtable_info, ibb, NULL, dst);

	_gen9_render_op(ibb, NULL, 0, 0,
			width, height, dst, dst_x, dst_y,
			pgtable_info.pgtable_buf,
			clear_color,
			gen12_render_copy,
			sizeof(gen12_render_copy));
	gen12_aux_pgtable_cleanup(ibb, &pgtable_info);
}

void gen12p71_render_clearfunc(struct intel_bb *ibb,
			       struct intel_buf *dst,
			       unsigned int dst_x, unsigned int dst_y,
			       unsigned int width, unsigned int height,
			       const float clear_color[4])
{
	_gen9_render_op(ibb, NULL, 0, 0,
			width, height, dst, dst_x, dst_y,
			NULL,
			clear_color,
			gen12p71_render_copy,
			sizeof(gen12p71_render_copy));
}

void mtl_render_clearfunc(struct intel_bb *ibb,
			  struct intel_buf *dst,
			  unsigned int dst_x, unsigned int dst_y,
			  unsigned int width, unsigned int height,
			  const float clear_color[4])
{
	struct aux_pgtable_info pgtable_info = { };

	gen12_aux_pgtable_init(&pgtable_info, ibb, NULL, dst);

	_gen9_render_op(ibb, NULL, 0, 0,
			width, height, dst, dst_x, dst_y,
			pgtable_info.pgtable_buf,
			clear_color,
			gen12p71_render_copy,
			sizeof(gen12p71_render_copy));
	gen12_aux_pgtable_cleanup(ibb, &pgtable_info);
}
