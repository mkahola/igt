/*
 * Copyright © 2016 Intel Corporation
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
 */

/**
 * TEST: kms plane multiple
 * Category: Display
 * Description: Test atomic mode setting with multiple planes.
 * Driver requirement: i915, xe
 * Mega feature: General Display Features
 */

#include "igt.h"
#include "drmtest.h"
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/**
 * SUBTEST: tiling-none
 * Description: Check that the kernel handles atomic updates of multiple planes
 *              correctly by changing their geometry and making sure the changes
 *              are reflected immediately after each commit.
 *
 * SUBTEST: tiling-%s
 * Description: Check that the kernel handles atomic updates of multiple planes
 *              correctly by changing their geometry and making sure the changes
 *              are reflected immediately after each commit.
 *
 * arg[1]:
 *
 * @4:           4-tiling
 * @x:           x-tiling
 * @y:           y-tiling
 * @yf:          yf-tiling
 *
 * SUBTEST: 2x-tiling-%s
 * Description: Check that the kernel handles atomic updates of multiple planes
 *		simultaneously committed on 2 displays.
 *
 * arg[1]:
 *
 * @none:	 no-tiling
 * @4:           4-tiling
 * @x:           x-tiling
 * @y:           y-tiling
 * @yf:          yf-tiling
 */

IGT_TEST_DESCRIPTION("Test atomic mode setting with multiple planes.");

#define SIZE_PLANE      256
#define SIZE_CURSOR     128
#define LOOP_FOREVER     -1
#define DEFAULT_N_PLANES  3

typedef struct {
	float red;
	float green;
	float blue;
} color_t;

typedef struct {
	int drm_fd;
	igt_display_t display;
	igt_crc_t ref_crc1, ref_crc2;
	igt_pipe_crc_t *pipe_crc1, *pipe_crc2;
	igt_plane_t **plane1, **plane2;
	struct igt_fb *fb1, *fb2;
} data_t;

/* Command line parameters. */
struct {
	int iterations;
	unsigned int seed;
	bool user_seed;
	bool all_planes;
} opt = {
	.iterations = 1,
	.all_planes = false,
};

/*
 * Common code across all tests, acting on data_t
 */
static void test_init(data_t *data, enum pipe pipe, int n_planes)
{
	data->pipe_crc1 = igt_crtc_crc_new(igt_crtc_for_pipe(&data->display, pipe),
					   IGT_PIPE_CRC_SOURCE_AUTO);

	data->plane1 = calloc(n_planes, sizeof(*data->plane1));
	igt_assert_f(data->plane1 != NULL, "Failed to allocate memory for planes\n");

	data->fb1 = calloc(n_planes, sizeof(struct igt_fb));
	igt_assert_f(data->fb1 != NULL, "Failed to allocate memory for FBs\n");
}

static void test_fini(data_t *data, igt_output_t *output, int n_planes)
{
	/* reset the constraint on the pipe */
	igt_output_set_crtc(output, NULL);

	igt_pipe_crc_free(data->pipe_crc1);
	data->pipe_crc1 = NULL;

	free(data->plane1);
	data->plane1 = NULL;

	free(data->fb1);
	data->fb1 = NULL;

	igt_display_reset(&data->display);
}

static void
get_reference_crc(data_t *data, igt_output_t *output, enum pipe pipe, igt_pipe_crc_t *pipe_crc,
	      color_t *color, igt_plane_t **plane, uint64_t modifier, igt_crc_t *ref_crc)
{
	drmModeModeInfo *mode;
	igt_plane_t *primary;
	int ret;

	igt_display_reset(&data->display);
	igt_output_set_crtc(output, igt_crtc_for_pipe(&data->display, pipe));

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	plane[primary->index] = primary;

	mode = igt_output_get_mode(output);

	igt_create_color_fb(data->drm_fd, mode->hdisplay, mode->vdisplay,
			    DRM_FORMAT_XRGB8888,
			    modifier,
			    color->red, color->green, color->blue,
			    &data->fb1[primary->index]);

	igt_plane_set_fb(plane[primary->index], &data->fb1[primary->index]);

	ret = igt_display_try_commit2(&data->display, COMMIT_ATOMIC);
	igt_skip_on(ret != 0);

	igt_pipe_crc_collect_crc(pipe_crc, ref_crc);
}

