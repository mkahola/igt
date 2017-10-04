/*
 * Copyright © 2013 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 * 	Daniel Vetter <daniel.vetter@ffwll.ch>
 * 	Damien Lespiau <damien.lespiau@intel.com>
 */

#ifndef __IGT_KMS_H__
#define __IGT_KMS_H__

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <assert.h>

#include <xf86drmMode.h>

#include "igt_fb.h"
#include "ioctl_wrappers.h"
#include "igt_debugfs.h"

/* Low-level helpers with kmstest_ prefix */

/**
 * pipe:
 * @PIPE_NONE: Invalid pipe, used for disconnecting a output from a pipe.
 * @PIPE_ANY: Deprecated alias for @PIPE_NONE.
 * @PIPE_A: First crtc.
 * @PIPE_B: Second crtc.
 * @PIPE_C: Third crtc.
 * ... and so on.
 * @IGT_MAX_PIPES: Max number of pipes allowed.
 */
enum pipe {
        PIPE_NONE = -1,
        PIPE_ANY = PIPE_NONE,
        PIPE_A = 0,
        PIPE_B,
        PIPE_C,
        PIPE_D,
        PIPE_E,
        PIPE_F,
        IGT_MAX_PIPES
};
const char *kmstest_pipe_name(enum pipe pipe);
int kmstest_pipe_to_index(char pipe);
const char *kmstest_plane_type_name(int plane_type);

enum port {
        PORT_A = 0,
        PORT_B,
        PORT_C,
        PORT_D,
        PORT_E,
        I915_MAX_PORTS
};

/**
 * kmstest_port_name:
 * @port: display plane
 *
 * Returns: String representing @port, e.g. "A".
 */
#define kmstest_port_name(port) ((port) + 'A')

const char *kmstest_encoder_type_str(int type);
const char *kmstest_connector_status_str(int status);
const char *kmstest_connector_type_str(int type);

void kmstest_dump_mode(drmModeModeInfo *mode);

int kmstest_get_pipe_from_crtc_id(int fd, int crtc_id);
void kmstest_set_vt_graphics_mode(void);
void kmstest_restore_vt_mode(void);

enum igt_atomic_crtc_properties {
       IGT_CRTC_BACKGROUND = 0,
       IGT_CRTC_CTM,
       IGT_CRTC_DEGAMMA_LUT,
       IGT_CRTC_GAMMA_LUT,
       IGT_CRTC_MODE_ID,
       IGT_CRTC_ACTIVE,
       IGT_CRTC_OUT_FENCE_PTR,
       IGT_NUM_CRTC_PROPS
};

/**
 * igt_crtc_prop_names
 *
 * igt_crtc_prop_names contains a list of crtc property names,
 * as indexed by the igt_atomic_crtc_properties enum.
 */
extern const char *igt_crtc_prop_names[];

enum igt_atomic_connector_properties {
       IGT_CONNECTOR_SCALING_MODE = 0,
       IGT_CONNECTOR_CRTC_ID,
       IGT_CONNECTOR_DPMS,
       IGT_NUM_CONNECTOR_PROPS
};

/**
 * igt_connector_prop_names
 *
 * igt_connector_prop_names contains a list of crtc property names,
 * as indexed by the igt_atomic_connector_properties enum.
 */
extern const char *igt_connector_prop_names[];

struct kmstest_connector_config {
	drmModeCrtc *crtc;
	drmModeConnector *connector;
	drmModeEncoder *encoder;
	drmModeModeInfo default_mode;
	uint64_t connector_scaling_mode;
	bool connector_scaling_mode_changed;
	bool pipe_changed;
	uint32_t atomic_props_connector[IGT_NUM_CONNECTOR_PROPS];
	int pipe;
	unsigned valid_crtc_idx_mask;
};

struct kmstest_plane {
	int id;
	int index;
	int type;
	int pos_x;
	int pos_y;
	int width;
	int height;
};

struct kmstest_crtc {
	int id;
	int pipe;
	bool active;
	int width;
	int height;
	int n_planes;
	struct kmstest_plane *planes;
};

/**
 * kmstest_force_connector_state:
 * @FORCE_CONNECTOR_UNSPECIFIED: Unspecified
 * @FORCE_CONNECTOR_ON: On
 * @FORCE_CONNECTOR_DIGITAL: Digital
 * @FORCE_CONNECTOR_OFF: Off
 */
