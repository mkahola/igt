// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */

/**
 * TEST: kms hdmi audio bw
 * Category: Display
 * Description: Validate HDMI TMDS audio bandwidth constraints by injecting
 *              EDIDs with all sample rates declared and observing which rates
 *              the driver exposes (via ELD) under varying BPC / channel /
 *              hblank configurations.
 * Driver requirement: i915, xe
 * Mega feature: Display Audio
 */

#include "config.h"

#include <math.h>
#include <string.h>

#include "igt.h"
#include "igt_edid.h"
#include "igt_eld.h"
#include "igt_aux.h"

/**
 * SUBTEST: audio-bw-hblank160
 * Description: Validate HDMI audio bandwidth with 160-pixel horizontal
 *              blanking across BPC and channel combinations.
 *
 * SUBTEST: audio-bw-hblank80
 * Description: Validate HDMI audio bandwidth with 80-pixel horizontal
 *              blanking across BPC and channel combinations.
 *
 * SUBTEST: suspend-%s-audio-recovery
 * Description: Validate audio state restoration after %arg[1] with
 *              constrained hblank=80 and 12bpc.
 *
 * arg[1]:
 *
 * @mem: Suspend to memory, respecting the system's mem_sleep default
 * @disk: Suspend to disk (hibernate)
 *
 * SUBTEST: dpms-audio-recovery
 * Description: Validate audio state restoration after a DPMS off/on cycle
 *              with constrained hblank=80 and 12bpc.
 */

IGT_TEST_DESCRIPTION("Validate HDMI TMDS audio bandwidth constraints. "
		      "EDIDs declare all sample rates (32k-192k); the test "
		      "observes which rates survive in the ELD under "
		      "constrained hblank timings.");

typedef struct {
	int drm_fd;
	igt_display_t display;
	igt_output_t *output;
	igt_crtc_t *crtc;
	struct igt_fb fb;
} data_t;

/* All sample rates declared in the EDID SAD */
#define ALL_SAMPLE_RATES (CEA_SAD_SAMPLING_RATE_32KHZ | \
			  CEA_SAD_SAMPLING_RATE_44KHZ | \
			  CEA_SAD_SAMPLING_RATE_48KHZ | \
			  CEA_SAD_SAMPLING_RATE_88KHZ | \
			  CEA_SAD_SAMPLING_RATE_96KHZ | \
			  CEA_SAD_SAMPLING_RATE_176KHZ | \
			  CEA_SAD_SAMPLING_RATE_192KHZ)

struct rate_info {
	unsigned int flag;
	const char *name;
	int freq_hz;
};

static const struct rate_info rate_table[] = {
	{ CEA_SAD_SAMPLING_RATE_32KHZ,  "32k",   32000 },
	{ CEA_SAD_SAMPLING_RATE_44KHZ,  "44.1k", 44100 },
	{ CEA_SAD_SAMPLING_RATE_48KHZ,  "48k",   48000 },
	{ CEA_SAD_SAMPLING_RATE_88KHZ,  "88.2k", 88200 },
	{ CEA_SAD_SAMPLING_RATE_96KHZ,  "96k",   96000 },
	{ CEA_SAD_SAMPLING_RATE_176KHZ, "176.4k", 176400 },
	{ CEA_SAD_SAMPLING_RATE_192KHZ, "192k",  192000 },
};

#define ACR_RATE_MAX		1500
#define TOLERANCE_AUDIOCLK_PPM	1000
#define TOLERANCE_PIXELCLK	0.005
#define HBLANK_OVERHEAD_STD	30
#define HBLANK_OVERHEAD_HDCP14	74
#define DI_PACKET_SIZE		32

static void rates_to_str(unsigned int rates, char *buf, size_t len)
{
	int pos = 0;

	buf[0] = '\0';
	for (int i = 0; i < ARRAY_SIZE(rate_table); i++) {
		if (!(rates & rate_table[i].flag))
			continue;
		if (pos > 0)
			pos += snprintf(buf + pos, len - pos, ",");
		pos += snprintf(buf + pos, len - pos, "%s", rate_table[i].name);
	}
	if (pos == 0)
		snprintf(buf, len, "none");
}

static const int bpc_values[] = { 8, 10, 12 };
static const int channel_values[] = { 2, 8 };

