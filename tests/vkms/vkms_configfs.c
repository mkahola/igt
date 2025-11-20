// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Google LLC.
 * Copyright © 2023 Collabora, Ltd.
 * Copyright © 2024-2025 Red Hat, Inc.
 */

/**
 * TEST: Tests for VKMS configfs support.
 * Category: Display
 * Mega feature: General Display Features
 * Sub-category: uapi
 * Functionality: vkms,configfs
 * Test category: functionality test
 */

#include <dirent.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>

#include "drmtest.h"
#include "igt.h"
#include "igt_device_scan.h"
#include "igt_vkms.h"

struct invalid_value {
	const char *value;
	int size;
};

static void assert_default_files(const char *path,
				 const char **files, size_t n_files,
				 const char **dirs, size_t n_dirs)
{
	DIR *dir;
	struct dirent *ent;
	int total = 0;
	int ret;

	/* Check that the number of files/directories matches the expected */
	dir = opendir(path);
	igt_assert(dir);
	while ((ent = readdir(dir)) != NULL) {
		if (strcmp(ent->d_name, ".") == 0 ||
		    strcmp(ent->d_name, "..") == 0)
			continue;

		total++;
	}
	igt_assert_eq(total, n_dirs + n_files);
	closedir(dir);

	/* Check that the files/directories are present */
	for (int i = 0; i < n_files; i++) {
		char file_path[PATH_MAX];
		struct stat buf;

		ret = snprintf(file_path, sizeof(file_path), "%s/%s", path,
			       files[i]);
		igt_assert(ret >= 0 && ret < sizeof(file_path));

		igt_assert_f(stat(file_path, &buf) == 0,
			     "File %s does not exists\n", file_path);
	}

	for (int i = 0; i < n_dirs; i++) {
		char dir_path[PATH_MAX];

		ret = snprintf(dir_path, sizeof(dir_path), "%s/%s", path,
			       dirs[i]);
		igt_assert(ret >= 0 && ret < sizeof(dir_path));

		dir = opendir(dir_path);
		igt_assert_f(dir, "Directory %s does not exists\n", dir_path);
		closedir(dir);
	}
}

static void assert_wrong_bool_values(const char *path)
{
	struct invalid_value invalid_bool_values[] = {
		{ "", 0 },
		{ "\0", 1 },
		{ "-1", 2 },
		{ "2", 1 },
		{ "o", 1 },
		{ "invalid", 8 },
	};
	int fd;
	int ret;

	for (int i = 0; i < ARRAY_SIZE(invalid_bool_values); i++) {
		struct invalid_value v = invalid_bool_values[i];

		fd = open(path, O_WRONLY);
		igt_assert_f(fd >= 0, "Error opening '%s'\n", path);

		ret = write(fd, v.value, v.size);
		igt_assert_f(ret <= 0, "Error writing '%s' to '%s'", v.value, path);

		close(fd);
	}
}

static bool attach(const char *src_path, const char *dst_path,
		   const char *link_name)
{
	char link_path[PATH_MAX];
	int ret;

	ret = snprintf(link_path, sizeof(link_path), "%s/%s", src_path, link_name);
	igt_assert(ret >= 0 && ret < sizeof(link_path));

	ret = symlink(dst_path, link_path);

	return ret == 0;
}

static bool find_device(const char *name, struct igt_device_card *card)
{
	igt_devices_scan();

	return igt_device_find_card_by_sysname(name, card);
}

static bool device_exists(const char *name)
{
	struct igt_device_card card;

	return find_device(name, &card);
}

static void assert_device_config(igt_vkms_config_t *cfg)
{
	struct igt_device_card card;
	drmModeResPtr res;
	drmModePlaneResPtr plane_res;
	drmModeConnectorPtr connector_res;
	igt_vkms_crtc_config_t *crtc;
	igt_vkms_connector_config_t *connector;
	bool found;
	int n_planes = 0;
	int n_crtcs = 0;
	int n_encoders = 0;
	int n_connectors = 0;
	int n_connector_status_cfg[4] = {0};
	int n_connector_status_drm[4] = {0};
	int fd;

	found = find_device(cfg->device_name, &card);
	igt_assert_f(found, "Device '%s' not found\n", cfg->device_name);

	fd = igt_open_card(&card);
	igt_assert_f(fd >= 0, "Error opening device '%s' at path '%s'\n",
		     cfg->device_name, card.card);
	igt_assert_f(!drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1),
		     "Error setting DRM_CLIENT_CAP_UNIVERSAL_PLANES\n");
	igt_assert_f(!drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1),
		     "Error setting DRM_CLIENT_CAP_ATOMIC\n");
	igt_assert_f(!drmSetClientCap(fd, DRM_CLIENT_CAP_WRITEBACK_CONNECTORS, 1),
		     "Error setting DRM_CLIENT_CAP_WRITEBACK_CONNECTORS\n");

	res = drmModeGetResources(fd);
	igt_assert_f(res, "Error getting resources\n");
	plane_res = drmModeGetPlaneResources(fd);
	igt_assert_f(plane_res, "Error getting plane resources\n");

	for (int n = 0; (&cfg->planes[n])->name; n++)
		n_planes++;

	for (int n = 0; (crtc = &cfg->crtcs[n])->name; n++) {
		n_crtcs++;

		if (crtc->writeback) {
			n_encoders++;
			n_connectors++;
			n_connector_status_cfg[DRM_MODE_UNKNOWNCONNECTION]++;
		}
	}

	for (int n = 0; (&cfg->encoders[n])->name; n++)
		n_encoders++;

	for (int n = 0; (connector = &cfg->connectors[n])->name; n++) {
		n_connectors++;
		n_connector_status_cfg[connector->status]++;
	}

	for (int n = 0; n < res->count_connectors; n++) {
		connector_res = drmModeGetConnectorCurrent(fd,
							   res->connectors[n]);
		n_connector_status_drm[connector_res->connection]++;
		drmModeFreeConnector(connector_res);
	}

	igt_assert_eq(n_planes, plane_res->count_planes);
	igt_assert_eq(n_crtcs, res->count_crtcs);
	igt_assert_eq(n_encoders, res->count_encoders);
	igt_assert_eq(n_connectors, res->count_connectors);
	igt_assert_eq(n_connector_status_cfg[DRM_MODE_CONNECTED],
		      n_connector_status_drm[DRM_MODE_CONNECTED]);
	igt_assert_eq(n_connector_status_cfg[DRM_MODE_DISCONNECTED],
		      n_connector_status_drm[DRM_MODE_DISCONNECTED]);
	igt_assert_eq(n_connector_status_cfg[DRM_MODE_UNKNOWNCONNECTION],
		      n_connector_status_drm[DRM_MODE_UNKNOWNCONNECTION]);

	drmModeFreePlaneResources(plane_res);
	drmModeFreeResources(res);
	close(fd);
}