enum kmstest_force_connector_state {
	FORCE_CONNECTOR_UNSPECIFIED,
	FORCE_CONNECTOR_ON,
	FORCE_CONNECTOR_DIGITAL,
	FORCE_CONNECTOR_OFF
};

/**
 * kmstest_broadcast_rgb_mode:
 * @BROADCAST_RGB_AUTO: Choose the color range to use automatically
 * @BROADCAST_RGB_FULL: Force the connector to use full color range
 * @BROADCAST_RGB_16_235: Force the connector to use a limited 16:235 color
 * range
 */
enum kmstest_broadcast_rgb_mode {
	BROADCAST_RGB_AUTO = 0,
	BROADCAST_RGB_FULL,
	BROADCAST_RGB_16_235
};


bool kmstest_force_connector(int fd, drmModeConnector *connector,
			     enum kmstest_force_connector_state state);
void kmstest_edid_add_3d(const unsigned char *edid, size_t length, unsigned char *new_edid_ptr[], size_t *new_length);
void kmstest_edid_add_4k(const unsigned char *edid, size_t length, unsigned char *new_edid_ptr[], size_t *new_length);
void kmstest_edid_add_audio(const unsigned char *edid, size_t length, unsigned char *new_edid_ptr[], size_t *new_length);
void kmstest_force_edid(int drm_fd, drmModeConnector *connector,
			const unsigned char *edid, size_t length);

bool kmstest_get_connector_default_mode(int drm_fd, drmModeConnector *connector,
					drmModeModeInfo *mode);
bool kmstest_get_connector_config(int drm_fd, uint32_t connector_id,
				  unsigned long crtc_idx_mask,
				  struct kmstest_connector_config *config);
bool kmstest_probe_connector_config(int drm_fd, uint32_t connector_id,
				    unsigned long crtc_idx_mask,
				    struct kmstest_connector_config *config);
void kmstest_free_connector_config(struct kmstest_connector_config *config);

void kmstest_set_connector_dpms(int fd, drmModeConnector *connector, int mode);
bool kmstest_set_connector_broadcast_rgb(int fd, drmModeConnector *connector,
					 enum kmstest_broadcast_rgb_mode mode);
bool kmstest_get_property(int drm_fd, uint32_t object_id, uint32_t object_type,
			  const char *name, uint32_t *prop_id, uint64_t *value,
			  drmModePropertyPtr *prop);
void kmstest_unset_all_crtcs(int drm_fd, drmModeResPtr resources);
int kmstest_get_crtc_idx(drmModeRes *res, uint32_t crtc_id);
uint32_t kmstest_find_crtc_for_connector(int fd, drmModeRes *res,
					 drmModeConnector *connector,
					 uint32_t crtc_blacklist_idx_mask);

uint32_t kmstest_dumb_create(int fd, int width, int height, int bpp,
			     unsigned *stride, unsigned *size);

void *kmstest_dumb_map_buffer(int fd, uint32_t handle, uint64_t size,
			      unsigned prot);
unsigned int kmstest_get_vblank(int fd, int pipe, unsigned int flags);
void kmstest_get_crtc(int fd, enum pipe pipe, struct kmstest_crtc *crtc);
void igt_assert_plane_visible(int fd, enum pipe pipe, bool visibility);

/*
 * A small modeset API
 */

/* High-level kms api with igt_ prefix */

/**
 * igt_commit_style:
 * @COMMIT_LEGACY: Changes will be committed using the legacy API.
 * @COMMIT_UNIVERSAL: Changes will be committed with the universal plane API, no modesets are allowed.
 * @COMMIT_ATOMIC: Changes will be committed using the atomic API.
 */
enum igt_commit_style {
	COMMIT_LEGACY = 0,
	COMMIT_UNIVERSAL,
	COMMIT_ATOMIC,
};

enum igt_atomic_plane_properties {
       IGT_PLANE_SRC_X = 0,
       IGT_PLANE_SRC_Y,
       IGT_PLANE_SRC_W,
       IGT_PLANE_SRC_H,

       IGT_PLANE_CRTC_X,
       IGT_PLANE_CRTC_Y,
       IGT_PLANE_CRTC_W,
       IGT_PLANE_CRTC_H,

       IGT_PLANE_FB_ID,
       IGT_PLANE_CRTC_ID,
       IGT_PLANE_IN_FENCE_FD,
       IGT_PLANE_TYPE,
       IGT_PLANE_ROTATION,
       IGT_NUM_PLANE_PROPS
};

