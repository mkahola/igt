#include "igt.h"
#include "igt_kmod.h"

/**
 * TEST: Xe driver live kunit tests
 * Description: Xe driver live dmabuf unit tests
 * Category: Core
 * Mega feature: General Core features
 * Sub-category: kunit
 * Functionality: kunit test
 * Test category: functionality test
 *
 * SUBTEST: xe_bo
 * Description:
 *	Kernel dynamic selftests to check if GPU buffer objects are
 *	being handled properly.
 * Functionality: bo
 *
 * SUBTEST: xe_bo_shrink
 * Description:
 *	Kernel dynamic selftests to check BO swap path and shrinking path.
 * Functionality: bo
 *
 * SUBTEST: xe_dma_buf
 * Description: Kernel dynamic selftests for dmabuf functionality.
 * Functionality: dmabuf test
 *
 * SUBTEST: xe_migrate
 * Description:
 *	Kernel dynamic selftests to check if page table migrations
 *	are working properly.
 * Functionality: migrate
 *
 * SUBTEST: xe_mocs
 * Description:
 *	Kernel dynamic selftests to check mocs configuration.
 * Functionality: mocs configuration
 *
 * SUBTEST: xe_eudebug
 * Description:
 *	Kernel dynamic selftests to check eudebug functionality.
 * Functionality: eudebug kunit
 * Mega feature: EUdebug
 * Sub-category: kunit eudebug
 */

static const char *live_tests[] = {
	"xe_bo",
	"xe_bo_shrink",
	"xe_dma_buf",
	"xe_migrate",
	"xe_mocs",
	"xe_eudebug",
};

int igt_main()
{
	int i;

	for (i = 0; i < ARRAY_SIZE(live_tests); i++)
		igt_kunit("xe_live_test", live_tests[i], NULL);
}
