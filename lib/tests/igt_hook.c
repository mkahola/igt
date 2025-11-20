// SPDX-License-Identifier: MIT
/*
 * Copyright(c) 2024 Intel Corporation. All rights reserved.
 */

#include <stdbool.h>
#include <stdio.h>
#include <unistd.h>

#include "igt_core.h"
#include "igt_hook.h"

static const char *env_var_names[] = {
	"IGT_HOOK_EVENT",
	"IGT_HOOK_TEST_FULLNAME",
	"IGT_HOOK_TEST",
	"IGT_HOOK_SUBTEST",
	"IGT_HOOK_DYN_SUBTEST",
	"IGT_HOOK_KMOD_UNBIND_MODULE_NAME",
	"IGT_HOOK_RESULT",
};

#define num_env_vars (sizeof(env_var_names) / sizeof(env_var_names[0]))

static int env_var_name_lookup(char *line)
{
	int i;
	char *c;

	c = strchr(line, '=');
	if (c)
		*c = '\0';

	for (i = 0; i < num_env_vars; i++)
		if (!strcmp(line, env_var_names[i]))
			goto out;

	i = -1;
out:
	if (c)
		*c = '=';

	return i;
}

static int igt_single_hook(const char *hook_str, struct igt_hook **igt_hook_ptr)
{
	const char *hook_strs[] = {
		hook_str,
	};

	return igt_hook_create(hook_strs, 1, igt_hook_ptr);
}

static void test_invalid_hook_descriptors(void)
{
	struct {
		const char *name;
		const char *hook_desc;
	} invalid_cases[] = {
		{"invalid-event-name", "invalid-event:echo hello"},
		{"invalid-empty-event-name", ":echo hello"},
		{"invalid-colon-in-cmd", "echo hello:world"},
		{},
	};

	for (int i = 0; invalid_cases[i].name; i++) {
		igt_subtest(invalid_cases[i].name) {
			int err;
			struct igt_hook *igt_hook;

			err = igt_single_hook(invalid_cases[i].hook_desc, &igt_hook);
			igt_assert(err != 0);
		}
	}
}

static void test_print_help(void)
{
	char *buf;
	size_t len;
	FILE *f;
	const char expected_initial_text[] = "The option --hook receives as argument a \"hook descriptor\"";

	f = open_memstream(&buf, &len);
	igt_assert(f);

	igt_hook_print_help(f, "--hook");
	fclose(f);

	igt_assert(!strncmp(buf, expected_initial_text, sizeof(expected_initial_text) - 1));

	/* This is an extra check to catch a case where an event type is added
	 * without a proper description. */
	igt_assert(!strstr(buf, "MISSING DESCRIPTION"));

	free(buf);
}

static void test_all_env_vars(void)
{
	struct igt_hook_evt evt = {
		.evt_type = IGT_HOOK_PRE_SUBTEST,
		.target_name = "foo",
	};
	bool env_vars_checklist[num_env_vars] = {};
	struct igt_hook *igt_hook;
	char *hook_str;
	FILE *f;
	int pipefd[2];
	int ret;
	int i;
	char *line;
	size_t line_size;

	ret = pipe(pipefd);
	igt_assert(ret == 0);

	/* Use grep to filter only env var set by us. This should ensure that
	 * writing to the pipe will not block due to capacity, since we only
	 * read from the pipe after the shell command is done. */
	ret = asprintf(&hook_str, "printenv -0 | grep -z ^IGT_HOOK >&%d", pipefd[1]);
	igt_assert(ret > 0);

	ret = igt_single_hook(hook_str, &igt_hook);
	igt_assert(ret == 0);

	igt_hook_event_notify(igt_hook, &evt);

	close(pipefd[1]);
	f = fdopen(pipefd[0], "r");
	igt_assert(f);

	line = NULL;
	line_size = 0;

	while (getdelim(&line, &line_size, '\0', f) != -1) {
		ret = env_var_name_lookup(line);
		igt_assert_f(ret >= 0, "Unexpected env var %s\n", line);
		env_vars_checklist[ret] = true;
	}

	for (i = 0; i < num_env_vars; i++)
		igt_assert_f(env_vars_checklist[i], "Missing env var %s\n", env_var_names[i]);

	fclose(f);
	igt_hook_free(igt_hook);
	free(hook_str);
	free(line);
}

igt_main()
{
	test_invalid_hook_descriptors();

	igt_subtest("help-description")
		test_print_help();

	igt_subtest_group() {
		igt_fixture() {
			igt_require_f(system(NULL), "Shell seems not to be available\n");
		}

		igt_subtest("all-env-vars")
			test_all_env_vars();
	}
}