/**
 * igt_plane_prop_names
 *
 * igt_plane_prop_names contains a list of crtc property names,
 * as indexed by the igt_atomic_plane_properties enum.
 */
extern const char *igt_plane_prop_names[];

typedef struct igt_display igt_display_t;
typedef struct igt_pipe igt_pipe_t;
typedef uint32_t igt_fixed_t;			/* 16.16 fixed point */

typedef enum {
	/* this maps to the kernel API */
	IGT_ROTATION_0   = 1 << 0,
	IGT_ROTATION_90  = 1 << 1,
	IGT_ROTATION_180 = 1 << 2,
	IGT_ROTATION_270 = 1 << 3,
} igt_rotation_t;

typedef struct {
	/*< private >*/
	igt_pipe_t *pipe;
	int index;
	/* capabilities */
	int type;
	/* state tracking */
	unsigned int fb_changed       : 1;
	unsigned int position_changed : 1;
	unsigned int rotation_changed : 1;
	unsigned int size_changed     : 1;
	/*
	 * drm_plane can be NULL for primary and cursor planes (when not
	 * using the atomic modeset API)
	 */
	drmModePlane *drm_plane;
	struct igt_fb *fb;

	uint32_t rotation_property;

	/* position within pipe_src_w x pipe_src_h */
	int crtc_x, crtc_y;
	/* size within pipe_src_w x pipe_src_h */
	int crtc_w, crtc_h;

	/* position within the framebuffer */
	uint32_t src_x;
	uint32_t src_y;
	/* size within the framebuffer*/
	uint32_t src_w;
	uint32_t src_h;

	igt_rotation_t rotation;

	/* in fence fd */
	int fence_fd;
	uint32_t atomic_props_plane[IGT_NUM_PLANE_PROPS];
} igt_plane_t;

struct igt_pipe {
	igt_display_t *display;
	enum pipe pipe;

	int n_planes;
	int plane_cursor;
	int plane_primary;
	igt_plane_t *planes;

	uint32_t atomic_props_crtc[IGT_NUM_CRTC_PROPS];

	uint64_t background; /* Background color MSB BGR 16bpc LSB */
	uint32_t background_changed : 1;
	uint32_t background_property;

	uint64_t degamma_blob;
	uint32_t degamma_property;
	uint64_t ctm_blob;
	uint32_t ctm_property;
	uint64_t gamma_blob;
	uint32_t gamma_property;
	uint32_t color_mgmt_changed : 1;

	uint32_t crtc_id;

	uint64_t mode_blob;
	bool mode_changed;

	int32_t out_fence_fd;
	bool out_fence_requested;
};

typedef struct {
	/*< private >*/
	igt_display_t *display;
	uint32_t id;					/* KMS id */
	struct kmstest_connector_config config;
	char *name;
	bool force_reprobe;
	enum pipe pending_pipe;
	bool use_override_mode;
	drmModeModeInfo override_mode;
} igt_output_t;

struct igt_display {
	int drm_fd;
	int log_shift;
	int n_pipes;
	int n_outputs;
	igt_output_t *outputs;
	igt_pipe_t *pipes;
	bool has_cursor_plane;
	bool is_atomic;
};

typedef struct {
	igt_display_t display;
	igt_pipe_crc_t *pipe_crc;
	igt_plane_t **plane;
	struct igt_fb *fb;
} kmstest_data_t;

void igt_display_init(igt_display_t *display, int drm_fd);
void igt_display_fini(igt_display_t *display);
int  igt_display_commit2(igt_display_t *display, enum igt_commit_style s);
int  igt_display_commit(igt_display_t *display);
int  igt_display_try_commit_atomic(igt_display_t *display, uint32_t flags, void *user_data);
void igt_display_commit_atomic(igt_display_t *display, uint32_t flags, void *user_data);
int  igt_display_try_commit2(igt_display_t *display, enum igt_commit_style s);
int  igt_display_get_n_pipes(igt_display_t *display);
void igt_display_require_output(igt_display_t *display);
void igt_display_require_output_on_pipe(igt_display_t *display, enum pipe pipe);