/*
 * 1920x1080@60Hz CVT RB2 — hblank=80 (constrained)
 * Available Packets/Line = FLOOR(((BPC/8)*80 - 74) / 32)
 *   8bpc=0, 10bpc=0, 12bpc=1
 */
static const drmModeModeInfo mode_1080p_hblank80 = {
	.clock = 133320,
	.hdisplay = 1920,
	.hsync_start = 1928,
	.hsync_end = 1960,
	.htotal = 2000,		/* hblank = 80 */
	.vdisplay = 1080,
	.vsync_start = 1097,
	.vsync_end = 1105,
	.vtotal = 1111,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC,
	.type = DRM_MODE_TYPE_DRIVER,
	.name = "1920x1080",
};

/*
 * 1920x1080@60Hz with hblank=160 (relaxed baseline)
 * Enough hblank for audio at any BPC.
 */
static const drmModeModeInfo mode_1080p_hblank160 = {
	.clock = 148500,
	.hdisplay = 1920,
	.hsync_start = 1968,
	.hsync_end = 2000,
	.htotal = 2080,		/* hblank = 160 */
	.vdisplay = 1080,
	.vsync_start = 1097,
	.vsync_end = 1105,
	.vtotal = 1111,
	.vrefresh = 60,
	.flags = DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_NVSYNC,
	.type = DRM_MODE_TYPE_DRIVER,
	.name = "1920x1080",
};

static igt_output_t *find_hdmi_output(igt_display_t *display)
{
	igt_output_t *output;

	for_each_connected_output(display, output) {
		drmModeConnector *c = output->config.connector;

		if (c->connector_type == DRM_MODE_CONNECTOR_HDMIA ||
		    c->connector_type == DRM_MODE_CONNECTOR_HDMIB)
			return output;
	}

	return NULL;
}

static int hblank_of(const drmModeModeInfo *mode)
{
	return mode->htotal - mode->hdisplay;
}

/*
 * Driver always reserves HDCP 1.4 rekey overhead (74 clocks) even when
 * HDCP is not active: 30 (standard) + 44 (HDCP 1.4 rekey quiet period).
 * FIXME: once driver exposes HDCP state, use 30 for no HDCP, 74 for HDCP 1.4.
 */
static int avail_pkts_per_line(int bpc, int hblank)
{
	int overhead = HBLANK_OVERHEAD_HDCP14;
	int tb_blank = (bpc * hblank + 7) / 8; /* CEIL(hblank * bpc/8) */
	int avail = (tb_blank - overhead) / DI_PACKET_SIZE;

	return avail > 0 ? avail : 0;
}

/* Packets required per line for a given audio rate and channel layout */
static int required_pkts_per_line(const drmModeModeInfo *mode, int freq_hz,
				 int channels)
{
	double ap = (channels <= 2) ? 0.25 : 1.0;
	double f_pixel_max = mode->clock * 1000.0 * (1 + TOLERANCE_PIXELCLK);
	double t_line = mode->htotal / f_pixel_max;
	double r_ap = ((freq_hz * ap) + (2 * ACR_RATE_MAX)) *
		      (1 + TOLERANCE_AUDIOCLK_PPM / 1e6);

	double avg_pkts = r_ap * t_line;

	return (int)avg_pkts + (avg_pkts > (int)avg_pkts ? 1 : 0);
}

/*
 * Build a CEA EDID declaring all 7 sample rates in the SAD.
 * Deep-color flags in HDMI VSDB match the requested bpc.
 */
