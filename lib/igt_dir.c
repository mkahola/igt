// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include <dirent.h>
#include <fcntl.h>

#include "igt.h"
#include "igt_dir.h"

/**
 * SECTION:igt_dir
 * @short_description: Dir and file processing i-g-t
 * @title: igt_dir
 * @include: igt_igt.h
 *
 * Utilities to facilitate reading and processing files within a directory.
 * For example, to read and discard all files from debugfs:
 *
 * igt_fixture {
 *	fd = drm_open_driver_master(DRIVER_ANY);
 *	debugfs = igt_debugfs_dir(fd);
 * }
 *
 * igt_dir = igt_dir_create(debugfs);
 * igt_dir_scan_dirfd(igt_dir, -1); // -1 means unlimited scan depth
 * igt_dir_process_files(igt_dir, NULL, NULL);
 *
 * igt_fixture {
 *	igt_dir_destroy(igt_dir);
 *	closedir(debugfs);
 *	drm_close_driver(fd);
 * }
 *
 * The igt_dir_scan_dirfd() function builds a linked list of files (using
 * igt_list), making it easy to add or remove specific files before
 * processing. If you only want to process a predetermined set of files,
 * you can skip the scan step and add the files directly to the list.
 *
 * The last two parameters of igt_dir_process_files() specify a callback
 * function and user data. If the callback is NULL, a default "read and
 * discard" function is used.
 *
 * Alternatively a "_simple" interface is also available. This function
 * encapsulate the calls to igt_dir_create(), igt_dir_scan_dirfd(),
 * igt_dir_process_files(), and igt_dir_destroy(). For using the "_simple"
 * interface:
 *
 * igt_fixture {
 *	fd = drm_open_driver_master(DRIVER_ANY);
 *	debugfs = igt_debugfs_dir(fd);
 * }
 *
 * igt_dir_process_files_simple(debugfs);
 *
 * igt_fixture {
 *	igt_dir_destroy(igt_dir);
 *	closedir(debugfs);
 *	drm_close_driver(fd);
 * }
 */

/**
 * igt_dir_get_fd_path: Get the path of a file descriptor
 * @fd: file descriptor to get the path for
 * @path: buffer to store the path
 * @path_len: length of the buffer
 *
 * Returns: 0 on success, a negative error code on failure
 */
int igt_dir_get_fd_path(int fd, char *path, size_t path_len)
{
	ssize_t len;
	char proc_path[64];

	snprintf(proc_path, sizeof(proc_path), "/proc/self/fd/%d", fd);
	len = readlink(proc_path, path, path_len - 1);
	if (len == -1)
		return -1;

	path[len] = '\0';

	return 0;
}

/**
 * igt_dir_callback_read_discard: Default callback function for reading and
 *				  discarding file contents
 * @filename: Path to the file
 * @callback_data: Optional pointer to user-defined data passed to the callback
 *
 * Returns: 0 on success, a negative error code on failure
 */
int igt_dir_callback_read_discard(const char *filename,
				  void *callback_data)
{
	int fd;
	char buf[4096];
	ssize_t bytes_read;

	fd = open(filename, O_RDONLY);
	if (fd < 0) {
		igt_debug("Failed to open file %s\n", filename);
		return -1;
	}

	bytes_read = read(fd, buf, sizeof(buf) - 1);
	if (bytes_read < 0) {
		igt_debug("Failed to read file %s\n", filename);
		close(fd);
		return -1;
	}

	buf[bytes_read] = '\0';
	igt_debug("Read %zd bytes from file %s: %s\n", bytes_read,
		  filename, buf);
	close(fd);

	return 0;
}

/**
 * igt_dir_create: Create a new igt_dir_t struct
 * @dirfd: file descriptor of the root directory
 *
 * Returns: Pointer to the new igt_dir_t struct, or NULL on failure
 */
igt_dir_t *igt_dir_create(int dirfd)
{
	igt_dir_t *config;
	size_t path_len = 512;
	char path[path_len];

	config = malloc(sizeof(igt_dir_t));
	if (!config)
		return NULL;

	config->dirfd = dirfd;

	igt_dir_get_fd_path(dirfd, path, path_len);
	igt_require(path[0] != '\0');

	config->root_path = malloc(path_len);
	if (!config->root_path) {
		free(config);
		return NULL;
	}

	strncpy(config->root_path, path, path_len);

	IGT_INIT_LIST_HEAD(&config->file_list_head);

	config->callback = NULL;

	return config;
}

