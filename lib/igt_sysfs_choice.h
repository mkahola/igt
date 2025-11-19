/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2026 Intel Corporation
 */
#ifndef __IGT_SYSFS_CHOICE_H__
#define __IGT_SYSFS_CHOICE_H__

#include <stddef.h>
#include <stdbool.h>

#define IGT_SYSFS_CHOICE_MAX_LEN    256
#define IGT_SYSFS_CHOICE_MAX_TOKENS 16

/**
 * struct igt_sysfs_choice - parsed sysfs enumerated choice attribute
 * @tokens:      array of token strings
 * @num_tokens:  number of entries in @tokens
 * @selected:    index of the active token in @tokens, or -1 if invalid
 *
 * This struct represents a sysfs enumerated choice attribute, for example:
 *
 *	"low [normal] high\n"
 *
 * After parsing, @tokens point to "low", "normal", "high" and
 * @selected will be 1 (the index of "normal").
 */
struct igt_sysfs_choice {
	char    buf[IGT_SYSFS_CHOICE_MAX_LEN];
	char   *tokens[IGT_SYSFS_CHOICE_MAX_TOKENS];
	size_t  num_tokens;
	int     selected; /* index into tokens[], or -1 */
};

int igt_sysfs_choice_parse(const char *buf, struct igt_sysfs_choice *choice);
int igt_sysfs_choice_read(int dirfd, const char *attr,
			  struct igt_sysfs_choice *choice);
const char *igt_sysfs_choice_selected(const struct igt_sysfs_choice *choice);
int igt_sysfs_choice_to_string(const struct igt_sysfs_choice *choice,
			       char *buf, size_t buf_sz);
int igt_sysfs_choice_find(const struct igt_sysfs_choice *choice,
			  const char *token);
int igt_sysfs_choice_to_mask(const struct igt_sysfs_choice *choice,
			     const char *const *names, size_t names_sz,
			     unsigned int *mask, int *selected_idx);
int igt_sysfs_choice_format_mask(char *buf, size_t buf_sz,
				 const char *const *names,
				 size_t names_sz, unsigned int mask,
				 int selected_idx);
int igt_sysfs_choice_intersect(struct igt_sysfs_choice *dst,
			       const struct igt_sysfs_choice *other);

#endif /* __IGT_SYSFS_CHOICE_H__ */