const char *igt_output_name(igt_output_t *output);
drmModeModeInfo *igt_output_get_mode(igt_output_t *output);
void igt_output_override_mode(igt_output_t *output, drmModeModeInfo *mode);
void igt_output_set_pipe(igt_output_t *output, enum pipe pipe);
void igt_output_set_scaling_mode(igt_output_t *output, uint64_t scaling_mode);
igt_plane_t *igt_output_get_plane(igt_output_t *output, int plane_idx);
igt_plane_t *igt_output_get_plane_type(igt_output_t *output, int plane_type);
igt_output_t *igt_output_from_connector(igt_display_t *display,
    drmModeConnector *connector);
igt_plane_t *igt_pipe_get_plane_type(igt_pipe_t *pipe, int plane_type);
bool igt_pipe_get_property(igt_pipe_t *pipe, const char *name,
			   uint32_t *prop_id, uint64_t *value,
			   drmModePropertyPtr *prop);

static inline bool igt_plane_supports_rotation(igt_plane_t *plane)
{
	return plane->rotation_property != 0;
}
void igt_pipe_request_out_fence(igt_pipe_t *pipe);
void igt_pipe_set_degamma_lut(igt_pipe_t *pipe, void *ptr, size_t length);
void igt_pipe_set_ctm_matrix(igt_pipe_t *pipe, void *ptr, size_t length);
void igt_pipe_set_gamma_lut(igt_pipe_t *pipe, void *ptr, size_t length);

void igt_plane_set_fb(igt_plane_t *plane, struct igt_fb *fb);
void igt_plane_set_fence_fd(igt_plane_t *plane, int fence_fd);
void igt_plane_set_position(igt_plane_t *plane, int x, int y);
void igt_plane_set_size(igt_plane_t *plane, int w, int h);
void igt_plane_set_rotation(igt_plane_t *plane, igt_rotation_t rotation);
void igt_crtc_set_background(igt_pipe_t *pipe, uint64_t background);
void igt_fb_set_position(struct igt_fb *fb, igt_plane_t *plane,
	uint32_t x, uint32_t y);
void igt_fb_set_size(struct igt_fb *fb, igt_plane_t *plane,
	uint32_t w, uint32_t h);

void igt_wait_for_vblank(int drm_fd, enum pipe pipe);
void igt_wait_for_vblank_count(int drm_fd, enum pipe pipe, int count);

static inline bool igt_output_is_connected(igt_output_t *output)
{
	/* Something went wrong during probe? */
	if (!output->config.connector)
		return false;

	if (output->config.connector->connection == DRM_MODE_CONNECTED)
		return true;

	return false;
}

/**
 * igt_pipe_connector_valid:
 * @pipe: pipe to check.
 * @output: #igt_output_t to check.
 *
 * Checks whether the given pipe and output can be used together.
 */
#define igt_pipe_connector_valid(pipe, output) \
	(igt_output_is_connected((output)) && \
	       (output->config.valid_crtc_idx_mask & (1 << (pipe))))

#define for_each_if(condition) if (!(condition)) {} else

/**
 * for_each_connected_output:
 * @display: a pointer to an #igt_display_t structure
 * @output: The output to iterate.
 *
 * This for loop iterates over all outputs.
 */
#define for_each_connected_output(display, output)		\
	for (int i__ = 0;  assert(igt_can_fail()), i__ < (display)->n_outputs; i__++)	\
		for_each_if (((output = &(display)->outputs[i__]), \
			      igt_output_is_connected(output)))

/**
 * for_each_pipe:
 * @pipe: The pipe to iterate.
 *
 * This for loop iterates over all pipes supported by IGT libraries.
 *
 * This should be used to enumerate per-pipe subtests since it has no runtime
 * depencies.
 */
#define for_each_pipe_static(pipe) \
	for (pipe = 0; pipe < IGT_MAX_PIPES; pipe++)

/**
 * for_each_pipe:
 * @display: a pointer to an #igt_display_t structure
 * @pipe: The pipe to iterate.
 *
 * This for loop iterates over all pipes.
 *
 * Note that this cannot be used to enumerate per-pipe subtest names since it
 * depends upon runtime probing of the actual kms driver that is being tested.
 * Used #for_each_pipe_static instead.
 */
#define for_each_pipe(display, pipe)					\
	for (pipe = 0; assert(igt_can_fail()), pipe < igt_display_get_n_pipes(display); pipe++)

/**
 * for_each_pipe_with_valid_output:
 * @display: a pointer to an #igt_display_t structure
 * @pipe: The pipe for which this @pipe / @output combination is valid.
 * @output: The output for which this @pipe / @output combination is valid.
 *
 * This for loop is called over all connected outputs. This function
 * will try every combination of @pipe and @output.
 */