static void
create_fb_for_mode_position(data_t *data, igt_output_t *output, drmModeModeInfo *mode,
			    color_t *color, int *rect_x, int *rect_y,
			    int *rect_w, int *rect_h, uint64_t modifier,
			    int max_planes, igt_fb_t *fb)
{
	unsigned int fb_id;
	cairo_t *cr;
	igt_plane_t *primary;

	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);

	fb_id = igt_create_fb(data->drm_fd,
			      mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888,
			      modifier,
			      &fb[primary->index]);
	igt_assert(fb_id);

	cr = igt_get_cairo_ctx(data->drm_fd, &fb[primary->index]);
	igt_paint_color(cr, rect_x[0], rect_y[0],
			mode->hdisplay, mode->vdisplay,
			color->red, color->green, color->blue);

	for (int i = 0; i < max_planes; i++) {
		if (data->plane1[i]->type == DRM_PLANE_TYPE_PRIMARY)
			continue;
		igt_paint_color(cr, rect_x[i], rect_y[i],
				rect_w[i], rect_h[i], 0.0, 0.0, 0.0);
	}

	igt_put_cairo_ctx(cr);
}


static void
prepare_planes(data_t *data, enum pipe pipe_id, color_t *color, igt_plane_t **plane,
	       uint64_t modifier, int max_planes, igt_output_t *output, igt_fb_t *fb)
{
	drmModeModeInfo *mode;
	igt_crtc_t *crtc;
	igt_plane_t *primary;
	int *x;
	int *y;
	int *size;
	int i;
	int* suffle;

	igt_output_set_crtc(output,
			    igt_crtc_for_pipe(&data->display, pipe_id));
	primary = igt_output_get_plane_type(output, DRM_PLANE_TYPE_PRIMARY);
	crtc = primary->crtc;

	x = malloc(crtc->n_planes * sizeof(*x));
	igt_assert_f(x, "Failed to allocate %ld bytes for variable x\n",
		     (long int) (crtc->n_planes * sizeof(*x)));
	y = malloc(crtc->n_planes * sizeof(*y));
	igt_assert_f(y, "Failed to allocate %ld bytes for variable y\n",
		     (long int) (crtc->n_planes * sizeof(*y)));
	size = malloc(crtc->n_planes * sizeof(*size));
	igt_assert_f(size, "Failed to allocate %ld bytes for variable size\n",
		     (long int) (crtc->n_planes * sizeof(*size)));
	suffle = malloc(crtc->n_planes * sizeof(*suffle));
	igt_assert_f(suffle, "Failed to allocate %ld bytes for variable size\n",
		     (long int) (crtc->n_planes * sizeof(*suffle)));

	for (i = 0; i < crtc->n_planes; i++)
		suffle[i] = i;

	/*
	 * suffle table for planes. using rand() should keep it
	 * 'randomized in expected way'
	 */
	for (i = 0; i < 256; i++) {
		int n, m;
		int a, b;

		n = rand() % (crtc->n_planes-1);
		m = rand() % (crtc->n_planes-1);

		/*
		 * keep primary plane at its place for test's sake.
		 */
		if(n == primary->index || m == primary->index)
			continue;

		a = suffle[n];
		b = suffle[m];
		suffle[n] = b;
		suffle[m] = a;
	}

	mode = igt_output_get_mode(output);

	/* planes with random positions */
	x[primary->index] = 0;
	y[primary->index] = 0;
	for (i = 0; i < max_planes; i++) {
		/*
		 * Here is made assumption primary plane will have
		 * index zero.
		 */
		uint32_t plane_format;
		uint64_t plane_modifier;

		plane[i] = igt_output_get_plane(output, suffle[i]);

		if (plane[i]->type == DRM_PLANE_TYPE_PRIMARY)
			continue;
		else if (plane[i]->type == DRM_PLANE_TYPE_CURSOR)
			size[i] = SIZE_CURSOR;
		else
			size[i] = SIZE_PLANE;

		x[i] = rand() % (mode->hdisplay - size[i]);
		y[i] = rand() % (mode->vdisplay - size[i]);

		plane_format = plane[i]->type == DRM_PLANE_TYPE_CURSOR ?
						 DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
		plane_modifier = plane[i]->type == DRM_PLANE_TYPE_CURSOR ?
						   DRM_FORMAT_MOD_LINEAR : modifier;

		igt_skip_on(!igt_plane_has_format_mod(plane[i], plane_format,
						      plane_modifier));

		igt_create_color_fb(data->drm_fd,
				    size[i], size[i],
				    plane_format,
				    plane_modifier,
				    color->red, color->green, color->blue,
				    &fb[i]);

		igt_plane_set_position(plane[i], x[i], y[i]);
		igt_plane_set_fb(plane[i], &fb[i]);
	}

	/* primary plane */
	plane[primary->index] = primary;
	create_fb_for_mode_position(data, output, mode, color, x, y,
				    size, size, modifier, max_planes, &fb[primary->index]);
	igt_plane_set_fb(plane[primary->index], &fb[primary->index]);
	free((void*)x);
	free((void*)y);
	free((void*)size);
	free((void*)suffle);
}