static const struct edid *
build_edid(int bpc, int audio_channels)
{
	static unsigned char raw_edid[2 * EDID_BLOCK_SIZE];
	struct edid *edid;
	struct edid_ext *ext;
	struct edid_cea *cea;
	struct edid_cea_data_block *block;
	struct cea_sad sad;
	struct hdmi_vsdb vsdb;
	struct cea_speaker_alloc speakers;
	size_t offset = 0;

	memset(raw_edid, 0, sizeof(raw_edid));
	edid = (struct edid *)raw_edid;
	memcpy(edid, igt_kms_get_base_edid(), sizeof(struct edid));
	edid->extensions_len = 1;

	ext = &edid->extensions[0];
	cea = &ext->data.cea;

	if (audio_channels > 0) {
		cea_sad_init_pcm(&sad,
				 audio_channels,
				 ALL_SAMPLE_RATES,
				 CEA_SAD_SAMPLE_SIZE_16 |
				 CEA_SAD_SAMPLE_SIZE_24);
		block = (struct edid_cea_data_block *)&cea->data[offset];
		offset += edid_cea_data_block_set_sad(block, &sad, 1);
	}

	/* Use source physical address 1.0.0.0, as in the standard HDMI VSDB. */
	memset(&vsdb, 0, sizeof(vsdb));
	vsdb.src_phy_addr[0] = 0x10;
	vsdb.src_phy_addr[1] = 0x00;
	/* Advertise ACP/ISRC packet support and the HDMI 1.4 TMDS limit. */
	vsdb.flags1 = HDMI_VSDB_SUPPORTS_AI;
	vsdb.max_tdms_clock = 340000000 / (5 * 1000000); /* 340 MHz / 5 MHz */

	switch (bpc) {
	case 12:
		vsdb.flags1 |= HDMI_VSDB_DC_36BIT;
		/* fall through */
	case 10:
		vsdb.flags1 |= HDMI_VSDB_DC_30BIT;
		/* fall through */
	case 8:
		break;
	}

	block = (struct edid_cea_data_block *)&cea->data[offset];
	offset += edid_cea_data_block_set_hdmi_vsdb(block, &vsdb,
	                                            sizeof(vsdb));

	memset(&speakers, 0, sizeof(speakers));
	speakers.speakers = CEA_SPEAKER_FRONT_LEFT_RIGHT;
	if (audio_channels == 8)
		speakers.speakers |= CEA_SPEAKER_FRONT_CENTER |
					 CEA_SPEAKER_LFE |
					 CEA_SPEAKER_REAR_LEFT_RIGHT |
					 CEA_SPEAKER_REAR_LEFT_RIGHT_CENTER;
	block = (struct edid_cea_data_block *)&cea->data[offset];
	offset += edid_cea_data_block_set_speaker_alloc(block, &speakers);

	edid_ext_set_cea(ext, offset, 0,
			 EDID_CEA_BASIC_AUDIO | EDID_CEA_UNDERSCAN |
			 EDID_CEA_YCBCR444 | EDID_CEA_YCBCR422);
	edid_update_checksum(edid);

	return edid;
}

static void force_edid_and_connector(data_t *data, const struct edid *edid)
{
	kmstest_force_edid(data->drm_fd, data->output->config.connector, edid);
	igt_skip_on_f(!kmstest_force_connector(data->drm_fd,
					       data->output->config.connector,
					       FORCE_CONNECTOR_ON),
		      "Could not force HDMI connector on\n");
}

static void cleanup_connector(data_t *data)
{
	if (data->output->pending_crtc) {
		igt_plane_t *primary;

		primary = igt_output_get_plane_type(data->output,
						    DRM_PLANE_TYPE_PRIMARY);
		igt_plane_set_fb(primary, NULL);
		igt_output_set_crtc(data->output, NULL);
		igt_display_commit2(&data->display, COMMIT_ATOMIC);
	}

	igt_remove_fb(data->drm_fd, &data->fb);

	kmstest_force_connector(data->drm_fd,
				data->output->config.connector,
				FORCE_CONNECTOR_UNSPECIFIED);
	kmstest_force_edid(data->drm_fd,
			   data->output->config.connector, NULL);
}

static int try_modeset(data_t *data, const drmModeModeInfo *mode)
{
	igt_plane_t *primary;
	int ret;

	igt_display_reset(&data->display);

	igt_output_set_crtc(data->output, data->crtc);
	igt_output_override_mode(data->output, mode);

	primary = igt_output_get_plane_type(data->output,
					    DRM_PLANE_TYPE_PRIMARY);

	igt_create_pattern_fb(data->drm_fd,
			      mode->hdisplay, mode->vdisplay,
			      DRM_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR,
			      &data->fb);
	igt_plane_set_fb(primary, &data->fb);

	ret = igt_display_try_commit_atomic(&data->display,
					    DRM_MODE_ATOMIC_ALLOW_MODESET,
					    NULL);
	if (ret) {
		igt_plane_set_fb(primary, NULL);
		igt_output_set_crtc(data->output, NULL);
		igt_remove_fb(data->drm_fd, &data->fb);
	}

	return ret;
}