/**
 * SUBTEST: device-default-files
 * Description: Test that creating a VKMS device creates the default files and
 *              directories.
 */

static void test_device_default_files(void)
{
	igt_vkms_t *dev;

	static const char *files[] = {
		"enabled",
	};

	static const char *dirs[] = {
		"planes",
		"crtcs",
		"encoders",
		"connectors",
	};

	dev = igt_vkms_device_create(__func__);
	igt_assert(dev);

	assert_default_files(dev->path,
			     files, ARRAY_SIZE(files),
			     dirs, ARRAY_SIZE(dirs));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: device-default-values
 * Description: Check that the default values for the device are correct.
 */

static void test_device_default_values(void)
{
	igt_vkms_t *dev;

	dev = igt_vkms_device_create(__func__);
	igt_assert(dev);

	igt_assert(!igt_vkms_device_is_enabled(dev));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: device-wrong-values
 * Description: Check that setting unexpected values doesn't work.
 */

static void test_device_wrong_values(void)
{
	igt_vkms_t *dev;
	char path[PATH_MAX];

	/* It is not possible to create devices named "vkms" to avoid clashes
	 * with the default device created by VKMS
	 */
	dev = igt_vkms_device_create("vkms");
	igt_assert(!dev);

	/* Test invalid values for "enabled" */
	dev = igt_vkms_device_create(__func__);
	igt_assert(dev);

	igt_vkms_get_device_enabled_path(dev, path, sizeof(path));

	assert_wrong_bool_values(path);
	igt_assert(!igt_vkms_device_is_enabled(dev));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: plane-default-files
 * Description: Test that creating a plane creates the default files and
 *              directories.
 */

static void test_plane_default_files(void)
{
	igt_vkms_t *dev;
	char path[PATH_MAX];

	static const char *files[] = {
		"type",
	};

	static const char *dirs[] = {
		"possible_crtcs",
	};

	dev = igt_vkms_device_create(__func__);
	igt_assert(dev);

	igt_vkms_device_add_plane(dev, "plane0");
	igt_vkms_get_plane_path(dev, "plane0", path, sizeof(path));

	assert_default_files(path,
			     files, ARRAY_SIZE(files),
			     dirs, ARRAY_SIZE(dirs));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: plane-default-values
 * Description: Check that the default values for the plane are correct.
 */

static void test_plane_default_values(void)
{
	igt_vkms_t *dev;

	dev = igt_vkms_device_create(__func__);
	igt_assert(dev);

	igt_vkms_device_add_plane(dev, "plane0");

	igt_assert_eq(igt_vkms_plane_get_type(dev, "plane0"),
		      DRM_PLANE_TYPE_OVERLAY);

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: plane-wrong-values
 * Description: Check that setting unexpected values doesn't work.
 */

static void test_plane_wrong_values(void)
{
	struct invalid_value invalid_type_values[] = {
		{ "", 0 },
		{ "\0", 1 },
		{ "-1", 2 },
		{ "4", 1 },
		{ "primary", 8 },
		{ "overlay", 8 },
	};
	igt_vkms_t *dev;
	char path[PATH_MAX];
	int fd;
	int ret;

	/* Create a device with a primary plane */
	dev = igt_vkms_device_create(__func__);
	igt_assert(dev);

	igt_vkms_device_add_plane(dev, "plane0");
	igt_vkms_plane_set_type(dev, "plane0", DRM_PLANE_TYPE_PRIMARY);
	igt_assert_eq(igt_vkms_plane_get_type(dev, "plane0"),
		      DRM_PLANE_TYPE_PRIMARY);
	igt_vkms_get_plane_type_path(dev, "plane0", path, sizeof(path));

	/* Test invalid values for "type" */
	for (int i = 0; i < ARRAY_SIZE(invalid_type_values); i++) {
		struct invalid_value v = invalid_type_values[i];

		fd = open(path, O_WRONLY);
		igt_assert_f(fd >= 0, "Error opening '%s'\n", path);

		ret = write(fd, v.value, v.size);
		igt_assert_f(ret <= 0, "Error writing '%s' to '%s'", v.value, path);

		close(fd);
	}

	igt_assert_eq(igt_vkms_plane_get_type(dev, "plane0"),
		      DRM_PLANE_TYPE_PRIMARY);

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: plane-valid-values
 * Description: Check that setting valid values works.
 */

static void test_plane_valid_values(void)
{
	igt_vkms_t *dev;

	dev = igt_vkms_device_create(__func__);
	igt_assert(dev);

	igt_vkms_device_add_plane(dev, "plane0");

	/* Test valid values for "type" */
	igt_vkms_plane_set_type(dev, "plane0", DRM_PLANE_TYPE_OVERLAY);
	igt_assert_eq(igt_vkms_plane_get_type(dev, "plane0"),
		      DRM_PLANE_TYPE_OVERLAY);

	igt_vkms_plane_set_type(dev, "plane0", DRM_PLANE_TYPE_PRIMARY);
	igt_assert_eq(igt_vkms_plane_get_type(dev, "plane0"),
		      DRM_PLANE_TYPE_PRIMARY);

	igt_vkms_plane_set_type(dev, "plane0", DRM_PLANE_TYPE_CURSOR);
	igt_assert_eq(igt_vkms_plane_get_type(dev, "plane0"),
		      DRM_PLANE_TYPE_CURSOR);

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: crtc-default-files
 * Description: Test that creating a CRTC creates the default files and
 *              directories.
 */

static void test_crtc_default_files(void)
{
	igt_vkms_t *dev;
	char path[PATH_MAX];

	static const char *files[] = {
		"writeback",
	};

	dev = igt_vkms_device_create(__func__);
	igt_assert(dev);

	igt_vkms_device_add_crtc(dev, "crtc0");
	igt_vkms_get_crtc_path(dev, "crtc0", path, sizeof(path));

	assert_default_files(path,
			     files, ARRAY_SIZE(files),
			     NULL, 0);

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: crtc-default-values
 * Description: Check that the default values for the CRTC are correct.
 */

static void test_crtc_default_values(void)
{
	igt_vkms_t *dev;

	dev = igt_vkms_device_create(__func__);
	igt_assert(dev);

	igt_vkms_device_add_crtc(dev, "crtc0");

	igt_assert(!igt_vkms_crtc_is_writeback_enabled(dev, "crtc0"));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: crtc-wrong-values
 * Description: Check that setting unexpected values doesn't work.
 */

static void test_crtc_wrong_values(void)
{
	igt_vkms_t *dev;
	char path[PATH_MAX];

	/* Test invalid values for "writeback" */
	dev = igt_vkms_device_create(__func__);
	igt_assert(dev);

	igt_vkms_device_add_crtc(dev, "crtc0");
	igt_vkms_crtc_set_writeback_enabled(dev, "crtc0", true);
	igt_assert(igt_vkms_crtc_is_writeback_enabled(dev, "crtc0"));
	igt_vkms_get_crtc_writeback_path(dev, "crtc0", path, sizeof(path));

	assert_wrong_bool_values(path);
	igt_assert(igt_vkms_crtc_is_writeback_enabled(dev, "crtc0"));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: crtc-valid-values
 * Description: Check that setting valid values works.
 */

static void test_crtc_valid_values(void)
{
	igt_vkms_t *dev;

	dev = igt_vkms_device_create(__func__);
	igt_assert(dev);

	igt_vkms_device_add_crtc(dev, "crtc0");

	/* Test valid values for "writeback" */
	igt_vkms_crtc_set_writeback_enabled(dev, "crtc0", true);
	igt_assert(igt_vkms_crtc_is_writeback_enabled(dev, "crtc0"));

	igt_vkms_crtc_set_writeback_enabled(dev, "crtc0", false);
	igt_assert(!igt_vkms_crtc_is_writeback_enabled(dev, "crtc0"));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: encoder-default-files
 * Description: Test that creating a encoder creates the default files and
 *              directories.
 */

static void test_encoder_default_files(void)
{
	igt_vkms_t *dev;
	char path[PATH_MAX];

	static const char *dirs[] = {
		"possible_crtcs",
	};

	dev = igt_vkms_device_create(__func__);
	igt_assert(dev);

	igt_vkms_device_add_encoder(dev, "encoder0");
	igt_vkms_get_encoder_path(dev, "encoder0", path, sizeof(path));

	assert_default_files(path,
			     NULL, 0,
			     dirs, ARRAY_SIZE(dirs));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: connector-default-files
 * Description: Test that creating a connector creates the default files and
 *              directories.
 */

static void test_connector_default_files(void)
{
	igt_vkms_t *dev;
	char path[PATH_MAX];

	static const char *files[] = {
		"status",
	};

	static const char *dirs[] = {
		"possible_encoders",
	};

	dev = igt_vkms_device_create(__func__);
	igt_assert(dev);

	igt_vkms_device_add_connector(dev, "connector0");
	igt_vkms_get_connector_path(dev, "connector0", path, sizeof(path));

	assert_default_files(path,
			     files, ARRAY_SIZE(files),
			     dirs, ARRAY_SIZE(dirs));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: connector-default-values
 * Description: Check that the default values for the connector are correct.
 */

static void test_connector_default_values(void)
{
	igt_vkms_t *dev;

	dev = igt_vkms_device_create(__func__);
	igt_assert(dev);

	igt_vkms_device_add_connector(dev, "connector0");

	igt_assert_eq(igt_vkms_connector_get_status(dev, "connector0"),
		      DRM_MODE_CONNECTED);

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: connector-wrong-values
 * Description: Check that setting unexpected values doesn't work.
 */

static void test_connector_wrong_values(void)
{
	struct invalid_value invalid_status_values[] = {
		{ "", 0 },
		{ "\0", 1 },
		{ "-1", 2 },
		{ "0", 1 },
		{ "4", 1 },
		{ "connected", 10 },
	};
	igt_vkms_t *dev;
	char path[PATH_MAX];
	int fd;
	int ret;

	/* Create a device with a disconnected connector */
	dev = igt_vkms_device_create(__func__);
	igt_assert(dev);

	igt_vkms_device_add_connector(dev, "connector0");
	igt_vkms_connector_set_status(dev, "connector0", DRM_MODE_DISCONNECTED);
	igt_assert_eq(igt_vkms_connector_get_status(dev, "connector0"),
		      DRM_MODE_DISCONNECTED);
	igt_vkms_get_connector_status_path(dev, "connector0", path, sizeof(path));

	/* Test invalid values for "status" */
	for (int i = 0; i < ARRAY_SIZE(invalid_status_values); i++) {
		struct invalid_value v = invalid_status_values[i];

		fd = open(path, O_WRONLY);
		igt_assert_f(fd >= 0, "Error opening '%s'\n", path);

		ret = write(fd, v.value, v.size);
		igt_assert(ret <= 0);

		close(fd);
	}

	igt_assert_eq(igt_vkms_connector_get_status(dev, "connector0"),
		      DRM_MODE_DISCONNECTED);

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: connector-valid-values
 * Description: Check that setting valid values works.
 */

static void test_connector_valid_values(void)
{
	igt_vkms_t *dev;

	dev = igt_vkms_device_create(__func__);
	igt_assert(dev);

	igt_vkms_device_add_connector(dev, "connector0");

	/* Test valid values for "status" */
	igt_vkms_connector_set_status(dev, "connector0", DRM_MODE_DISCONNECTED);
	igt_assert_eq(igt_vkms_connector_get_status(dev, "connector0"),
		      DRM_MODE_DISCONNECTED);

	igt_vkms_connector_set_status(dev, "connector0", DRM_MODE_CONNECTED);
	igt_assert_eq(igt_vkms_connector_get_status(dev, "connector0"),
		      DRM_MODE_CONNECTED);

	igt_vkms_connector_set_status(dev, "connector0", DRM_MODE_UNKNOWNCONNECTION);
	igt_assert_eq(igt_vkms_connector_get_status(dev, "connector0"),
		      DRM_MODE_UNKNOWNCONNECTION);

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: attach-plane-to-crtc
 * Description: Check that errors are handled while attaching planes to CRTCs.
 */

static void test_attach_plane_to_crtc(void)
{
	igt_vkms_t *dev1;
	igt_vkms_t *dev2;
	char plane1[PATH_MAX];
	char crtc1[PATH_MAX];
	char connector1[PATH_MAX];
	char crtc2[PATH_MAX];
	char dev2_enabled_path[PATH_MAX];
	bool ok;

	dev1 = igt_vkms_device_create("test_attach_plane_to_crtc_1");
	igt_assert(dev1);

	dev2 = igt_vkms_device_create("test_attach_plane_to_crtc_2");
	igt_assert(dev2);

	igt_vkms_device_add_plane(dev1, "plane1");
	igt_vkms_device_add_crtc(dev1, "crtc1");
	igt_vkms_device_add_connector(dev1, "connector1");
	igt_vkms_device_add_crtc(dev2, "crtc2");

	igt_vkms_get_plane_possible_crtcs_path(dev1, "plane1", plane1, sizeof(plane1));
	igt_vkms_get_crtc_path(dev1, "crtc1", crtc1, sizeof(crtc1));
	igt_vkms_get_connector_path(dev1, "connector1", connector1, sizeof(connector1));
	igt_vkms_get_crtc_path(dev2, "crtc2", crtc2, sizeof(crtc2));
	igt_vkms_get_device_enabled_path(dev2, dev2_enabled_path, sizeof(dev2_enabled_path));

	/* Error: Attach a plane to a connector */
	ok = attach(plane1, connector1, "connector");
	igt_assert_f(!ok, "Attaching plane1 to connector1 should fail\n");

	/* Error: Attach a plane to a random file */
	ok = attach(plane1, dev2_enabled_path, "file");
	igt_assert_f(!ok, "Attaching plane1 to a random file should fail\n");

	/* Error: Attach a plane to a CRTC from other device */
	ok = attach(plane1, crtc2, "crtc2");
	igt_assert_f(!ok, "Attaching plane1 to crtc2 should fail\n");

	/* OK: Attaching plane1 to crtc1 */
	ok = igt_vkms_plane_attach_crtc(dev1, "plane1", "crtc1");
	igt_assert_f(ok, "Error attaching plane1 to crtc1\n");

	/* Error: Attaching plane1 to crtc1 twice */
	ok = attach(plane1, crtc1, "crtc1_duplicated");
	igt_assert_f(!ok, "Error attaching plane1 to crtc1 twice should fail");

	/* OK: Detaching and attaching again */
	ok = igt_vkms_plane_detach_crtc(dev1, "plane1", "crtc1");
	igt_assert_f(ok, "Error detaching plane1 from crtc1\n");
	ok = igt_vkms_plane_attach_crtc(dev1, "plane1", "crtc1");
	igt_assert_f(ok, "Error attaching plane1 to crtc1\n");

	igt_vkms_device_destroy(dev1);
	igt_vkms_device_destroy(dev2);
}

/**
 * SUBTEST: attach-encoder-to-crtc
 * Description: Check that errors are handled while attaching encoders to CRTCs.
 */

static void test_attach_encoder_to_crtc(void)
{
	igt_vkms_t *dev1;
	igt_vkms_t *dev2;
	char plane1[PATH_MAX];
	char crtc1[PATH_MAX];
	char encoder1[PATH_MAX];
	char plane1_type_path[PATH_MAX];
	char crtc2[PATH_MAX];
	bool ok;

	dev1 = igt_vkms_device_create("test_attach_encoder_to_crtc_1");
	igt_assert(dev1);

	dev2 = igt_vkms_device_create("test_attach_encoder_to_crtc_2");
	igt_assert(dev2);

	igt_vkms_device_add_plane(dev1, "plane1");
	igt_vkms_device_add_crtc(dev1, "crtc1");
	igt_vkms_device_add_encoder(dev1, "encoder1");
	igt_vkms_device_add_crtc(dev2, "crtc2");

	igt_vkms_get_plane_path(dev1, "plane1", plane1, sizeof(plane1));
	igt_vkms_get_crtc_path(dev1, "crtc1", crtc1, sizeof(crtc1));
	igt_vkms_get_encoder_possible_crtcs_path(dev1, "encoder1", encoder1,
						 sizeof(encoder1));
	igt_vkms_get_crtc_path(dev2, "crtc2", crtc2, sizeof(crtc2));
	igt_vkms_get_plane_type_path(dev1, "plane1", plane1_type_path,
				     sizeof(plane1_type_path));

	/* Error: Attach a encoder to a plane */
	ok = attach(encoder1, plane1, "plane");
	igt_assert_f(!ok, "Attaching encoder1 to plane1 should fail\n");

	/* Error: Attach a encoder to a random file */
	ok = attach(encoder1, plane1_type_path, "file");
	igt_assert_f(!ok, "Attaching encoder1 to a random file should fail\n");

	/* Error: Attach a encoder to a CRTC from other device */
	ok = attach(encoder1, crtc2, "crtc2");
	igt_assert_f(!ok, "Attaching encoder1 to crtc2 should fail\n");

	/* OK: Attaching encoder1 to crtc1 */
	ok = igt_vkms_encoder_attach_crtc(dev1, "encoder1", "crtc1");
	igt_assert_f(ok, "Error attaching plane1 to crtc1\n");

	/* Error: Attaching encoder1 to crtc1 twice */
	ok = attach(encoder1, crtc1, "crtc1_duplicated");
	igt_assert_f(!ok, "Error attaching encoder1 to crtc1 twice should fail");

	/* OK: Detaching and attaching again */
	ok = igt_vkms_encoder_detach_crtc(dev1, "encoder1", "crtc1");
	igt_assert_f(ok, "Error detaching encoder1 from crtc1\n");
	ok = igt_vkms_encoder_attach_crtc(dev1, "encoder1", "crtc1");
	igt_assert_f(ok, "Error attaching encoder1 to crtc1\n");

	igt_vkms_device_destroy(dev1);
	igt_vkms_device_destroy(dev2);
}

/**
 * SUBTEST: attach-connector-to-encoder
 * Description: Check that errors are handled while attaching connectors to
 *              encoders.
 */

static void test_attach_connector_to_encoder(void)
{
	igt_vkms_t *dev1;
	igt_vkms_t *dev2;
	char crtc1[PATH_MAX];
	char encoder1[PATH_MAX];
	char connector1[PATH_MAX];
	char encoder2[PATH_MAX];
	char crtc1_writeback_path[PATH_MAX];
	bool ok;

	dev1 = igt_vkms_device_create("test_attach_encoder_to_crtc_1");
	igt_assert(dev1);

	dev2 = igt_vkms_device_create("test_attach_encoder_to_crtc_2");
	igt_assert(dev2);

	igt_vkms_device_add_crtc(dev1, "crtc1");
	igt_vkms_device_add_encoder(dev1, "encoder1");
	igt_vkms_device_add_connector(dev1, "connector1");
	igt_vkms_device_add_encoder(dev2, "encoder2");

	igt_vkms_get_crtc_path(dev1, "crtc1", crtc1, sizeof(crtc1));
	igt_vkms_get_encoder_path(dev1, "encoder1", encoder1, sizeof(encoder1));
	igt_vkms_get_connector_possible_encoders_path(dev1, "connector1",
						      connector1,
						      sizeof(connector1));
	igt_vkms_get_encoder_path(dev2, "encoder2", encoder2, sizeof(encoder2));
	igt_vkms_get_crtc_writeback_path(dev1, "crtc1", crtc1_writeback_path,
					 sizeof(crtc1_writeback_path));

	/* Error: Attach a connector to a CRTC */
	ok = attach(connector1, crtc1, "crtc");
	igt_assert_f(!ok, "Attaching connector1 to crtc1 should fail\n");

	/* Error: Attach a connector to a random file */
	ok = attach(connector1, crtc1_writeback_path, "file");
	igt_assert_f(!ok, "Attaching connector1 to a random file should fail\n");

	/* Error: Attach a connector to an encoder from other device */
	ok = attach(connector1, encoder2, "encoder2");
	igt_assert_f(!ok, "Attaching connector1 to encoder2 should fail\n");

	/* OK: Attaching connector1 to encoder1 */
	ok = igt_vkms_connector_attach_encoder(dev1, "connector1", "encoder1");
	igt_assert_f(ok, "Error attaching plane1 to crtc1\n");

	/* Error: Attaching connector1 to encoder1 twice */
	ok = attach(connector1, encoder1, "encoder1_duplicated");
	igt_assert_f(!ok, "Error attaching connector1 to encoder1 twice should fail");

	/* OK: Detaching and attaching again */
	ok = igt_vkms_connector_detach_encoder(dev1, "connector1", "encoder1");
	igt_assert_f(ok, "Error detaching connector1 from encoder1\n");
	ok = igt_vkms_connector_attach_encoder(dev1, "connector1", "encoder1");
	igt_assert_f(ok, "Error attaching connector1 to encoder1\n");

	igt_vkms_device_destroy(dev1);
	igt_vkms_device_destroy(dev2);
}

/**
 * SUBTEST: enable-no-pipeline-items
 * Description: Try to enable a VKMS device without adding any pipeline items
 *              and test that it fails.
 */

static void test_enable_no_pipeline_items(void)
{
	igt_vkms_t *dev;

	dev = igt_vkms_device_create(__func__);
	igt_assert(dev);

	/* Try to enable it and check that the device is not set as enabled */
	igt_vkms_device_set_enabled(dev, true);
	igt_assert(!igt_vkms_device_is_enabled(dev));

	/* Check that no actual device was created*/
	igt_assert(!device_exists(__func__));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enable-no-planes
 * Description: Try to enable a VKMS device without adding planes and test that
 *              it fails.
 */

static void test_enable_no_planes(void)
{
	igt_vkms_t *dev;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = { },
		.crtcs = {
			{ .name = "crtc0" },
			{ .name = "crtc1" },
		},
		.encoders = {
			{ .name = "encoder0", .possible_crtcs = { "crtc0" } },
			{ .name = "encoder1", .possible_crtcs = { "crtc1" } },
		},
		.connectors = {
			{
				.name = "connector0",
				.possible_encoders = { "encoder0", "encoder1" },
			},
		},
	};

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(!igt_vkms_device_is_enabled(dev));
	igt_assert(!device_exists(__func__));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enable-too-many-planes
 * Description: Try to enable a VKMS device with too many planes and test that
 *              it fails.
 */

static void test_enable_too_many_planes(void)
{
	igt_vkms_t *dev;
	char plane_names[VKMS_MAX_PIPELINE_ITEMS][8];
	int ret;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = {
			{
				.name = "plane0",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc0" },
			},
		},
		.crtcs = {
			{ .name = "crtc0" },
		},
		.encoders = {
			{ .name = "encoder0", .possible_crtcs = { "crtc0" } },
		},
		.connectors = {
			{
				.name = "connector0",
				.possible_encoders = { "encoder0" },
			},
		},
	};

	for (int n = 1; n < 32; n++) {
		ret = snprintf(plane_names[n], sizeof(plane_names[n]),
			       "plane%d", n);
		igt_assert(ret >= 0 && ret < sizeof(plane_names[n]));

		cfg.planes[n] = (igt_vkms_plane_config_t){
			.name = plane_names[n],
			.possible_crtcs = { "crtc0" },
		};
	}

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(!igt_vkms_device_is_enabled(dev));
	igt_assert(!device_exists(__func__));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enable-no-primary-plane
 * Description: Try to enable a VKMS device without a primary plane for one of
 *              its CRTCs and test that it fails.
 */

static void test_enable_no_primary_plane(void)
{
	igt_vkms_t *dev;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = {
			{
				.name = "plane0",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc0" },
			},
			{
				.name = "plane1",
				.type = DRM_PLANE_TYPE_CURSOR,
				.possible_crtcs = { "crtc1" },
			},
		},
		.crtcs = {
			{ .name = "crtc0" },
			{ .name = "crtc1" },
		},
		.encoders = {
			{ .name = "encoder0", .possible_crtcs = { "crtc0" } },
			{ .name = "encoder1", .possible_crtcs = { "crtc1" } },
		},
		.connectors = {
			{
				.name = "connector0",
				.possible_encoders = { "encoder0", "encoder1" },
			},
		},
	};

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(!igt_vkms_device_is_enabled(dev));
	igt_assert(!device_exists(__func__));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enable-multiple-primary-planes
 * Description: Try to enable a VKMS device with multiple primary planes for one
 *              of its CRTCs and test that it fails.
 */

static void test_enable_multiple_primary_planes(void)
{
	igt_vkms_t *dev;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = {
			{
				.name = "plane0",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc0" },
			},
			{
				.name = "plane1",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc1" },
			},
			{
				.name = "plane2",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc1" },
			},
		},
		.crtcs = {
			{ .name = "crtc0" },
			{ .name = "crtc1" },
		},
		.encoders = {
			{ .name = "encoder0", .possible_crtcs = { "crtc0" } },
			{ .name = "encoder1", .possible_crtcs = { "crtc1" } },
		},
		.connectors = {
			{
				.name = "connector0",
				.possible_encoders = { "encoder0", "encoder1" },
			},
		},
	};

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(!igt_vkms_device_is_enabled(dev));
	igt_assert(!device_exists(__func__));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enable-multiple-cursor-planes
 * Description: Try to enable a VKMS device with multiple cursor planes for one
 *              of its CRTCs and test that it fails.
 */

static void test_enable_multiple_cursor_planes(void)
{
	igt_vkms_t *dev;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = {
			{
				.name = "plane0",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc0" },
			},
			{
				.name = "plane1",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc1" },
			},
			{
				.name = "plane2",
				.type = DRM_PLANE_TYPE_CURSOR,
				.possible_crtcs = { "crtc1" },
			},
			{
				.name = "plane3",
				.type = DRM_PLANE_TYPE_CURSOR,
				.possible_crtcs = { "crtc1" },
			},
		},
		.crtcs = {
			{ .name = "crtc0" },
			{ .name = "crtc1" },
		},
		.encoders = {
			{ .name = "encoder0", .possible_crtcs = { "crtc0" } },
			{ .name = "encoder1", .possible_crtcs = { "crtc1" } },
		},
		.connectors = {
			{
				.name = "connector0",
				.possible_encoders = { "encoder0", "encoder1" },
			},
		},
	};

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(!igt_vkms_device_is_enabled(dev));
	igt_assert(!device_exists(__func__));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enable-plane-no-crtcs
 * Description: Try to enable a VKMS device with a plane without possible CRTCs
 *              and test that it fails.
 */

static void test_enable_plane_no_crtcs(void)
{
	igt_vkms_t *dev;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = {
			{
				.name = "plane0",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc0" },
			},
			{
				.name = "plane1",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = {},
			},
		},
		.crtcs = {
			{ .name = "crtc0" },
			{ .name = "crtc1" },
		},
		.encoders = {
			{ .name = "encoder0", .possible_crtcs = { "crtc0" } },
			{ .name = "encoder1", .possible_crtcs = { "crtc1" } },
		},
		.connectors = {
			{
				.name = "connector0",
				.possible_encoders = { "encoder0", "encoder1" },
			},
		},
	};

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(!igt_vkms_device_is_enabled(dev));
	igt_assert(!device_exists(__func__));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enable-no-crtcs
 * Description: Try to enable a VKMS device without adding CRTCs and test that
 *              it fails.
 */

static void test_enable_no_crtcs(void)
{
	igt_vkms_t *dev;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = {
			{
				.name = "plane0",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = {},
			},
		},
		.crtcs = { },
		.encoders = {
			{ .name = "encoder0", .possible_crtcs = { "crtc0" } },
		},
		.connectors = {
			{
				.name = "connector0",
				.possible_encoders = { "encoder0" },
			},
		},
	};

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(!igt_vkms_device_is_enabled(dev));
	igt_assert(!device_exists(__func__));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enable-too-many-crtcs
 * Description: Try to enable a VKMS device with too many CRTCs and test that it
 *              fails.
 */

static void test_enable_too_many_crtcs(void)
{
	igt_vkms_t *dev;
	char crtc_names[VKMS_MAX_PIPELINE_ITEMS][7];
	int ret;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = {
			{
				.name = "plane0",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc0" },
			},
		},
		.crtcs = {},
		.encoders = {
			{ .name = "encoder0", .possible_crtcs = { "crtc0" } },
		},
		.connectors = {
			{
				.name = "connector0",
				.possible_encoders = { "encoder0" },
			},
		},
	};

	for (int n = 0; n < 32; n++) {
		ret = snprintf(crtc_names[n], sizeof(crtc_names[n]),
			       "crtc%d", n);
		igt_assert(ret >= 0 && ret < sizeof(crtc_names[n]));

		cfg.crtcs[n] = (igt_vkms_crtc_config_t){
			.name = crtc_names[n],
		};
	}

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(!igt_vkms_device_is_enabled(dev));
	igt_assert(!device_exists(__func__));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enable-no-encoders
 * Description: Try to enable a VKMS device without adding encoders and test
 *              that it fails.
 */

static void test_enable_no_encoders(void)
{
	igt_vkms_t *dev;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = {
			{
				.name = "plane0",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc0" },
			},
		},
		.crtcs = {
			{ .name = "crtc0" },
		},
		.encoders = { },
		.connectors = {
			{
				.name = "connector0",
				.possible_encoders = {},
			},
		},
	};

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(!igt_vkms_device_is_enabled(dev));
	igt_assert(!device_exists(__func__));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enable-too-many-encoders
 * Description: Try to enable a VKMS device with too many encoders and test that
 *              it fails.
 */

static void test_enable_too_many_encoders(void)
{
	igt_vkms_t *dev;
	char encoder_names[VKMS_MAX_PIPELINE_ITEMS][10];
	int ret;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = {
			{
				.name = "plane0",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc0" },
			},
		},
		.crtcs = {
			{ .name = "crtc0" },
		},
		.encoders = {},
		.connectors = {
			{
				.name = "connector0",
				.possible_encoders = { "encoder0" },
			},
		},
	};

	for (int n = 0; n < 32; n++) {
		ret = snprintf(encoder_names[n], sizeof(encoder_names[n]),
			       "encoder%d", n);
		igt_assert(ret >= 0 && ret < sizeof(encoder_names[n]));

		cfg.encoders[n] = (igt_vkms_encoder_config_t){
			.name = encoder_names[n],
			.possible_crtcs = { "crtc0" }
		};
	}

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(!igt_vkms_device_is_enabled(dev));
	igt_assert(!device_exists(__func__));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enable-encoder-no-crtcs
 * Description: Try to enable a VKMS device with an encoder without possible
 *              CRTCs and test that it fails.
 */

static void test_enable_encoder_no_crtcs(void)
{
	igt_vkms_t *dev;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = {
			{
				.name = "plane0",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc0" },
			},
		},
		.crtcs = {
			{ .name = "crtc0" },
		},
		.encoders = {
			{ .name = "encoder0", .possible_crtcs = {} },
		},
		.connectors = {
			{
				.name = "connector0",
				.possible_encoders = { "encoder0" },
			},
		},
	};

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(!igt_vkms_device_is_enabled(dev));
	igt_assert(!device_exists(__func__));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enable-crtc-no-encoder
 * Description: Try to enable a VKMS device with a CRTC without encoders and
 *              test that it fails.
 */

static void test_enable_crtc_no_encoder(void)
{
	igt_vkms_t *dev;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = {
			{
				.name = "plane0",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc0" },
			},
			{
				.name = "plane1",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc1" },
			},
		},
		.crtcs = {
			{ .name = "crtc0" },
			{ .name = "crtc1" },
		},
		.encoders = {
			{ .name = "encoder0", .possible_crtcs = { "crtc0" } },
			{ .name = "encoder1", .possible_crtcs = { "crtc0" } },
		},
		.connectors = {
			{
				.name = "connector0",
				.possible_encoders = { "encoder0", "encoder1" },
			},
		},
	};

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(!igt_vkms_device_is_enabled(dev));
	igt_assert(!device_exists(__func__));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enable-no-connectors
 * Description: Try to enable a VKMS device without adding connectors and test
 *              that it fails.
 */

static void test_enable_no_connectors(void)
{
	igt_vkms_t *dev;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = {
			{
				.name = "plane0",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc0"},
			},
		},
		.crtcs = {
			{ .name = "crtc0" },
		},
		.encoders = {
			{ .name = "encoder0", .possible_crtcs = { "crtc0" } },
		},
		.connectors = { },
	};

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(!igt_vkms_device_is_enabled(dev));
	igt_assert(!device_exists(__func__));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enable-too-many-connectors
 * Description: Try to enable a VKMS device with too many connectors and test
 *              that it fails.
 */

static void test_enable_too_many_connectors(void)
{
	igt_vkms_t *dev;
	char connector_names[VKMS_MAX_PIPELINE_ITEMS][12];
	int ret;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = {
			{
				.name = "plane0",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc0" },
			},
		},
		.crtcs = {
			{ .name = "crtc0" },
		},
		.encoders = {
			{ .name = "encoder0", .possible_crtcs = { "crtc0" } },
		},
		.connectors = {},
	};

	for (int n = 0; n < 32; n++) {
		ret = snprintf(connector_names[n], sizeof(connector_names[n]),
			       "connector%d", n);
		igt_assert(ret >= 0 && ret < sizeof(connector_names[n]));

		cfg.connectors[n] = (igt_vkms_connector_config_t){
			.name = connector_names[n],
			.possible_encoders = { "encoder0" }
		};
	}

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(!igt_vkms_device_is_enabled(dev));
	igt_assert(!device_exists(__func__));

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enabled-plane-cannot-change
 * Description: Test that, once a VKMS device is enabled, the plane values can't
 *              change and that deleting it or the attached CRTCs doesn't change
 *              the VKMS device.
 */

static void test_enabled_plane_cannot_change(void)
{
	igt_vkms_t *dev;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = {
			{
				.name = "plane0",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc0"},
			},
			{
				.name = "plane1",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc1"},
			},
		},
		.crtcs = {
			{ .name = "crtc0" },
			{ .name = "crtc1" },
		},
		.encoders = {
			{ .name = "encoder0", .possible_crtcs = { "crtc0" } },
			{ .name = "encoder1", .possible_crtcs = { "crtc1" } },
		},
		.connectors = {
			{
				.name = "connector0",
				.possible_encoders = { "encoder0", "encoder1" },
			},
		},
	};

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(igt_vkms_device_is_enabled(dev));
	assert_device_config(&cfg);

	/* Try to change values */
	igt_vkms_plane_set_type(dev, "plane0", DRM_PLANE_TYPE_OVERLAY);
	igt_assert_eq(igt_vkms_plane_get_type(dev, "plane0"),
		      DRM_PLANE_TYPE_PRIMARY);

	igt_assert(!igt_vkms_plane_attach_crtc(dev, "plane0", "crtc1"));

	/* Deleting pipeline items doesn't affect the device */
	igt_assert(igt_vkms_plane_detach_crtc(dev, "plane0", "crtc0"));
	igt_assert(igt_vkms_device_remove_plane(dev, "plane0"));
	assert_device_config(&cfg);

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enabled-crtc-cannot-change
 * Description: Test that, once a VKMS device is enabled, the CRTC values can't
 *              change and that deleting it or the attached planes/encoders
 *              doesn't change the VKMS device.
 */

static void test_enabled_crtc_cannot_change(void)
{
	igt_vkms_t *dev;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = {
			{
				.name = "plane0",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc0"},
			},
		},
		.crtcs = {
			{ .name = "crtc0", .writeback = true },
		},
		.encoders = {
			{ .name = "encoder0", .possible_crtcs = { "crtc0" } },
		},
		.connectors = {
			{
				.name = "connector0",
				.possible_encoders = { "encoder0" },
			},
		},
	};

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(igt_vkms_device_is_enabled(dev));
	assert_device_config(&cfg);

	/* Try to change values */
	igt_vkms_crtc_set_writeback_enabled(dev, "crtc0", false);
	igt_assert(igt_vkms_crtc_is_writeback_enabled(dev, "crtc0"));

	/* Deleting pipeline items doesn't affect the device */
	igt_assert(igt_vkms_plane_detach_crtc(dev, "plane0", "crtc0"));
	igt_assert(igt_vkms_encoder_detach_crtc(dev, "encoder0", "crtc0"));
	igt_assert(igt_vkms_device_remove_crtc(dev, "crtc0"));
	assert_device_config(&cfg);

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enabled-encoder-cannot-change
 * Description: Test that, once a VKMS device is enabled, the encoder values
 *              can't change and that deleting it or the attached CRTCs doesn't
 *              change the VKMS device.
 */

static void test_enabled_encoder_cannot_change(void)
{
	igt_vkms_t *dev;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = {
			{
				.name = "plane0",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc0"},
			},
			{
				.name = "plane1",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc1"},
			},
		},
		.crtcs = {
			{ .name = "crtc0" },
			{ .name = "crtc1" },
		},
		.encoders = {
			{ .name = "encoder0", .possible_crtcs = { "crtc0" } },
			{ .name = "encoder1", .possible_crtcs = { "crtc1" } },
		},
		.connectors = {
			{
				.name = "connector0",
				.possible_encoders = { "encoder0", "encoder1" },
			},
		},
	};

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(igt_vkms_device_is_enabled(dev));
	assert_device_config(&cfg);

	/* Try to change values */
	igt_assert(!igt_vkms_encoder_attach_crtc(dev, "encoder0", "crtc1"));

	/* Deleting pipeline items doesn't affect the device */
	igt_assert(igt_vkms_encoder_detach_crtc(dev, "encoder0", "crtc0"));
	igt_assert(igt_vkms_connector_detach_encoder(dev, "connector0",
						     "encoder0"));
	igt_assert(igt_vkms_device_remove_encoder(dev, "encoder0"));
	assert_device_config(&cfg);

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enabled-connector-cannot-change
 * Description: Test that, once a VKMS device is enabled, the connector values,
 *              with the exception of status, can't change and that deleting it
 *              or the attached encoders doesn't change the VKMS device.
 *              Connector hot-plug/unplug is not covered by this test.
 */

static void test_enabled_connector_cannot_change(void)
{
	igt_vkms_t *dev;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = {
			{
				.name = "plane0",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc0"},
			},
			{
				.name = "plane1",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc1"},
			},
		},
		.crtcs = {
			{ .name = "crtc0" },
			{ .name = "crtc1" },
		},
		.encoders = {
			{ .name = "encoder0", .possible_crtcs = { "crtc0" } },
			{ .name = "encoder1", .possible_crtcs = { "crtc1" } },
		},
		.connectors = {
			{
				.name = "connector0",
				.status = DRM_MODE_CONNECTED,
				.possible_encoders = { "encoder0" },
			},
		},
	};

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(igt_vkms_device_is_enabled(dev));
	assert_device_config(&cfg);

	/* Try to change values */
	igt_assert(!igt_vkms_connector_attach_encoder(dev, "connector0",
						      "encoder1"));

	/* Deleting pipeline items doesn't affect the device */
	igt_assert(igt_vkms_connector_detach_encoder(dev, "connector0",
						     "encoder0"));
	igt_assert(igt_vkms_device_remove_connector(dev, "connector0"));
	assert_device_config(&cfg);

	igt_vkms_device_destroy(dev);
}

/**
 * SUBTEST: enabled-connector-hot-plug
 * Description: Test that, once a VKMS device is enabled, the connectors can be
 *              hot-plugged and unplugged.
 */

static void test_enabled_connector_hot_plug(void)
{
	igt_vkms_t *dev;
	drmModeConnection status;

	igt_vkms_config_t cfg = {
		.device_name = __func__,
		.planes = {
			{
				.name = "plane0",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc0"},
			},
			{
				.name = "plane1",
				.type = DRM_PLANE_TYPE_PRIMARY,
				.possible_crtcs = { "crtc1"},
			},
		},
		.crtcs = {
			{ .name = "crtc0", .writeback = true },
			{ .name = "crtc1", .writeback = true },
		},
		.encoders = {
			{ .name = "encoder0", .possible_crtcs = { "crtc0" } },
			{ .name = "encoder1", .possible_crtcs = { "crtc1" } },
		},
		.connectors = {
			{
				.name = "connector0",
				.status = DRM_MODE_DISCONNECTED,
				.possible_encoders = { "encoder0" },
			},
			{
				.name = "connector1",
				.status = DRM_MODE_DISCONNECTED,
				.possible_encoders = { "encoder1" },
			},
		},
	};

	dev = igt_vkms_device_create_from_config(&cfg);
	igt_assert(dev);

	igt_vkms_device_set_enabled(dev, true);
	igt_assert(igt_vkms_device_is_enabled(dev));
	assert_device_config(&cfg);

	/* Connect both connectors */
	cfg.connectors[0].status = DRM_MODE_CONNECTED;
	igt_vkms_connector_set_status(dev, "connector0", DRM_MODE_CONNECTED);
	status = igt_vkms_connector_get_status(dev, "connector0");
	igt_assert_eq(status, DRM_MODE_CONNECTED);
	assert_device_config(&cfg);

	cfg.connectors[1].status = DRM_MODE_CONNECTED;
	igt_vkms_connector_set_status(dev, "connector1", DRM_MODE_CONNECTED);
	status = igt_vkms_connector_get_status(dev, "connector1");
	igt_assert_eq(status, DRM_MODE_CONNECTED);
	assert_device_config(&cfg);

	/* Set one to unknown connection and disconnect the other one */
	cfg.connectors[0].status = DRM_MODE_UNKNOWNCONNECTION;
	igt_vkms_connector_set_status(dev, "connector0", DRM_MODE_UNKNOWNCONNECTION);
	status = igt_vkms_connector_get_status(dev, "connector0");
	igt_assert_eq(status, DRM_MODE_UNKNOWNCONNECTION);
	assert_device_config(&cfg);

	cfg.connectors[1].status = DRM_MODE_DISCONNECTED;
	igt_vkms_connector_set_status(dev, "connector1", DRM_MODE_DISCONNECTED);
	status = igt_vkms_connector_get_status(dev, "connector1");
	igt_assert_eq(status, DRM_MODE_DISCONNECTED);
	assert_device_config(&cfg);

	igt_vkms_device_destroy(dev);
}

int igt_main()
{
	struct {
		const char *name;
		void (*fn)(void);
	} tests[] = {
		{ "device-default-files", test_device_default_files },
		{ "device-default-values", test_device_default_values },
		{ "device-wrong-values", test_device_wrong_values },
		{ "plane-default-files", test_plane_default_files },
		{ "plane-default-values", test_plane_default_values },
		{ "plane-wrong-values", test_plane_wrong_values },
		{ "plane-valid-values", test_plane_valid_values },
		{ "crtc-default-files", test_crtc_default_files },
		{ "crtc-default-values", test_crtc_default_values },
		{ "crtc-wrong-values", test_crtc_wrong_values },
		{ "crtc-valid-values", test_crtc_valid_values },
		{ "encoder-default-files", test_encoder_default_files },
		{ "connector-default-files", test_connector_default_files },
		{ "connector-default-values", test_connector_default_values },
		{ "connector-wrong-values", test_connector_wrong_values },
		{ "connector-valid-values", test_connector_valid_values },
		{ "attach-plane-to-crtc", test_attach_plane_to_crtc },
		{ "attach-encoder-to-crtc", test_attach_encoder_to_crtc },
		{ "attach-connector-to-encoder", test_attach_connector_to_encoder },
		{ "enable-no-pipeline-items", test_enable_no_pipeline_items },
		{ "enable-no-planes", test_enable_no_planes },
		{ "enable-too-many-planes", test_enable_too_many_planes },
		{ "enable-no-primary-plane", test_enable_no_primary_plane },
		{ "enable-multiple-primary-planes", test_enable_multiple_primary_planes },
		{ "enable-multiple-cursor-planes", test_enable_multiple_cursor_planes },
		{ "enable-plane-no-crtcs", test_enable_plane_no_crtcs },
		{ "enable-no-crtcs", test_enable_no_crtcs },
		{ "enable-too-many-crtcs", test_enable_too_many_crtcs },
		{ "enable-no-encoders", test_enable_no_encoders },
		{ "enable-too-many-encoders", test_enable_too_many_encoders },
		{ "enable-encoder-no-crtcs", test_enable_encoder_no_crtcs },
		{ "enable-crtc-no-encoder", test_enable_crtc_no_encoder },
		{ "enable-no-connectors", test_enable_no_connectors },
		{ "enable-too-many-connectors", test_enable_too_many_connectors },
		{ "enabled-plane-cannot-change", test_enabled_plane_cannot_change },
		{ "enabled-crtc-cannot-change", test_enabled_crtc_cannot_change },
		{ "enabled-encoder-cannot-change", test_enabled_encoder_cannot_change },
		{ "enabled-connector-cannot-change", test_enabled_connector_cannot_change },
		{ "enabled-connector-hot-plug", test_enabled_connector_hot_plug },
	};

	igt_fixture() {
		drm_load_module(DRIVER_VKMS);
		igt_require_vkms();
		igt_require_vkms_configfs();
		igt_vkms_destroy_all_devices();
	}

	for (int i = 0; i < ARRAY_SIZE(tests); i++) {
		igt_subtest(tests[i].name)
			tests[i].fn();
	}

	igt_fixture() {
		igt_require_vkms();
		igt_require_vkms_configfs();
		igt_vkms_destroy_all_devices();
	}
}
