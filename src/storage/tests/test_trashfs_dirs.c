/* Tests for trashfs hierarchical directories: mkdir/rmdir, nested
 * paths, "." and "..", and path-aware open/unlink/opendir. */

#include "test_runner.h"
#include "storage/trashfs.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t g_region[64 * 1024];

static void fresh(TrashfsVolume *vol) {
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_format(g_region, sizeof(g_region), 0, 0));
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_mount(vol, g_region, sizeof(g_region)));
}

/* create a file at `path` and write `s` into it */
static void make_file(TrashfsVolume *vol, const char *path, const char *s) {
    TrashfsFile f;
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_open(vol, path, TRASHFS_O_CREAT | TRASHFS_O_TRUNC, &f));
    uint32_t w = 0;
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_write(&f, s, (uint32_t)strlen(s), &w, 0));
    ASSERT_EQ_INT((int)strlen(s), (int)w);
    trashfs_close(&f);
}

/* read a file at `path`, assert its contents == `s` */
static void check_file(TrashfsVolume *vol, const char *path, const char *s) {
    TrashfsFile f;
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_open(vol, path, TRASHFS_O_RDONLY, &f));
    char buf[128]; uint32_t got = 0;
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_read(&f, buf, sizeof buf, &got));
    ASSERT_EQ_INT((int)strlen(s), (int)got);
    ASSERT(memcmp(buf, s, got) == 0);
    trashfs_close(&f);
}

/* ============================================================
 *  mkdir + files in subdirs
 * ============================================================ */

static void test_mkdir_and_file_inside(void) {
    TrashfsVolume vol; fresh(&vol);

    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_mkdir(&vol, "/sub", 0));
    /* a file created inside the new dir is readable back by its path */
    make_file(&vol, "/sub/hello.txt", "hi there");
    check_file(&vol, "/sub/hello.txt", "hi there");
    /* same name at root is a DIFFERENT file (namespacing works) */
    make_file(&vol, "/hello.txt", "root-level");
    check_file(&vol, "/sub/hello.txt", "hi there");
    check_file(&vol, "/hello.txt", "root-level");
}

static void test_nested_mkdir(void) {
    TrashfsVolume vol; fresh(&vol);
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_mkdir(&vol, "/a", 0));
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_mkdir(&vol, "/a/b", 0));
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_mkdir(&vol, "/a/b/c", 0));
    make_file(&vol, "/a/b/c/deep.bin", "deep payload");
    check_file(&vol, "/a/b/c/deep.bin", "deep payload");
}

static void test_dot_and_dotdot(void) {
    TrashfsVolume vol; fresh(&vol);
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_mkdir(&vol, "/a", 0));
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_mkdir(&vol, "/a/b", 0));
    make_file(&vol, "/a/b/f", "data");

    check_file(&vol, "/a/./b/f", "data");        /* "." is a no-op */
    check_file(&vol, "/a/b/../b/f", "data");      /* ".." then back down */
    check_file(&vol, "/a/b/../../a/b/f", "data"); /* up to root, down again */
    /* ".." at root clamps to root */
    make_file(&vol, "/top", "T");
    check_file(&vol, "/../top", "T");
}

/* ============================================================
 *  mkdir error cases
 * ============================================================ */

static void test_mkdir_errors(void) {
    TrashfsVolume vol; fresh(&vol);
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_mkdir(&vol, "/d", 0));
    /* already exists */
    ASSERT_EQ_INT(TRASHFS_ERR_EXISTS, (int)trashfs_mkdir(&vol, "/d", 0));
    /* missing intermediate */
    ASSERT_EQ_INT(TRASHFS_ERR_NOT_FOUND, (int)trashfs_mkdir(&vol, "/nope/x", 0));
    /* intermediate is a file, not a dir */
    make_file(&vol, "/afile", "x");
    ASSERT_EQ_INT(TRASHFS_ERR_NOT_DIR, (int)trashfs_mkdir(&vol, "/afile/x", 0));
    /* root has no nameable leaf */
    ASSERT_EQ_INT(TRASHFS_ERR_INVALID_ARG, (int)trashfs_mkdir(&vol, "/", 0));
}

