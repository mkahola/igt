#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <jansson.h>

#include "igt.h"
#include "resultgen.h"

static char testdatadir[] = JSON_TESTS_DIRECTORY;

static void compare(struct json_t *one,
		    struct json_t *two);

static void compare_objects(struct json_t *one, struct json_t *two)
{
	const char *key;
	json_t *one_value, *two_value;

	json_object_foreach(one, key, one_value) {
		igt_debug("Key %s\n", key);
		igt_assert(two_value = json_object_get(two, key));
		compare(one_value, two_value);
	}
}

static void compare_arrays(struct json_t *one, struct json_t *two)
{
	for (size_t i = 0; i < json_array_size(one); i++) {
		igt_debug("Array index %zd\n", i);
		compare(json_array_get(one, i),
			json_array_get(two, i));
	}
}

static bool compatible_types(struct json_t *one, struct json_t *two)
{
	/*
	 * Numbers should be compatible with each other. A double of
	 * value 0.0 gets written as "0", which gets read as an int.
	 */
	if (json_is_number(one))
		return json_is_number(two);

	if (json_is_boolean(one))
		return json_is_boolean(two);

	return json_typeof(one) == json_typeof(two);
}

static void compare(struct json_t *one,
		    struct json_t *two)
{
	igt_assert(compatible_types(one, two));

	switch (json_typeof(one)) {
	case JSON_NULL:
	case JSON_TRUE:
	case JSON_FALSE:
		/* These values are singletons: */
		igt_assert(one == two);
		break;
	case JSON_REAL:
	case JSON_INTEGER:
		/*
		 * A double of value 0.0 gets written as "0", which
		 * gets read as an int. Both yield 0.0 with
		 * json_object_get_double(). Comparing doubles with ==
		 * considered crazy but it's good enough.
		 */
		igt_assert_eq_double(json_number_value(one), json_number_value(two));
		break;
	case JSON_STRING:
		igt_assert(!strcmp(json_string_value(one), json_string_value(two)));
		break;
	case JSON_OBJECT:
		igt_assert_eq(json_object_size(one), json_object_size(two));
		compare_objects(one, two);
		break;
	case JSON_ARRAY:
		igt_assert_eq(json_array_size(one), json_array_size(two));
		compare_arrays(one, two);
		break;
	default:
		igt_assert(!"Cannot be reached");
	}
}

static void run_results_and_compare(int dirfd, const char *dirname)
{
	int testdirfd = openat(dirfd, dirname, O_RDONLY | O_DIRECTORY);
	int reference;
	struct json_t *resultsobj, *referenceobj;
	struct json_error_t error;

	igt_assert_fd(testdirfd);

	igt_assert((resultsobj = generate_results_json(testdirfd)) != NULL);

	reference = openat(testdirfd, "reference.json", O_RDONLY);
	close(testdirfd);

	igt_assert_fd(reference);
	referenceobj = json_loadfd(reference, 0, &error);
	close(reference);
	igt_assert_f(referenceobj, "JSON loading error for %s/%s:%d:%d - %s\n",
		     dirname, "reference.json", error.line, error.column, error.text);

	igt_debug("Root object\n");
	compare(resultsobj, referenceobj);
	json_decref(resultsobj);
	json_decref(referenceobj);
}

static const char *dirnames[] = {
	"normal-run",
	"warnings",
	"warnings-with-dmesg-warns",
	"piglit-style-dmesg",
	"incomplete-before-any-subtests",
	"dmesg-results",
	"aborted-on-boot",
	"aborted-after-a-test",
	"dmesg-escapes",
	"notrun-results",
	"notrun-results-multiple-mode",
	"dmesg-warn-level",
	"dmesg-warn-level-piglit-style",
	"dmesg-warn-level-one-piglit-style",
	"dynamic-subtests-keep-dynamic",
	"dynamic-subtests-keep-subtests",
	"dynamic-subtests-keep-all",
	"dynamic-subtests-keep-requested",
	"dynamic-subtest-name-in-multiple-subtests",
	"unprintable-characters",
	"empty-result-files",
	"graceful-notrun",
};

int igt_main()
{
	int dirfd = open(testdatadir, O_RDONLY | O_DIRECTORY);
	size_t i;

	igt_assert_fd(dirfd);

	for (i = 0; i < ARRAY_SIZE(dirnames); i++) {
		igt_subtest(dirnames[i]) {
			run_results_and_compare(dirfd, dirnames[i]);
		}
	}
}
