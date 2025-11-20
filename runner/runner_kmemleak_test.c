// SPDX-License-Identifier: MIT
/*
 * Copyright © 2025 Intel Corporation
 */

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <zlib.h>

#include "igt.h"
#include "kmemleak.h"

const char *kmemleak_file_example =
"unreferenced object 0xffff888102a2e638 (size 80):\n"
" comm \"swapper/0\", pid 1, jiffies 4294672730\n"
" hex dump (first 32 bytes):\n"
"   00 00 00 00 00 00 00 00 0d 01 a2 00 00 00 00 00  ................\n"
"   f0 7c 03 00 00 c9 ff ff 00 00 00 00 00 00 00 00  .|..............\n"
" backtrace (crc 2df71a7e):\n"
"   [<ffffffff824cd71b>] kmemleak_alloc+0x4b/0x80\n"
"   [<ffffffff814e169b>] kmem_cache_alloc_noprof+0x2ab/0x370\n"
"   [<ffffffff81c2f4dc>] acpi_ps_alloc_op+0xdc/0xf0\n"
"   [<ffffffff81c2d650>] acpi_ps_create_op+0x1c0/0x400\n"
"   [<ffffffff81c2c8dc>] acpi_ps_parse_loop+0x16c/0xa60\n"
"   [<ffffffff81c2e94f>] acpi_ps_parse_aml+0x22f/0x5f0\n"
"   [<ffffffff81c2fa82>] acpi_ps_execute_method+0x152/0x380\n"
"   [<ffffffff81c233ed>] acpi_ns_evaluate+0x31d/0x5e0\n"
"   [<ffffffff81c2a606>] acpi_evaluate_object+0x206/0x490\n"
"   [<ffffffff81bf1202>] __acpi_power_off.isra.0+0x22/0x70\n"
"   [<ffffffff81bf275b>] acpi_turn_off_unused_power_resources+0xbb/0xf0\n"
"   [<ffffffff83867799>] acpi_scan_init+0x119/0x290\n"
"   [<ffffffff8386711a>] acpi_init+0x23a/0x590\n"
"   [<ffffffff81002c71>] do_one_initcall+0x61/0x3d0\n"
"   [<ffffffff837dce32>] kernel_init_freeable+0x3e2/0x680\n"
"   [<ffffffff824ca53b>] kernel_init+0x1b/0x170"
"unreferenced object 0xffff888102a2ed18 (size 80):\n"
" comm \"swapper/0\", pid 1, jiffies 4294672730\n"
" hex dump (first 32 bytes):\n"
"   38 e6 a2 02 81 88 ff ff 0d 11 2d 00 00 00 00 00  8.........-.....\n"
"   f2 7c 03 00 00 c9 ff ff 58 ea a2 02 81 88 ff ff  .|......X.......\n"
" backtrace (crc ec2a8bdc):\n"
"   [<ffffffff824cd71b>] kmemleak_alloc+0x4b/0x80\n"
"   [<ffffffff814e169b>] kmem_cache_alloc_noprof+0x2ab/0x370\n"
"   [<ffffffff81c2f4dc>] acpi_ps_alloc_op+0xdc/0xf0\n"
"   [<ffffffff81c2d650>] acpi_ps_create_op+0x1c0/0x400\n"
"   [<ffffffff81c2c8dc>] acpi_ps_parse_loop+0x16c/0xa60\n"
"   [<ffffffff81c2e94f>] acpi_ps_parse_aml+0x22f/0x5f0\n"
"   [<ffffffff81c2fa82>] acpi_ps_execute_method+0x152/0x380\n"
"   [<ffffffff81c233ed>] acpi_ns_evaluate+0x31d/0x5e0\n"
"   [<ffffffff81c2a606>] acpi_evaluate_object+0x206/0x490\n"
"   [<ffffffff81bf1202>] __acpi_power_off.isra.0+0x22/0x70\n"
"   [<ffffffff81bf275b>] acpi_turn_off_unused_power_resources+0xbb/0xf0\n"
"   [<ffffffff83867799>] acpi_scan_init+0x119/0x290\n"
"   [<ffffffff8386711a>] acpi_init+0x23a/0x590\n"
"   [<ffffffff81002c71>] do_one_initcall+0x61/0x3d0\n"
"   [<ffffffff837dce32>] kernel_init_freeable+0x3e2/0x680\n"
"   [<ffffffff824ca53b>] kernel_init+0x1b/0x170"
"unreferenced object 0xffff888102a2ea58 (size 80):\n"
" comm \"swapper/0\", pid 1, jiffies 4294672730\n"
" hex dump (first 32 bytes):\n"
"   38 e6 a2 02 81 88 ff ff 0d 01 a0 00 00 00 00 00  8...............\n"
"   f6 7c 03 00 00 c9 ff ff 00 00 00 00 00 00 00 00  .|..............\n"
" backtrace (crc f911c0d1):\n"
"   [<ffffffff824cd71b>] kmemleak_alloc+0x4b/0x80\n"
"   [<ffffffff814e169b>] kmem_cache_alloc_noprof+0x2ab/0x370\n"
"   [<ffffffff81c2f4dc>] acpi_ps_alloc_op+0xdc/0xf0\n"
"   [<ffffffff81c2d650>] acpi_ps_create_op+0x1c0/0x400\n"
"   [<ffffffff81c2c8dc>] acpi_ps_parse_loop+0x16c/0xa60\n"
"   [<ffffffff81c2e94f>] acpi_ps_parse_aml+0x22f/0x5f0\n"
"   [<ffffffff81c2fa82>] acpi_ps_execute_method+0x152/0x380\n"
"   [<ffffffff81c233ed>] acpi_ns_evaluate+0x31d/0x5e0\n"
"   [<ffffffff81c2a606>] acpi_evaluate_object+0x206/0x490\n"
"   [<ffffffff81bf1202>] __acpi_power_off.isra.0+0x22/0x70\n"
"   [<ffffffff81bf275b>] acpi_turn_off_unused_power_resources+0xbb/0xf0\n"
"   [<ffffffff83867799>] acpi_scan_init+0x119/0x290\n"
"   [<ffffffff8386711a>] acpi_init+0x23a/0x590\n"
"   [<ffffffff81002c71>] do_one_initcall+0x61/0x3d0\n"
"   [<ffffffff837dce32>] kernel_init_freeable+0x3e2/0x680\n"
"   [<ffffffff824ca53b>] kernel_init+0x1b/0x170"
"unreferenced object 0xffff888102a2e428 (size 80):\n"
" comm \"swapper/0\", pid 1, jiffies 4294672730\n"
" hex dump (first 32 bytes):\n"
"   58 ea a2 02 81 88 ff ff 0d 01 35 00 00 00 00 00  X.........5.....\n"
"   fc 7c 03 00 00 c9 ff ff 00 00 00 00 00 00 00 00  .|..............\n"
" backtrace (crc cb8aaffd):\n"
"   [<ffffffff824cd71b>] kmemleak_alloc+0x4b/0x80\n"
"   [<ffffffff814e169b>] kmem_cache_alloc_noprof+0x2ab/0x370\n"
"   [<ffffffff81c2f4dc>] acpi_ps_alloc_op+0xdc/0xf0\n"
"   [<ffffffff81c2d650>] acpi_ps_create_op+0x1c0/0x400\n"
"   [<ffffffff81c2c8dc>] acpi_ps_parse_loop+0x16c/0xa60\n"
"   [<ffffffff81c2e94f>] acpi_ps_parse_aml+0x22f/0x5f0\n"
"   [<ffffffff81c2fa82>] acpi_ps_execute_method+0x152/0x380\n"
"   [<ffffffff81c233ed>] acpi_ns_evaluate+0x31d/0x5e0\n"
"   [<ffffffff81c2a606>] acpi_evaluate_object+0x206/0x490\n"
"   [<ffffffff81bf1202>] __acpi_power_off.isra.0+0x22/0x70\n"
"   [<ffffffff81bf275b>] acpi_turn_off_unused_power_resources+0xbb/0xf0\n"
"   [<ffffffff83867799>] acpi_scan_init+0x119/0x290\n"
"   [<ffffffff8386711a>] acpi_init+0x23a/0x590\n"
"   [<ffffffff81002c71>] do_one_initcall+0x61/0x3d0\n"
"   [<ffffffff837dce32>] kernel_init_freeable+0x3e2/0x680\n"
"   [<ffffffff824ca53b>] kernel_init+0x1b/0x170"
"unreferenced object 0xffff888102a2e008 (size 80):\n"
" comm \"swapper/0\", pid 1, jiffies 4294672730\n"
" hex dump (first 32 bytes):\n"
"   28 e4 a2 02 81 88 ff ff 0d 01 2d 00 00 00 00 00  (.........-.....\n"
"   fc 7c 03 00 00 c9 ff ff c8 e2 a2 02 81 88 ff ff  .|..............\n"
" backtrace (crc 7f883e78):\n"
"   [<ffffffff824cd71b>] kmemleak_alloc+0x4b/0x80\n"
"   [<ffffffff814e169b>] kmem_cache_alloc_noprof+0x2ab/0x370\n"
"   [<ffffffff81c2f4dc>] acpi_ps_alloc_op+0xdc/0xf0\n"
"   [<ffffffff81c2b9e5>] acpi_ps_get_next_namepath+0x1f5/0x390\n"
"   [<ffffffff81c2cc15>] acpi_ps_parse_loop+0x4a5/0xa60\n"
"   [<ffffffff81c2e94f>] acpi_ps_parse_aml+0x22f/0x5f0\n"
"   [<ffffffff81c2fa82>] acpi_ps_execute_method+0x152/0x380\n"
"   [<ffffffff81c233ed>] acpi_ns_evaluate+0x31d/0x5e0\n"
"   [<ffffffff81c2a606>] acpi_evaluate_object+0x206/0x490\n"
"   [<ffffffff81bf1202>] __acpi_power_off.isra.0+0x22/0x70\n"
"   [<ffffffff81bf275b>] acpi_turn_off_unused_power_resources+0xbb/0xf0\n"
"   [<ffffffff83867799>] acpi_scan_init+0x119/0x290\n"
"   [<ffffffff8386711a>] acpi_init+0x23a/0x590\n"
"   [<ffffffff81002c71>] do_one_initcall+0x61/0x3d0\n"
"   [<ffffffff837dce32>] kernel_init_freeable+0x3e2/0x680\n"
"   [<ffffffff824ca53b>] kernel_init+0x1b/0x170"
"unreferenced object 0xffff888102a2e2c8 (size 80):\n"
" comm \"swapper/0\", pid 1, jiffies 4294672730\n"
" hex dump (first 32 bytes):\n"
"   28 e4 a2 02 81 88 ff ff 0d 01 73 00 00 00 00 00  (.........s.....\n"
"   00 7d 03 00 00 c9 ff ff 00 00 00 00 00 00 00 00  .}..............\n"
" backtrace (crc 338c016):\n"
"   [<ffffffff824cd71b>] kmemleak_alloc+0x4b/0x80\n"
"   [<ffffffff814e169b>] kmem_cache_alloc_noprof+0x2ab/0x370\n"
"   [<ffffffff81c2f4dc>] acpi_ps_alloc_op+0xdc/0xf0\n"
"   [<ffffffff81c2d650>] acpi_ps_create_op+0x1c0/0x400\n"
"   [<ffffffff81c2c8dc>] acpi_ps_parse_loop+0x16c/0xa60\n"
"   [<ffffffff81c2e94f>] acpi_ps_parse_aml+0x22f/0x5f0\n"
"   [<ffffffff81c2fa82>] acpi_ps_execute_method+0x152/0x380\n"
"   [<ffffffff81c233ed>] acpi_ns_evaluate+0x31d/0x5e0\n"
"   [<ffffffff81c2a606>] acpi_evaluate_object+0x206/0x490\n"
"   [<ffffffff81bf1202>] __acpi_power_off.isra.0+0x22/0x70\n"
"   [<ffffffff81bf275b>] acpi_turn_off_unused_power_resources+0xbb/0xf0\n"
"   [<ffffffff83867799>] acpi_scan_init+0x119/0x290\n"
"   [<ffffffff8386711a>] acpi_init+0x23a/0x590\n"
"   [<ffffffff81002c71>] do_one_initcall+0x61/0x3d0\n"
"   [<ffffffff837dce32>] kernel_init_freeable+0x3e2/0x680\n"
"   [<ffffffff824ca53b>] kernel_init+0x1b/0x170"
"unreferenced object 0xffff888102a2e378 (size 80):\n"
" comm \"swapper/0\", pid 1, jiffies 4294672730\n"
" hex dump (first 32 bytes):\n"
"   c8 e2 a2 02 81 88 ff ff 0d 01 0d 00 00 00 00 00  ................\n"
"   01 7d 03 00 00 c9 ff ff 98 e7 a2 02 81 88 ff ff  .}..............\n"
" backtrace (crc 665fb8a7):\n"
"   [<ffffffff824cd71b>] kmemleak_alloc+0x4b/0x80\n"
"   [<ffffffff814e169b>] kmem_cache_alloc_noprof+0x2ab/0x370\n"
"   [<ffffffff81c2f4dc>] acpi_ps_alloc_op+0xdc/0xf0\n"
"   [<ffffffff81c2d650>] acpi_ps_create_op+0x1c0/0x400\n"
"   [<ffffffff81c2c8dc>] acpi_ps_parse_loop+0x16c/0xa60\n"
"   [<ffffffff81c2e94f>] acpi_ps_parse_aml+0x22f/0x5f0\n"
"   [<ffffffff81c2fa82>] acpi_ps_execute_method+0x152/0x380\n"
"   [<ffffffff81c233ed>] acpi_ns_evaluate+0x31d/0x5e0\n"
"   [<ffffffff81c2a606>] acpi_evaluate_object+0x206/0x490\n"
"   [<ffffffff81bf1202>] __acpi_power_off.isra.0+0x22/0x70\n"
"   [<ffffffff81bf275b>] acpi_turn_off_unused_power_resources+0xbb/0xf0\n"
"   [<ffffffff83867799>] acpi_scan_init+0x119/0x290\n"
"   [<ffffffff8386711a>] acpi_init+0x23a/0x590\n"
"   [<ffffffff81002c71>] do_one_initcall+0x61/0x3d0\n"
"   [<ffffffff837dce32>] kernel_init_freeable+0x3e2/0x680\n"
"   [<ffffffff824ca53b>] kernel_init+0x1b/0x170"
"unreferenced object 0xffff888102a2e798 (size 80):\n"
" comm \"swapper/0\", pid 1, jiffies 4294672730\n"
" hex dump (first 32 bytes):\n"
"   7c8 e2 a2 02 81 88 ff ff 0d 01 98 00 00 00 00 00  ................\n"
"   1b 7d 03 00 00 c9 ff ff 00 00 00 00 00 00 00 00  .}..............\n"
" backtrace (crc b7a23a1c):\n"
"   [<ffffffff824cd71b>] kmemleak_alloc+0x4b/0x80\n"
"   [<ffffffff814e169b>] kmem_cache_alloc_noprof+0x2ab/0x370\n"
"   [<ffffffff81c2f4dc>] acpi_ps_alloc_op+0xdc/0xf0\n"
"   [<ffffffff81c2d650>] acpi_ps_create_op+0x1c0/0x400\n"
"   [<ffffffff81c2c8dc>] acpi_ps_parse_loop+0x16c/0xa60\n"
"   [<ffffffff81c2e94f>] acpi_ps_parse_aml+0x22f/0x5f0\n"
"   [<ffffffff81c2fa82>] acpi_ps_execute_method+0x152/0x380\n"
"   [<ffffffff81c233ed>] acpi_ns_evaluate+0x31d/0x5e0\n"
"   [<ffffffff81c2a606>] acpi_evaluate_object+0x206/0x490\n"
"   [<ffffffff81bf1202>] __acpi_power_off.isra.0+0x22/0x70\n"
"   [<ffffffff81bf275b>] acpi_turn_off_unused_power_resources+0xbb/0xf0\n"
"   [<ffffffff83867799>] acpi_scan_init+0x119/0x290\n"
"   [<ffffffff8386711a>] acpi_init+0x23a/0x590\n"
"   [<ffffffff81002c71>] do_one_initcall+0x61/0x3d0\n"
"   [<ffffffff837dce32>] kernel_init_freeable+0x3e2/0x680\n"
"   [<ffffffff824ca53b>] kernel_init+0x1b/0x170"
"unreferenced object 0xffff888102a2e0b8 (size 80):\n"
" comm \"swapper/0\", pid 1, jiffies 4294672730\n"
" hex dump (first 32 bytes):\n"
"   98 e7 a2 02 81 88 ff ff 0d 01 2d 00 00 00 00 00  ..........-.....\n"
"   1c 7d 03 00 00 c9 ff ff 00 00 00 00 00 00 00 00  .}..............\n"
" backtrace (crc 14d67a9c):\n"
"   [<ffffffff824cd71b>] kmemleak_alloc+0x4b/0x80\n"
"   [<ffffffff814e169b>] kmem_cache_alloc_noprof+0x2ab/0x370\n"
"   [<ffffffff81c2f4dc>] acpi_ps_alloc_op+0xdc/0xf0\n"
"   [<ffffffff81c2d650>] acpi_ps_create_op+0x1c0/0x400\n"
"   [<ffffffff81c2c8dc>] acpi_ps_parse_loop+0x16c/0xa60\n"
"   [<ffffffff81c2e94f>] acpi_ps_parse_aml+0x22f/0x5f0\n"
"   [<ffffffff81c2fa82>] acpi_ps_execute_method+0x152/0x380\n"
"   [<ffffffff81c233ed>] acpi_ns_evaluate+0x31d/0x5e0\n"
"   [<ffffffff81c2a606>] acpi_evaluate_object+0x206/0x490\n"
"   [<ffffffff81bf1202>] __acpi_power_off.isra.0+0x22/0x70\n"
"   [<ffffffff81bf275b>] acpi_turn_off_unused_power_resources+0xbb/0xf0\n"
"   [<ffffffff83867799>] acpi_scan_init+0x119/0x290\n"
"   [<ffffffff8386711a>] acpi_init+0x23a/0x590\n"
"   [<ffffffff81002c71>] do_one_initcall+0x61/0x3d0\n"
"   [<ffffffff837dce32>] kernel_init_freeable+0x3e2/0x680\n"
"   [<ffffffff824ca53b>] kernel_init+0x1b/0x170\n";

