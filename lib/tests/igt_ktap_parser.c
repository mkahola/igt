// SPDX-License-Identifier: MIT
/*
* Copyright © 2023 Intel Corporation
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "igt_core.h"
#include "igt_ktap.h"
#include "igt_list.h"

static void ktap_list(void)
{
	struct igt_ktap_result *result, *rn;
	struct igt_ktap_results *ktap;
	int suite = 1, test = 1;
	IGT_LIST_HEAD(results);

	ktap = igt_ktap_alloc(&results);
	igt_require(ktap);

	igt_assert_eq(igt_ktap_parse("KTAP version 1\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("1..3\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    KTAP version 1\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    # Subtest: test_suite_1\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    1..3\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    ok 1 test_case_1 # SKIP\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    ok 2 test_case_2 # SKIP\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    ok 3 test_case_3 # SKIP\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("ok 1 test_suite_1\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    KTAP version 1\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    # Subtest: test_suite_2\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    1..1\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    ok 1 test_case_1 # SKIP\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("ok 2 test_suite_2\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    KTAP version 1\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    # Subtest: test_suite_3\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    1..4\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    ok 1 test_case_1 # SKIP\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    ok 2 test_case_2 # SKIP\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    ok 3 test_case_3 # SKIP\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    ok 4 test_case_4 # SKIP\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("ok 3 test_suite_3\n", ktap), 0);

	igt_ktap_free(&ktap);

	igt_assert_eq(igt_list_length(&results), 8);

	igt_list_for_each_entry_safe(result, rn, &results, link) {
		char *case_name, *suite_name;

		igt_list_del(&result->link);

		igt_assert_lt(0, asprintf(&case_name, "test_case_%u", test));
		igt_assert_lt(0, asprintf(&suite_name, "test_suite_%u", suite));

		igt_assert(result->case_name);
		igt_assert_eq(strcmp(result->case_name, case_name), 0);
		free(result->case_name);
		free(case_name);

		igt_assert(result->suite_name);
		igt_assert_eq(strcmp(result->suite_name, suite_name), 0);
		free(suite_name);

		igt_assert(!result->msg);
		igt_assert_eq(result->code, IGT_EXIT_SKIP);

		if ((suite == 1 && test < 3) || (suite == 3 && test < 4)) {
			test++;
		} else {
			free(result->suite_name);
			suite++;
			test = 1;
		}

		free(result);
	}
}

static void ktap_results(void)
{
	struct igt_ktap_result *result;
	struct igt_ktap_results *ktap;
	char *suite_name, *case_name;
	IGT_LIST_HEAD(results);

	ktap = igt_ktap_alloc(&results);
	igt_require(ktap);

	igt_assert_eq(igt_ktap_parse("KTAP version 1\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("1..1\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    KTAP version 1\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    # Subtest: test_suite\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    1..1\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("        KTAP version 1\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("        # Subtest: test_case\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("        ok 1 parameter 1\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("        ok 2 parameter 2 # a comment\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("        ok 3 parameter 3 # SKIP\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("        ok 4 parameter 4 # SKIP with a message\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("        not ok 5 parameter 5\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("        not ok 6 parameter 6 # failure message\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    ok 1 test_case\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("not ok 1 test_suite\n", ktap), 0);

	igt_ktap_free(&ktap);

	igt_assert_eq(igt_list_length(&results), 2);

	result = igt_list_first_entry(&results, result, link);
	igt_list_del(&result->link);
	igt_assert_eq(strcmp(result->suite_name, "test_suite"), 0);
	igt_assert_eq(strcmp(result->case_name, "test_case"), 0);
	igt_assert_eq(result->code, IGT_EXIT_INVALID);
	igt_assert(!result->msg);
	free(result->msg);
	suite_name = result->suite_name;
	case_name = result->case_name;
	free(result);

	result = igt_list_first_entry(&results, result, link);
	igt_list_del(&result->link);
	igt_assert_eq(strcmp(result->suite_name, suite_name), 0);
	igt_assert_eq(strcmp(result->case_name, case_name), 0);
	igt_assert_neq(result->code, IGT_EXIT_INVALID);
	free(result->msg);
	free(suite_name);
	free(case_name);
	free(result);
}

static void ktap_success(void)
{
	struct igt_ktap_result *result;
	struct igt_ktap_results *ktap;
	IGT_LIST_HEAD(results);

	ktap = igt_ktap_alloc(&results);
	igt_require(ktap);

	igt_assert_eq(igt_ktap_parse("KTAP version 1\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("1..1\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    KTAP version 1\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    # Subtest: test_suite\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("    1..1\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_ktap_parse("        KTAP version 1\n", ktap), -EINPROGRESS);
	igt_assert(igt_list_empty(&results));

	igt_assert_eq(igt_ktap_parse("        # Subtest: test_case\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_list_length(&results), 1);

	igt_assert_eq(igt_ktap_parse("        ok 1 parameter # SKIP\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_list_length(&results), 1);

	igt_assert_eq(igt_ktap_parse("    ok 1 test_case\n", ktap), -EINPROGRESS);
	igt_assert_eq(igt_list_length(&results), 2);

	igt_assert_eq(igt_ktap_parse("not ok 1 test_suite\n", ktap), 0);
	igt_assert_eq(igt_list_length(&results), 2);

	igt_ktap_free(&ktap);

	result = igt_list_last_entry(&results, result, link);
	igt_list_del(&result->link);
	igt_assert_eq(result->code, IGT_EXIT_SUCCESS);
	free(result->msg);
	free(result);

	result = igt_list_last_entry(&results, result, link);
	igt_list_del(&result->link);
	free(result->suite_name);
	free(result->case_name);
	free(result->msg);
	free(result);
}

static void ktap_top_version(void)
{
	struct igt_ktap_results *ktap;
	IGT_LIST_HEAD(results);

	ktap = igt_ktap_alloc(&results);
	igt_require(ktap);
	igt_assert_eq(igt_ktap_parse("1..1\n", ktap), -EPROTO);
	igt_ktap_free(&ktap);

	ktap = igt_ktap_alloc(&results);
	igt_require(ktap);
	/* TODO: change to -EPROTO as soon as related workaround is dropped */
	igt_assert_eq(igt_ktap_parse("    KTAP version 1\n", ktap), -EINPROGRESS);
	igt_ktap_free(&ktap);

	ktap = igt_ktap_alloc(&results);
	igt_require(ktap);
	igt_assert_eq(igt_ktap_parse("    # Subtest: test_suite\n", ktap), -EPROTO);
	igt_ktap_free(&ktap);

	ktap = igt_ktap_alloc(&results);
	igt_require(ktap);
	igt_assert_eq(igt_ktap_parse("    1..1\n", ktap), -EPROTO);
	igt_ktap_free(&ktap);

	ktap = igt_ktap_alloc(&results);
	igt_require(ktap);
	igt_assert_eq(igt_ktap_parse("        KTAP version 1\n", ktap), -EPROTO);
	igt_ktap_free(&ktap);

	ktap = igt_ktap_alloc(&results);
	igt_require(ktap);
	igt_assert_eq(igt_ktap_parse("        # Subtest: test_case\n", ktap), -EPROTO);
	igt_ktap_free(&ktap);

	ktap = igt_ktap_alloc(&results);
	igt_require(ktap);
	igt_assert_eq(igt_ktap_parse("        ok 1 parameter 1\n", ktap), -EPROTO);
	igt_ktap_free(&ktap);

	ktap = igt_ktap_alloc(&results);
	igt_require(ktap);
	igt_assert_eq(igt_ktap_parse("    ok 1 test_case\n", ktap), -EPROTO);
	igt_ktap_free(&ktap);

	ktap = igt_ktap_alloc(&results);
	igt_require(ktap);
	igt_assert_eq(igt_ktap_parse("ok 1 test_suite\n", ktap), -EPROTO);
	igt_ktap_free(&ktap);
}

igt_main()
{
	igt_subtest("list")
		ktap_list();

	igt_subtest("results")
		ktap_results();

	igt_subtest("success")
		ktap_success();

	igt_subtest("top-ktap-version")
		ktap_top_version();
}
