// SPDX-License-Identifier: MIT
/*
 * Copyright © 2026 Intel Corporation
 */
#include "igt_sysfs_choice.h"
#include <ctype.h>
#include <errno.h>
#include "igt_core.h"
#include "igt_sysfs.h"

#define IGT_SYSFS_CHOICE_MAX_LEN 256
#define IGT_SYSFS_CHOICE_MAX_TOKENS 16

/**
 * igt_sysfs_choice_parse() - parse sysfs enumerated choice buffer
 * @buf: NUL-terminated buffer with sysfs contents
 * @choice: output descriptor, must be non-NULL (can be zeroed)
 *
 * Parses a sysfs enumerated choice buffer, e.g.:
 *
 *	"low [normal] high\n"
 *
 * into a token list and the index of the selected token.
 *
 * Parsing rules:
 *  - tokens are separated by ASCII whitespace
 *  - exactly one token must be wrapped in '[' and ']'
 *  - surrounding '[' and ']' are stripped from the selected token
 *  - empty tokens are treated as malformed input
 *
 * On entry, any previous contents of @choice are freed.
 *
 * Returns:
 *  0        on success,
 *  -EINVAL  malformed format (no tokens, no selected token, multiple
 *           selected tokens, unterminated '[' or ']'),
 *  -E2BIG   on too many tokens or too small choice buffer size.
 */
int igt_sysfs_choice_parse(const char *buf, struct igt_sysfs_choice *choice)
{
	char *p, *tok_start;
	bool selected_seen = false;
	size_t num_tokens = 0;
	int n, selected = -1;
	bool is_selected;

	igt_assert(buf && choice);

	memset(choice, 0, sizeof(*choice));
	n = snprintf(choice->buf, sizeof(choice->buf), "%s", buf);
	if (igt_debug_on(n < 0))
		return -EINVAL;
	if (igt_debug_on((size_t)n >= sizeof(choice->buf)))
		return -E2BIG;

	choice->num_tokens = 0;
	choice->selected = -1;
	p = choice->buf;

	while (*p) {
		/* skip leading whitespace */
		while (*p && isspace((unsigned char)*p))
			p++;
		if (!*p)
			break;

		is_selected = false;
		tok_start = p;

		if (*p == '[') {
			is_selected = true;
			p++;
			tok_start = p;

			if (selected_seen) {
				igt_debug("choice-parse: multiple [selected] tokens: \"%s\"\n",
					  choice->buf);
				return -EINVAL;
			}
			selected_seen = true;
		}

		/* walk until ']' or whitespace */
		while (*p && !isspace((unsigned char)*p) && *p != ']')
			p++;

		if (is_selected) {
			if (*p != ']') {
				igt_debug("choice-parse: unterminated '[' in: \"%s\"\n",
					  choice->buf);
				return -EINVAL;
			}
		}

		/* terminate token */
		if (*p) {
			*p = '\0';
			p++;
		}

		if (!*tok_start) {
			igt_debug("choice-parse: empty token in: \"%s\"\n",
				  choice->buf);
			return -EINVAL;
		}

		if (num_tokens >= IGT_SYSFS_CHOICE_MAX_TOKENS) {
			igt_debug("choice-parse: too many tokens (>%d) in: \"%s\"\n",
				  IGT_SYSFS_CHOICE_MAX_TOKENS, choice->buf);
			return -E2BIG;
		}

		choice->tokens[num_tokens] = tok_start;
		if (is_selected)
			selected = (int)num_tokens;

		num_tokens++;
	}

	if (!num_tokens) {
		igt_debug("choice-parse: no tokens in string: \"%s\"\n",
			  choice->buf);
		return -EINVAL;
	}

	if (selected < 0) {
		igt_debug("choice-parse: missing selected token ([...]) in: \"%s\"\n",
			  choice->buf);
		return -EINVAL;
	}

	choice->num_tokens = num_tokens;
	choice->selected = selected;

	return 0;
}

/**
 * igt_sysfs_choice_read() - read and parse a sysfs enumerated choice attribute
 * @dirfd: directory file descriptor of the sysfs node
 * @attr: attribute name relative to @dirfd
 * @choice: output descriptor, must be non-NULL
 *
 * Reads the given sysfs attribute into a temporary buffer and parses it.
 *
 * Returns:
 *  0 on success,
 *  negative errno-style value on read or parse error.
 */
