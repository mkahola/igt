/* SPDX-License-Identifier: MIT */

#ifndef _IGT_ANDROID_GLIB_H
#define _IGT_ANDROID_GLIB_H

#include <stdbool.h>
#include <stdlib.h>

#define G_KEY_FILE_NONE 0
#define G_REGEX_OPTIMIZE 0
#define GINT_TO_POINTER(i) ((void *)(signed long)(i))

typedef struct _GError {
	int code;
	char *message;
} GError;

typedef struct _GKeyFile {
	char *key;
	char *value;
} GKeyFile;

typedef struct _GMatchInfo {
	int dummy;
} GMatchInfo;

typedef struct _GHashTable {
  unsigned int hash_func;
  bool key_equal_func;
} GHashTable;

typedef int gint;
typedef size_t gsize;
typedef char gchar;
typedef unsigned char guchar;
typedef void GRegex;
typedef int GRegexMatchFlags;
typedef bool gboolean;
typedef unsigned int guint;
typedef guint (*GHashFunc)(const void *key);
typedef gboolean (*GEqualFunc)(const void *a, const void *b);

static inline void g_clear_error(GError **error) { }
static inline void g_error_free(GError *error) { }

static inline const char *g_get_home_dir(void) { return "/data/local/tmp/igt"; }

static inline void g_key_file_free(GKeyFile *file) { }
static inline GKeyFile *g_key_file_new(void) { return NULL; }
static inline int g_key_file_get_integer(GKeyFile *key_file,
	const char *group_name, const char *key, GError **error) { return 0; }
static inline char *g_key_file_get_string(GKeyFile *key_file,
	const char *group_name, const char *key, GError **error) { return NULL; }
static inline bool g_key_file_load_from_file(GKeyFile *key_file,
	const char *file, int flags, GError **error) { return false; }

static inline GRegex *g_regex_new(const char *pattern, int compile_options,
	int match_options, GError **error) { return NULL; }
static inline void g_regex_unref(GRegex *pattern) { };
static inline gboolean g_regex_match(GRegex *regex, const gchar *string,
	GRegexMatchFlags match_options, GMatchInfo **match_info) { return false; }

static gchar *g_base64_encode(const guchar *data, gsize len) { return NULL; }
static guint g_direct_hash(const void *v) { return 0; }
static gboolean g_direct_equal(const void *v1, const void *v2) { return 0; }
static inline GHashTable *g_hash_table_new(GHashFunc hash_func,
                                           GEqualFunc key_equal_func) {
  return NULL;
}
static inline void g_hash_table_destroy(GHashTable *hash_table) { }
static inline gboolean g_hash_table_add(GHashTable *hash_table, void *key) {
  return false;
}
static inline gboolean g_hash_table_contains(GHashTable *hash_table, const void *key) {
  return false;
}

#endif /* _IGT_ANDROID_GLIB_H */
