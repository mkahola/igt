// SPDX-License-Identifier: MIT
/*
 * Copyright © 2024 Intel Corporation
 */

#include <stdbool.h>

#include "igt_core.h"
#include "igt_facts.h"

/* Tests are not defined here so we can keep most of the functions static */

int igt_simple_main()
{
	igt_info("Running igt_facts_test\n");

	igt_facts_test();
}