static const char *runner_kmemleak_unit_testing_resdir = "/tmp";

igt_main()
{
	char unit_testing_kmemleak_filepath[256] = "/tmp/runner_kmemleak_test_XXXXXX";
	int written_bytes;
	int resdirfd;
	int fd;

	igt_fixture() {
		/* resdirfd is used by runner_kmemleak() to store results */
		igt_assert(resdirfd = open(runner_kmemleak_unit_testing_resdir,
					   O_DIRECTORY | O_RDONLY));

		/* Try to delete results file in case of leftovers,
		 * ignoring errors as the file may not exist
		 */
		unlinkat(resdirfd, KMEMLEAK_RESFILENAME, 0);

		/* Creating a fake kmemleak file for unit testing */
		fd = mkstemp(unit_testing_kmemleak_filepath);
		igt_assert(fd >= 0);

		written_bytes = write(fd, kmemleak_file_example,
				      strlen(kmemleak_file_example));
		igt_assert_eq(written_bytes, strlen(kmemleak_file_example));

		close(fd);

		/* Initializing runner_kmemleak with a fake kmemleak file
		 * for unit testing
		 */
		igt_assert(runner_kmemleak_init(unit_testing_kmemleak_filepath));
	}

	igt_subtest_group() {
		igt_subtest("test_runner_kmemleak_once")
			igt_assert(runner_kmemleak(NULL, resdirfd, false, true));

		igt_subtest("test_runner_kmemleak_each") {
			igt_assert(runner_kmemleak("test_name_1", resdirfd,
						   true, false));
			igt_assert(runner_kmemleak("test_name_2", resdirfd,
						   true, true));
			igt_assert(runner_kmemleak("test_name_3", resdirfd,
						   true, false));
		}
		igt_fixture() {
			close(resdirfd);
		}
	}
	igt_fixture()
		unlinkat(resdirfd, KMEMLEAK_RESFILENAME, 0);
}