int igt_sysfs_choice_read(int dirfd, const char *attr,
			  struct igt_sysfs_choice *choice)
{
	char buf[IGT_SYSFS_CHOICE_MAX_LEN];
	int len;

	len = igt_sysfs_read(dirfd, attr, buf, sizeof(buf) - 1);
	if (len < 0)
		return len;

	buf[len] = '\0';

	return igt_sysfs_choice_parse(buf, choice);
}

/**
 * igt_sysfs_choice_selected() - Return selected token string
 * @choice: Parsed choice
 *
 * Returns:
 *   Pointer to the selected token string, or NULL if no valid selection.
 */
const char *igt_sysfs_choice_selected(const struct igt_sysfs_choice *choice)
{
	if (!choice || choice->selected < 0 ||
	    (size_t)choice->selected >= choice->num_tokens)
		return NULL;

	return choice->tokens[choice->selected];
}

/**
 * igt_sysfs_choice_to_string() - Render a parsed choice into string
 * @choice:   Parsed choice (tokens[] + selected index)
 * @buf:      Output buffer for formatted string
 * @buf_sz:   Size of @buf in bytes
 *
 * Formats the given @choice into the string:
 *
 *	"low [normal] high"
 *
 * Tokens are emitted in the order stored in @choice->tokens.  The
 * selected token (choice->selected) is wrapped in '[' and ']'.
 *
 * Returns:
 *   0        on success,
 *   -EINVAL  if arguments are invalid,
 *   -E2BIG   if @buf_sz is too small.
 */
int igt_sysfs_choice_to_string(const struct igt_sysfs_choice *choice,
			       char *buf, size_t buf_sz)
{
	bool first = true;
	size_t pos = 0;
	int n;

	if (!choice || !buf || !buf_sz)
		return -EINVAL;

	buf[0] = '\0';

	for (size_t i = 0; i < choice->num_tokens; i++) {
		const char *name = choice->tokens[i];
		bool is_selected = (choice->selected == (int)i);

		if (!name)
			continue;

		n = snprintf(buf + pos, buf_sz - pos,
			     "%s%s%s%s",
			     first ? "" : " ",
			     is_selected ? "[" : "",
			     name,
			     is_selected ? "]" : "");

		if (n < 0)
			return -EINVAL;
		if ((size_t)n >= buf_sz - pos)
			return -E2BIG;

		pos += (size_t)n;
		first = false;
	}

	return 0;
}

/**
 * igt_sysfs_choice_find() - find token index by name
 * @choice: parsed choice struct
 * @token: token to look for (plain name, without '[' / ']')
 *
 * Performs a case-sensitive comparison of @token against entries in
 * @choice->tokens.
 *
 * Returns:
 *  index in [0..choice->num_tokens-1] on match,
 *  -1 if @token is not present or @choice/@token is NULL.
 */
int igt_sysfs_choice_find(const struct igt_sysfs_choice *choice,
			  const char *token)
{
	if (!choice || !token)
		return -1;

	for (size_t i = 0; i < choice->num_tokens; i++)
		if (!strcmp(choice->tokens[i], token))
			return (int)i;

	return -1;
}

/**
 * igt_sysfs_choice_to_mask() - map parsed tokens to bitmask + selection
 * @choice: parsed choice struct
 * @names: array of known token names
 * @names_sz: number of elements in @names
 * @mask: output bitmask of supported names
 * @selected_idx: output index of selected token in @names, or -1 if selected
 *                token is not among @names
 *
 * Builds a bitmask of known tokens present in @choice and identifies the
 * selected token, if it matches one of @names.
 *
 * Unknown tokens do not cause an error; they are ignored and not
 * reflected in @mask. This keeps the API "loose": tests can still
 * validate required choices while tolerating additional values.
 *
 * Returns:
 *  0        on success,
 *  -EINVAL  on bad input parameters.
 */