/* ============================================================
 *  opendir / readdir on subdirectories
 * ============================================================ */

static int count_entries(TrashfsVolume *vol, const char *path) {
    TrashfsDir d;
    if (trashfs_opendir(vol, path, &d) != TRASHFS_OK) return -1;
    int n = 0; bool have = false; TrashfsDirent_Out e;
    while (trashfs_readdir(&d, &e, &have) == TRASHFS_OK && have) n++;
    trashfs_closedir(&d);
    return n;
}

static void test_opendir_subdir(void) {
    TrashfsVolume vol; fresh(&vol);
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_mkdir(&vol, "/dir", 0));
    make_file(&vol, "/dir/a", "1");
    make_file(&vol, "/dir/b", "2");
    make_file(&vol, "/dir/c", "3");
    ASSERT_EQ_INT(3, count_entries(&vol, "/dir"));
    /* root holds just the one subdir */
    ASSERT_EQ_INT(1, count_entries(&vol, "/"));

    /* root entry reports DIR type */
    TrashfsDir d; ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_opendir(&vol, "/", &d));
    TrashfsDirent_Out e; bool have = false;
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_readdir(&d, &e, &have));
    ASSERT(have);
    ASSERT_EQ_INT(TRASHFS_TYPE_DIR, (int)e.type);
    ASSERT(strcmp(e.name, "dir") == 0);
    trashfs_closedir(&d);

    /* opendir on a file / missing path */
    ASSERT_EQ_INT(TRASHFS_ERR_NOT_DIR,   (int)trashfs_opendir(&vol, "/dir/a", &d));
    ASSERT_EQ_INT(TRASHFS_ERR_NOT_FOUND, (int)trashfs_opendir(&vol, "/dir/zzz", &d));
}

/* ============================================================
 *  rmdir + unlink semantics
 * ============================================================ */

static void test_rmdir(void) {
    TrashfsVolume vol; fresh(&vol);
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_mkdir(&vol, "/d", 0));
    make_file(&vol, "/d/f", "x");

    /* non-empty dir can't be removed */
    ASSERT_EQ_INT(TRASHFS_ERR_NOT_EMPTY, (int)trashfs_rmdir(&vol, "/d"));
    /* unlink refuses a directory; rmdir refuses a file */
    ASSERT_EQ_INT(TRASHFS_ERR_NOT_DIR, (int)trashfs_unlink(&vol, "/d"));
    ASSERT_EQ_INT(TRASHFS_ERR_NOT_DIR, (int)trashfs_rmdir(&vol, "/d/f"));
    /* root can't be removed */
    ASSERT_EQ_INT(TRASHFS_ERR_INVALID_ARG, (int)trashfs_rmdir(&vol, "/"));

    /* empty it, then rmdir works; afterwards it's gone */
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_unlink(&vol, "/d/f"));
    ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_rmdir(&vol, "/d"));
    TrashfsDir d;
    ASSERT_EQ_INT(TRASHFS_ERR_NOT_FOUND, (int)trashfs_opendir(&vol, "/d", &d));
    ASSERT_EQ_INT(TRASHFS_ERR_NOT_FOUND, (int)trashfs_rmdir(&vol, "/d"));
}

/* Reclamation: mkdir/rmdir churn must return inodes so we don't leak. */
static void test_rmdir_reclaims(void) {
    TrashfsVolume vol; fresh(&vol);
    uint32_t free_inodes0 = trashfs_free_inodes(&vol);
    for (int i = 0; i < 50; i++) {
        ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_mkdir(&vol, "/scratch", 0));
        ASSERT_EQ_INT(TRASHFS_OK, (int)trashfs_rmdir(&vol, "/scratch"));
    }
    ASSERT_EQ_INT((int)free_inodes0, (int)trashfs_free_inodes(&vol));
}

int main(void) {
    RUN(test_mkdir_and_file_inside);
    RUN(test_nested_mkdir);
    RUN(test_dot_and_dotdot);
    RUN(test_mkdir_errors);
    RUN(test_opendir_subdir);
    RUN(test_rmdir);
    RUN(test_rmdir_reclaims);
    return TEST_SUITE_RESULT();
}