static bool audio_is_active(void)
{
	if (!eld_is_supported())
		return false;

	return eld_has_igt();
}

static void wait_for_eld(void)
{
	/* ELD creation is asynchronous; return as soon as it is available. */
	igt_wait(eld_has_igt(), 200, 10);
}

static unsigned int get_eld_rates(void)
{
	struct eld_entry eld;

	if (!eld_get_igt(&eld))
		return 0;

	if (eld.sads_len == 0)
		return 0;

	/* build_edid() currently adds exactly one SAD; use its rates. */
	return eld.sads[0].rates;
}

static void log_eld_rates(unsigned int declared, unsigned int eld_rates)
{
	char decl_str[128], eld_str[128], pruned_str[128];
	unsigned int pruned = declared & ~eld_rates;

	rates_to_str(declared, decl_str, sizeof(decl_str));
	rates_to_str(eld_rates, eld_str, sizeof(eld_str));
	rates_to_str(pruned, pruned_str, sizeof(pruned_str));

	igt_info("    SAD declared: %s\n", decl_str);
	igt_info("    ELD reports:  %s\n", eld_str);
	if (pruned)
		igt_info("    Pruned:       %s\n", pruned_str);
}

static void assert_per_rate(const drmModeModeInfo *mode, int channels,
			   int pkts_avail, unsigned int eld_rates)
{
	for (int i = 0; i < ARRAY_SIZE(rate_table); i++) {
		int req = required_pkts_per_line(mode, rate_table[i].freq_hz,
						 channels);
		bool in_eld = eld_rates & rate_table[i].flag;

		igt_assert_f(in_eld == (req <= pkts_avail),
				     "%s: req=%d avail=%d ELD=%s, expected=%s\n",
				     rate_table[i].name, req, pkts_avail,
				     in_eld ? "present" : "missing",
				     req <= pkts_avail ? "present" : "missing");
	}
}

static void log_per_rate_analysis(const drmModeModeInfo *mode,
				  int bpc, int channels,
				  int pkts_avail, unsigned int eld_rates)
{
	const char *layout = (channels <= 2) ? "L0" : "L1";

	igt_info("    %-6s %-3s  pkts: avail=%d\n",
		 "Rate", layout, pkts_avail);

	for (int i = 0; i < ARRAY_SIZE(rate_table); i++) {
		int req = required_pkts_per_line(mode, rate_table[i].freq_hz,
						 channels);
		const char *expect = (req <= pkts_avail && pkts_avail > 0) ?
				     "fit" : "NO";
		const char *eld_has = (eld_rates & rate_table[i].flag) ?
				     "yes" : "no";

		igt_info("      %5s: req=%d fit=%s  (ELD: %s)\n",
			 rate_table[i].name, req, expect, eld_has);
	}
}

/* Run the BPC × channels matrix for a given mode/hblank. */
static void test_audio_bw_matrix(data_t *data, const drmModeModeInfo *mode)
{
	int hblank = hblank_of(mode);

	igt_info("=== Audio BW matrix: %s hblank=%d ===\n",
		 mode->name, hblank);

	for (int b = 0; b < ARRAY_SIZE(bpc_values); b++) {
		int bpc = bpc_values[b];

		for (int c = 0; c < ARRAY_SIZE(channel_values); c++) {
			int channels = channel_values[c];
			const struct edid *edid;
			int pkts, ret;
			bool audio;
			unsigned int eld_rates;

			edid = build_edid(bpc, channels);
			force_edid_and_connector(data, edid);

			igt_output_set_prop_value(data->output,
						  IGT_CONNECTOR_MAX_BPC, bpc);

			pkts = avail_pkts_per_line(bpc, hblank);

			igt_info("\n  %dbpc %dch hblank=%d avail_pkts=%d\n",
				 bpc, channels, hblank, pkts);

			ret = try_modeset(data, mode);

			if (ret) {
				igt_info("    modeset: REJECTED\n");
				cleanup_connector(data);
				continue;
			}

			wait_for_eld();

			audio = audio_is_active();
			eld_rates = audio ? get_eld_rates() : 0;

			igt_info("    modeset: OK\n");
			igt_info("    audio:   %s\n", audio ? "active" : "inactive");

			igt_assert_f(!(pkts == 0 && audio),
				     "Audio active with 0 available packets\n");
			igt_assert_f(!(pkts > 0 && !audio),
				     "Audio inactive with %d available packets\n",
				     pkts);

			if (audio) {
				log_eld_rates(ALL_SAMPLE_RATES, eld_rates);
				assert_per_rate(mode, channels, pkts,
						eld_rates);
			}

			log_per_rate_analysis(mode, bpc, channels,
					      pkts, eld_rates);

			cleanup_connector(data);
		}
	}

	igt_info("\n=== End matrix ===\n");
}