int igt_sysfs_choice_to_mask(const struct igt_sysfs_choice *choice,
			     const char * const *names, size_t names_sz,
			     unsigned int *mask, int *selected_idx)
{
	unsigned int m = 0;
	int sel = -1, idx;

	if (!choice || !names || !mask)
		return -EINVAL;

	for (size_t i = 0; i < names_sz; i++) {
		const char *name = names[i];

		if (!name)
			continue;

		idx = igt_sysfs_choice_find(choice, name);
		if (idx >= 0) {
			m |= 1u << i;
			if (idx == choice->selected)
				sel = (int)i;
		}
	}

	*mask = m;
	if (selected_idx)
		*selected_idx = sel;

	return 0;
}

/**
 * igt_sysfs_choice_format_mask() - Format a bitmask as a space-separated list of names
 * @buf: Output buffer
 * @buf_sz: Size of @buf in bytes
 * @names: Array of token names indexed by bit position
 * @names_sz: Number of elements in @names
 * @mask: Bitmask of available tokens
 * @selected_idx: Index to highlight with brackets, or <0 for none
 *
 * Builds a space-separated list of all bits set in @mask, mapping bit positions
 * to names in @names. If @selected_idx >= 0 and that bit is set, the token is
 * wrapped in brackets, e.g. "low [normal] high".
 *
 * This function is best-effort by design:
 *  - If names[i] is NULL, it is formatted as "?".
 *  - Bits beyond @names_sz are ignored.
 * Empty @mask results in an empty string.
 *
 * Returns:
 *  0        on success,
 *  -EINVAL  on invalid arguments,
 *  -E2BIG   if @buf_sz is too small.
 */
int igt_sysfs_choice_format_mask(char *buf, size_t buf_sz,
				 const char *const *names,
				 size_t names_sz,
				 unsigned int mask,
				 int selected_idx)
{
	bool first = true;
	size_t pos = 0;

	if (!buf || !buf_sz || !names || !names_sz)
		return -EINVAL;

	buf[0] = '\0';

	for (size_t idx = 0; idx < names_sz && mask; idx++) {
		int n;
		const char *name;
		bool highlight;

		if (!(mask & 1u)) {
			mask >>= 1;
			continue;
		}

		name = names[idx] ?: "?";
		highlight = ((int)idx == selected_idx);
		n = snprintf(buf + pos, buf_sz - pos, "%s%s%s%s",
			     first ? "" : " ",
			     highlight ? "[" : "",
			     name,
			     highlight ? "]" : "");
		if (n < 0)
			return -EINVAL;
		if ((size_t)n >= buf_sz - pos)
			return -E2BIG;

		pos += (size_t)n;
		first = false;
		mask >>= 1;
	}

	return 0;
}

/**
 * igt_sysfs_choice_intersect() - Restrict a choice set to tokens common with another
 * @dst:   Choice to be updated in place
 * @other: Choice providing the allowed tokens
 *
 * Computes the intersection of the token sets in @dst and @other.
 * The resulting @dst contains only tokens that appear in both choices,
 * preserving their original order from @dst.
 *
 * If the previously selected token in @dst is still present after
 * intersection, its index is updated accordingly.  If it is not present,
 * @dst->selected is set to -1.
 *
 * Returns:
 * * 0        - success
 * * -EINVAL  - invalid arguments
 * * -ENOENT  - no common tokens
 */
int igt_sysfs_choice_intersect(struct igt_sysfs_choice *dst,
			       const struct igt_sysfs_choice *other)
{
	char *new_tokens[IGT_SYSFS_CHOICE_MAX_TOKENS];
	const char *selected_name;
	int new_selected = -1;
	size_t new_n = 0;

	if (!dst || !other)
		return -EINVAL;

	selected_name = (dst->selected >= 0 && dst->selected < dst->num_tokens) ?
			 dst->tokens[dst->selected] : NULL;

	for (size_t i = 0; i < dst->num_tokens; i++) {
		char *tok = dst->tokens[i];

		if (igt_sysfs_choice_find(other, tok) < 0)
			continue;

		new_tokens[new_n] = tok;

		if (selected_name && !strcmp(tok, selected_name))
			new_selected = (int)new_n;

		new_n++;
	}

	if (!new_n) {
		dst->num_tokens = 0;
		dst->selected = -1;
		return -ENOENT;
	}

	for (size_t i = 0; i < new_n; i++)
		dst->tokens[i] = new_tokens[i];

	dst->num_tokens = new_n;
	dst->selected = new_selected;

	return 0;
}