static int _igt_dir_scan_dirfd(igt_dir_t *config, int scan_maxdepth,
			       int depth, const char *current_path)
{
	struct dirent *entry;
	igt_dir_file_list_t *file_list_entry;
	DIR *dirp;
	int dirfd;
	int ret = 0;

	if (depth > scan_maxdepth && scan_maxdepth != -1) {
		igt_debug("Max scan depth reached\n");
		return 0;
	}

	if (!current_path) {
		igt_debug("Invalid current path\n");
		return -1;
	}

	dirfd = open(current_path, O_RDONLY | O_DIRECTORY);
	if (dirfd < 0) {
		igt_debug("Failed to open directory %s\n", current_path);
		return -1;
	}

	dirp = fdopendir(dirfd);
	if (!dirp) {
		igt_debug("Failed to fdopendir %s\n", current_path);
		close(dirfd);
		return -1;
	}

	while ((entry = readdir(dirp))) {
		char entry_path[PATH_MAX];

		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;

		snprintf(entry_path, sizeof(entry_path),
			 "%s/%s", current_path, entry->d_name);

		if (entry->d_type == DT_DIR) {
			ret = _igt_dir_scan_dirfd(config, scan_maxdepth,
						  depth + 1, entry_path);
			if (ret)
				break;
		} else {
			/* Compute path relative to the scan root */
			const char *relative_path = entry_path +
						    strlen(config->root_path);
			if (*relative_path == '/')
				relative_path++; /* skip leading slash */

			file_list_entry = malloc(sizeof(igt_dir_file_list_t));
			if (!file_list_entry) {
				igt_debug("Failed to allocate memory for file list entry\n");
				continue;
			}
			file_list_entry->relative_path = strdup(relative_path);
			file_list_entry->match = true;
			igt_list_add(&file_list_entry->link,
				     &config->file_list_head);
		}
	}

	closedir(dirp);
	close(dirfd);

	return ret;
}

/**
 * igt_dir_scan_dirfd: Perform a directory scan based on config.
 * @config: Pointer to the igt_dir struct
 * @scan_maxdepth: Maximum depth to scan the directory. -1 means no limit
 *
 * Returns: 0 on success, a negative error code on failure
 */
int igt_dir_scan_dirfd(igt_dir_t *config, int scan_maxdepth)
{
	igt_require(config);
	igt_require(config->root_path);
	igt_require(config->dirfd >= 0);
	igt_require(scan_maxdepth >= -1);

	/* If the linked list is not empty, clean it first */
	if (!igt_list_empty(&config->file_list_head)) {
		igt_dir_file_list_t *file_list_entry, *tmp;

		igt_list_for_each_entry_safe(file_list_entry, tmp,
					     &config->file_list_head, link) {
			free(file_list_entry->relative_path);
			free(file_list_entry);
		}
	}

	return _igt_dir_scan_dirfd(config, scan_maxdepth, 0, config->root_path);
}

/**
 * igt_dir_process_files: Process files in the directory
 * @config: Pointer to the igt_dir struct
 * @callback: Callback function to process each file
 * @callback_data: Optional pointer to user-defined data passed to the callback
 *
 * Returns: 0 on success, a negative error code on failure
 */
int igt_dir_process_files(igt_dir_t *config,
			  igt_dir_file_callback callback,
			  void *callback_data)
{
	igt_dir_file_list_t *file_list_entry;
	int ret = 0;

	igt_require(config);
	igt_require(config->root_path);
	igt_require(config->dirfd >= 0);

	if (!callback)
		callback = igt_dir_callback_read_discard;

	igt_list_for_each_entry(file_list_entry, &config->file_list_head, link) {
		/* Only if match is true */
		if (file_list_entry->match) {
			char full_path[PATH_MAX];

			snprintf(full_path, sizeof(full_path),
				 "%s/%s", config->root_path,
				 file_list_entry->relative_path);
			ret = callback(full_path, callback_data);
			if (ret)
				break;
		}
	}

	return ret;
}

/**
 * igt_dir_destroy: Destroy the igt_dir struct
 * @config: Pointer to the igt_dir struct
 *
 * Returns: 0 on success, a negative error code on failure
 */
void igt_dir_destroy(igt_dir_t *config)
{
	igt_dir_file_list_t *file_list_entry, *tmp;

	igt_require(config);

	igt_list_for_each_entry_safe(file_list_entry, tmp,
				     &config->file_list_head, link) {
		free(file_list_entry->relative_path);
		free(file_list_entry);
	}

	free(config->root_path);
	free(config);
}

/**
 * igt_dir_process_files_simple: Process files in the directory using the
 *				 default callback to read and discard file
 *				 contents.
 *
 * @dirfd: file descriptor of the root directory
 *
 * Returns: 0 on success, a negative error code on failure
 */
int igt_dir_process_files_simple(int dirfd)
{
	igt_dir_t *config;
	int ret;

	config = igt_dir_create(dirfd);
	if (!config)
		return -1;

	igt_dir_scan_dirfd(config, -1);

	/* Use the default callback to read and discard file contents */
	ret = igt_dir_process_files(config, NULL, NULL);

	igt_dir_destroy(config);

	return ret;
}