/*
 * Multiple plane position test.
 *   - We start by grabbing a reference CRC of a full blue fb being scanned
 *     out on the primary plane
 *   - Then we scannout number of planes:
 *      * the primary plane uses a blue fb with a black rectangle holes
 *      * planes, on top of the primary plane, with a blue fb that is set-up
 *        to cover the black rectangles of the primary plane
 *     The resulting CRC should be identical to the reference CRC
 */

static void
test_plane_position_with_output(data_t *data, enum pipe pipe,
				igt_output_t *output, int n_planes,
				uint64_t modifier)
{
	color_t blue  = { 0.0f, 0.0f, 1.0f };
	igt_crc_t crc;
	igt_plane_t *plane;
	int i;
	int err, c = 0;
	int iterations = max(1, opt.iterations);
	bool loop_forever;
	char info[256];

	igt_info("Using (pipe %s + %s) to run the subtest.\n",
		 kmstest_pipe_name(pipe), igt_output_name(output));

	if (opt.iterations == LOOP_FOREVER) {
		loop_forever = true;
		sprintf(info, "forever");
	} else {
		loop_forever = false;
		sprintf(info, "for %d %s",
			iterations, iterations > 1 ? "iterations" : "iteration");
	}

	test_init(data, pipe, n_planes);

	get_reference_crc(data, output, pipe, data->pipe_crc1, &blue,
			  data->plane1, modifier, &data->ref_crc1);

	/* Find out how many planes are allowed simultaneously */
	do {
		c++;
		prepare_planes(data, pipe, &blue, data->plane1, modifier, c, output, data->fb1);
		err = igt_display_try_commit2(&data->display, COMMIT_ATOMIC);

		for_each_plane_on_pipe(&data->display, pipe, plane)
			igt_plane_set_fb(plane, NULL);

		igt_output_set_crtc(output, NULL);
		igt_display_commit2(&data->display, COMMIT_ATOMIC);

		for (int x = 0; x < c; x++)
			igt_remove_fb(data->drm_fd, &data->fb1[x]);
	} while (!err && c < n_planes);

	if (err)
		c--;

	igt_info("Testing connector %s using pipe %s with %d planes %s with seed %d\n",
		 igt_output_name(output), kmstest_pipe_name(pipe), c,
		 info, opt.seed);

	i = 0;
	while (i < iterations || loop_forever) {

		/* randomize planes and set up the holes */
		prepare_planes(data, pipe, &blue, data->plane1, modifier, c, output, data->fb1);

		igt_display_commit2(&data->display, COMMIT_ATOMIC);
		igt_pipe_crc_start(data->pipe_crc1);

		igt_pipe_crc_get_current(data->display.drm_fd, data->pipe_crc1, &crc);
		igt_assert_crc_equal(&data->ref_crc1, &crc);
		igt_pipe_crc_stop(data->pipe_crc1);

		for_each_plane_on_pipe(&data->display, pipe, plane)
			igt_plane_set_fb(plane, NULL);

		igt_output_set_crtc(output, NULL);
		igt_display_commit2(&data->display, COMMIT_ATOMIC);

		for (int x = 0; x < c; x++)
			igt_remove_fb(data->drm_fd, &data->fb1[x]);

		i++;
	}

	test_fini(data, output, n_planes);
}

static void
test_plane_position(data_t *data, enum pipe pipe, igt_output_t *output, uint64_t modifier)
{
	int n_planes = opt.all_planes ?
			igt_crtc_for_pipe(&data->display, pipe)->n_planes : DEFAULT_N_PLANES;

	if (!opt.user_seed)
		opt.seed = time(NULL);

	srand(opt.seed);

	test_plane_position_with_output(data, pipe, output,
					n_planes, modifier);
}

