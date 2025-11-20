/*
 * Copyright © 2019 Intel Corporation
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
 */

#include "igt.h"
#include "igt_kmod.h"
/**
 * TEST: dmabuf
 * Description: Kernel selftests for the dmabuf API
 * Category: Core
 * Mega feature: General Core features
 * Functionality: drm_mm
 * Sub-category: Memory management tests
 * Feature: mapping, prime
 * Test category: GEM_Legacy
 *
 * SUBTEST: all-tests
 *
 * SUBTEST: all-tests@dma_fence
 *
 * SUBTEST: all-tests@sanitycheck
 */

IGT_TEST_DESCRIPTION("Kernel selftests for the dmabuf API");

static unsigned int bogomips(void)
{
	unsigned int bogomips, ret = 0;
	char *line = NULL;
	size_t size = 0;
	FILE *cpuinfo;

	cpuinfo = fopen("/proc/cpuinfo", "r");
	if (igt_debug_on(!cpuinfo))
		return UINT_MAX;

	while (getline(&line, &size, cpuinfo) != -1) {
		char *colon;

		if (strncmp(line, "bogomips", 8))
			continue;

		colon = strchr(line, ':');
		if (igt_debug_on(!colon))
			bogomips = 0;
		else
			bogomips = atoi(colon + 1);

		if (igt_debug_on(!bogomips))
			break;

		ret += bogomips;
	}
	free(line);
	fclose(cpuinfo);

	return igt_debug_on(!bogomips) ? UINT_MAX : ret;
}

static int wrapper(const char *dynamic_name,
		   struct igt_ktest *tst,
		   struct igt_kselftest_list *tl)
{
	/*
	 * Test case wait-backward of dma_fence_chain selftest can trigger soft
	 * lockups on slow machines.  Since that slowness is not recognized as
	 * a bug on the kernel side, the issue is not going to be fixed.  Based
	 * on analysis of CI results, skip that selftest on machines slower than
	 * 25000 BogoMIPS to avoid ever returning CI reports on that failure.
	 */
	igt_skip_on(!strcmp(dynamic_name, "dma_fence_chain") && bogomips() < 25000);

	return igt_kselftest_execute(tst, tl, NULL, NULL);
}

int igt_main()
{
	igt_kselftests("dmabuf_selftests", NULL, NULL, NULL, wrapper);
}