static void test_audio_bw_supported(data_t *data)
{
	test_audio_bw_matrix(data, &mode_1080p_hblank160);
}

static void test_audio_bw_pruned(data_t *data)
{
	test_audio_bw_matrix(data, &mode_1080p_hblank80);
}

static void test_audio_recovery(data_t *data, enum igt_suspend_state state,
				bool runtime)
{
	const struct edid *edid;
	bool audio_before, audio_after;
	unsigned int rates_before, rates_after;
	char before_str[128], after_str[128];
	int ret;

	edid = build_edid(12, 2);
	force_edid_and_connector(data, edid);

	igt_output_set_prop_value(data->output, IGT_CONNECTOR_MAX_BPC, 12);

	ret = try_modeset(data, &mode_1080p_hblank80);
	igt_require(ret == 0);

	wait_for_eld();

	audio_before = audio_is_active();
	rates_before = audio_before ? get_eld_rates() : 0;
	rates_to_str(rates_before, before_str, sizeof(before_str));
	igt_info("Before %s: audio=%d rates=%s\n",
				 runtime ? "DPMS cycle" : "suspend",
		 audio_before, before_str);

	if (runtime) {
		kmstest_set_connector_dpms(data->drm_fd,
					   data->output->config.connector,
					   DRM_MODE_DPMS_OFF);
		kmstest_set_connector_dpms(data->drm_fd,
					   data->output->config.connector,
					   DRM_MODE_DPMS_ON);
	} else {
		igt_system_suspend_autoresume(state, SUSPEND_TEST_NONE);
	}

	wait_for_eld();

	audio_after = audio_is_active();
	rates_after = audio_after ? get_eld_rates() : 0;
	rates_to_str(rates_after, after_str, sizeof(after_str));
	igt_info("After %s:  audio=%d rates=%s\n",
				 runtime ? "DPMS cycle" : "suspend",
		 audio_after, after_str);

	igt_assert_eq(audio_before, audio_after);
	if (audio_before)
		igt_assert_eq(rates_before, rates_after);

	cleanup_connector(data);
}

int igt_main()
{
	data_t data = {};

	igt_fixture() {
		data.drm_fd = drm_open_driver_master(DRIVER_INTEL | DRIVER_XE);
		igt_require(is_intel_device(data.drm_fd));
		kmstest_set_vt_graphics_mode();
		igt_display_require(&data.display, data.drm_fd);

		data.output = find_hdmi_output(&data.display);
		igt_require_f(data.output, "No HDMI connector found\n");

		data.crtc = igt_first_crtc(&data.display);
		igt_require_f(data.crtc, "No usable CRTC found\n");
	}

	igt_describe("Validate HDMI audio bandwidth with 160-pixel horizontal "
		     "blanking across BPC and channel combinations.");
	igt_subtest("audio-bw-hblank160")
		test_audio_bw_supported(&data);

	igt_describe("Validate HDMI audio bandwidth with 80-pixel horizontal "
		     "blanking across BPC and channel combinations.");
	igt_subtest("audio-bw-hblank80")
		test_audio_bw_pruned(&data);

	igt_describe("Validate audio recovery after suspend to memory with "
		     "constrained hblank.");
	igt_subtest("suspend-mem-audio-recovery")
		test_audio_recovery(&data, SUSPEND_STATE_MEM, false);

	igt_describe("Validate audio recovery after suspend to disk "
		     "(hibernate) with constrained hblank.");
	igt_subtest("suspend-disk-audio-recovery")
		test_audio_recovery(&data, SUSPEND_STATE_DISK, false);

	igt_describe("Validate audio recovery after a DPMS off/on cycle "
		     "with constrained hblank.");
	igt_subtest("dpms-audio-recovery")
		test_audio_recovery(&data, 0, true);

	igt_fixture() {
		igt_display_fini(&data.display);
		drm_close_driver(data.drm_fd);
	}
}