static void test_init_2_display(data_t *data, enum pipe pipe1, enum pipe pipe2, int n_planes)
{
	data->pipe_crc1 = igt_crtc_crc_new(igt_crtc_for_pipe(&data->display, pipe1),
					   IGT_PIPE_CRC_SOURCE_AUTO);
	data->pipe_crc2 = igt_crtc_crc_new(igt_crtc_for_pipe(&data->display, pipe2),
					   IGT_PIPE_CRC_SOURCE_AUTO);

	data->plane1 = calloc(n_planes, sizeof(*data->plane1));
	igt_assert_f(data->plane1 != NULL, "Failed to allocate memory for planes\n");

	data->plane2 = calloc(n_planes, sizeof(*data->plane2));
	igt_assert_f(data->plane2 != NULL, "Failed to allocate memory for planes\n");

	data->fb1 = calloc(n_planes, sizeof(struct igt_fb));
	igt_assert_f(data->fb1 != NULL, "Failed to allocate memory for FBs\n");

	data->fb2 = calloc(n_planes, sizeof(struct igt_fb));
	igt_assert_f(data->fb2 != NULL, "Failed to allocate memory for FBs\n");
}

static void test_fini_2_display(data_t *data)
{
	igt_pipe_crc_stop(data->pipe_crc1);
	igt_pipe_crc_stop(data->pipe_crc2);

	igt_pipe_crc_free(data->pipe_crc1);
	igt_pipe_crc_free(data->pipe_crc2);
	data->pipe_crc1 = NULL;
	data->pipe_crc2 = NULL;

	free(data->plane1);
	free(data->plane2);
	data->plane1 = NULL;
	data->plane2 = NULL;

	free(data->fb1);
	free(data->fb2);
	data->fb1 = NULL;
	data->fb2 = NULL;

	igt_display_reset(&data->display);
}

static void test_plane_position_2_display(data_t *data, enum pipe pipe1, enum pipe pipe2,
					  igt_output_t *output1, igt_output_t *output2,
					  uint64_t modifier)
{
	color_t blue  = { 0.0f, 0.0f, 1.0f };
	igt_crc_t crc1, crc2;
	int n_planes = opt.all_planes ?
		       igt_crtc_for_pipe(&data->display, 0)->n_planes : DEFAULT_N_PLANES;

	/*
	 * Note: We could use the dynamic way of calculating the maximum planes here
	 * like we've on single display subtest but this consumes a lot of extra time
	 * with the number of dynamic subtests in this case. So keeping n_planes to the
	 * default value. This might need to be tweaked if we see any bw related failures.
	 */

	test_init_2_display(data, pipe1, pipe2, n_planes);
	get_reference_crc(data, output1, pipe1, data->pipe_crc1, &blue,
			  data->plane1, DRM_FORMAT_MOD_LINEAR, &data->ref_crc1);
	get_reference_crc(data, output2, pipe2, data->pipe_crc2, &blue,
			  data->plane2, DRM_FORMAT_MOD_LINEAR, &data->ref_crc2);

	prepare_planes(data, pipe1, &blue, data->plane1,
		       modifier, 2, output1, data->fb1);
	prepare_planes(data, pipe2, &blue, data->plane2,
		       modifier, 2, output2, data->fb2);

	igt_display_commit2(&data->display, COMMIT_ATOMIC);
	igt_pipe_crc_start(data->pipe_crc1);
	igt_pipe_crc_start(data->pipe_crc2);

	igt_pipe_crc_get_current(data->display.drm_fd, data->pipe_crc1, &crc1);
	igt_pipe_crc_get_current(data->display.drm_fd, data->pipe_crc2, &crc2);

	igt_assert_crc_equal(&data->ref_crc1, &crc1);
	igt_assert_crc_equal(&data->ref_crc2, &crc2);
}

#define for_each_connected_output_local(display, output)		\
	for (int j__ = 0;  assert(igt_can_fail()), j__ < (display)->n_outputs; j__++)	\
		for_each_if((((output) = &(display)->outputs[j__]), \
			      igt_output_is_connected((output))))

#define for_each_valid_output_on_pipe_local(display, pipe, output) \
	for_each_connected_output_local((display), (output)) \
		for_each_if(igt_pipe_connector_valid((pipe), (output)))