#define for_each_pipe_with_valid_output(display, pipe, output) \
	for (int con__ = pipe = 0; \
	     assert(igt_can_fail()), pipe < igt_display_get_n_pipes((display)) && con__ < (display)->n_outputs; \
	     con__ = (con__ + 1 < (display)->n_outputs) ? con__ + 1 : (pipe = pipe + 1, 0)) \
		for_each_if (((output = &(display)->outputs[con__]), \
			     igt_pipe_connector_valid(pipe, output)))

/**
 * for_each_valid_output_on_pipe:
 * @display: a pointer to an #igt_display_t structure
 * @pipe: Pipe to enumerate valid outputs over
 * @output: The enumerated output.
 *
 * This for loop is called over all connected @output that can be used
 * on this @pipe . If there are no valid outputs for this pipe, nothing
 * happens.
 */
#define for_each_valid_output_on_pipe(display, pipe, output) \
	for_each_connected_output(display, output) \
		for_each_if (igt_pipe_connector_valid(pipe, output))

#define for_each_plane_on_pipe(display, pipe, plane)			\
	for (int j__ = 0; assert(igt_can_fail()), (plane) = &(display)->pipes[(pipe)].planes[j__], \
		     j__ < (display)->pipes[(pipe)].n_planes; j__++)

#define IGT_FIXED(i,f)	((i) << 16 | (f))

/**
 * igt_atomic_populate_plane_req:
 * @req: A pointer to drmModeAtomicReq
 * @plane: A pointer igt_plane_t
 * @prop: one of igt_atomic_plane_properties
 * @value: the value to add
 */
#define igt_atomic_populate_plane_req(req, plane, prop, value) \
	igt_assert_lt(0, drmModeAtomicAddProperty(req, plane->drm_plane->plane_id,\
						  plane->atomic_props_plane[prop], value))

/**
 * igt_atomic_populate_crtc_req:
 * @req: A pointer to drmModeAtomicReq
 * @pipe: A pointer igt_pipe_t
 * @prop: one of igt_atomic_crtc_properties
 * @value: the value to add
 */
#define igt_atomic_populate_crtc_req(req, pipe, prop, value) \
	igt_assert_lt(0, drmModeAtomicAddProperty(req, pipe->crtc_id,\
						  pipe->atomic_props_crtc[prop], value))
/**
 * igt_atomic_populate_connector_req:
 * @req: A pointer to drmModeAtomicReq
 * @output: A pointer igt_output_t
 * @prop: one of igt_atomic_connector_properties
 * @value: the value to add
 */
#define igt_atomic_populate_connector_req(req, output, prop, value) \
	igt_assert_lt(0, drmModeAtomicAddProperty(req, output->config.connector->connector_id,\
						  output->config.atomic_props_connector[prop], value))

/*
 * igt_pipe_refresh:
 * @display: a pointer to an #igt_display_t structure
 * @pipe: Pipe to refresh
 * @force: Should be set to true if mode_blob is no longer considered
 * to be valid, for example after doing an atomic commit during fork or closing display fd.
 *
 * Requests the pipe to be part of the state on next update.
 * This is useful when state may have been out of sync after
 * a fork, or we just want to be sure the pipe is included
 * in the next commit.
 */
static inline void
igt_pipe_refresh(igt_display_t *display, enum pipe pipe, bool force)
{
	if (force)
		display->pipes[pipe].mode_blob = 0;

	display->pipes[pipe].mode_changed = true;
}

void igt_enable_connectors(void);
void igt_reset_connectors(void);

uint32_t kmstest_get_vbl_flag(uint32_t pipe_id);
void kmstest_cleanup(kmstest_data_t *data, enum pipe pipe,
		     igt_output_t *output);

#define EDID_LENGTH 128
const unsigned char* igt_kms_get_base_edid(void);
const unsigned char* igt_kms_get_alt_edid(void);

#ifdef HAVE_UDEV
struct udev_monitor *igt_watch_hotplug(void);
bool igt_hotplug_detected(struct udev_monitor *mon,
			  int timeout_secs);
void igt_flush_hotplugs(struct udev_monitor *mon);
void igt_cleanup_hotplug(struct udev_monitor *mon);
#endif

#endif /* __IGT_KMS_H__ */