static void run_2_display_test(data_t *data, uint64_t modifier, const char *name)
{
	igt_crtc_t *crtc2;
	igt_crtc_t *crtc;
	igt_output_t *output1, *output2;
	igt_display_t *display = &data->display;

	igt_skip_on_f(!igt_display_has_format_mod(display, DRM_FORMAT_XRGB8888, modifier),
		      "%s modifier is not supported\n", name);

	igt_display_reset(display);

	for_each_crtc(display, crtc) {
		for_each_valid_output_on_pipe(display, crtc->pipe, output1) {
			for_each_crtc(display, crtc2) {
				if (crtc->pipe == crtc2->pipe)
					continue;

				for_each_valid_output_on_pipe_local(display,
								    crtc2->pipe,
								    output2) {
					if (output1 == output2)
						continue;

					igt_display_reset(display);

					igt_output_set_crtc(output1,
							    crtc);
					igt_output_set_crtc(output2,
							    crtc2);

					if (!intel_pipe_output_combo_valid(display))
						continue;

					igt_dynamic_f("pipe-%s-%s-pipe-%s-%s",
						       igt_crtc_name(crtc),
						       output1->name,
						       igt_crtc_name(crtc2),
						       output2->name)
						test_plane_position_2_display(data,
									      crtc->pipe,
									      crtc2->pipe,
									      output1, output2,
									      modifier);

					test_fini_2_display(data);
				}
			}
		}
	}
}

static void run_test(data_t *data, uint64_t modifier, const char *name)
{
	igt_crtc_t *crtc;
	igt_output_t *output;
	igt_display_t *display = &data->display;

	igt_skip_on_f(!igt_display_has_format_mod(display, DRM_FORMAT_XRGB8888, modifier),
		      "%s modifier is not supported\n", name);

	for_each_crtc_with_valid_output(display, crtc, output) {
		igt_display_reset(display);

		igt_output_set_crtc(output,
				    crtc);
		if (!intel_pipe_output_combo_valid(display))
			continue;

		igt_dynamic_f("pipe-%s-%s", igt_crtc_name(crtc), output->name)
			test_plane_position(data, crtc->pipe, output,
					    modifier);
	}
}

static const struct {
	const char *name;
	uint64_t modifier;
} subtests[] = {
	{ .name = "tiling-none",
	  .modifier = DRM_FORMAT_MOD_LINEAR,
	},
	{ .name = "tiling-x",
	  .modifier = I915_FORMAT_MOD_X_TILED,
	},
	{ .name = "tiling-y",
	  .modifier = I915_FORMAT_MOD_Y_TILED,
	},
	{ .name = "tiling-yf",
	  .modifier = I915_FORMAT_MOD_Yf_TILED,
	},
	{ .name = "tiling-4",
	  .modifier = I915_FORMAT_MOD_4_TILED,
	},
};

static data_t data;

static int opt_handler(int option, int option_index, void *input)
{
	switch (option) {
	case 'a':
		opt.all_planes = true;
		break;
	case 'i':
		opt.iterations = strtol(optarg, NULL, 0);

		if (opt.iterations < LOOP_FOREVER || opt.iterations == 0) {
			igt_info("incorrect number of iterations: %d\n", opt.iterations);
			return IGT_OPT_HANDLER_ERROR;
		}

		break;
	case 's':
		opt.user_seed = true;
		opt.seed = strtoul(optarg, NULL, 0);
		break;
	default:
		return IGT_OPT_HANDLER_ERROR;
	}

	return IGT_OPT_HANDLER_SUCCESS;
}

const char *help_str =
	"  --iterations Number of iterations for test coverage. -1 loop forever, default 64 iterations\n"
	"  --seed       Seed for random number generator\n"
	"  --all-planes Test with all available planes";

struct option long_options[] = {
	{ "iterations", required_argument, NULL, 'i'},
	{ "seed",    required_argument, NULL, 's'},
	{ "all-planes", no_argument, NULL, 'a'},
	{ 0, 0, 0, 0 }
};

int igt_main_args("", long_options, help_str, opt_handler, NULL)
{
	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_ANY);
		kmstest_set_vt_graphics_mode();
		igt_require_pipe_crc(data.drm_fd);
		igt_display_require(&data.display, data.drm_fd);
		igt_require(data.display.is_atomic);
		igt_display_require_output(&data.display);
	}

	for (int i = 0; i < ARRAY_SIZE(subtests); i++) {
		igt_describe("Check that the kernel handles atomic updates of "
			     "multiple planes correctly by changing their "
			     "geometry and making sure the changes are "
			     "reflected immediately after each commit.");

		igt_subtest_with_dynamic(subtests[i].name)
			run_test(&data, subtests[i].modifier, subtests[i].name);
	}

	for (int i = 0; i < ARRAY_SIZE(subtests); i++) {
		igt_subtest_with_dynamic_f("2x-%s", subtests[i].name) {
			int valid_outputs = 0;
			igt_output_t *output;

			for_each_connected_output(&data.display, output)
				valid_outputs++;

			igt_require(valid_outputs > 1);

			run_2_display_test(&data, subtests[i].modifier, subtests[i].name);
		}
	}

	igt_fixture() {
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
	}
}
