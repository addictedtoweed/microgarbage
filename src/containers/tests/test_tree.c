/* Tests for tree (BST/AVL over a node pool, shared in-order walk).
 * Public domain (CC0). No warranty. */

#include "test_runner.h"
#include "containers/tree.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

/* ---- test-side structural validation (peeks at node layout) ----
 * We re-derive node access from the public TREE_NODE_OVERHEAD so the
 * test can verify the AVL height invariant independently of the
 * implementation's internal helpers. Layout: left,right,parent (u32),
 * balance (i8). */
static uint32_t t_left(const Tree *t, uint32_t i) {
    return *(const uint32_t *)(const void *)(t->pool + (size_t)i * t->stride);
}
static uint32_t t_right(const Tree *t, uint32_t i) {
    return *(const uint32_t *)(const void *)(t->pool + (size_t)i * t->stride + 4);
}

/* recompute true height; returns height and asserts |balance| <= 1 at
 * every node for AVL trees. */
static int check_height(const Tree *t, uint32_t n, int *ok) {
    if (n == TREE_NIL) return 0;
    int lh = check_height(t, t_left(t, n), ok);
    int rh = check_height(t, t_right(t, n), ok);
    int bf = rh - lh;
    if (bf < -1 || bf > 1) *ok = 0;
    return (lh > rh ? lh : rh) + 1;
}

/* ============================================================
 *  Sizing
 * ============================================================ */

static void test_pool_bytes_macro_matches_function(void) {
    ASSERT_EQ_INT((int)TREE_POOL_BYTES(64, sizeof(int)),
                  (int)tree_pool_bytes(64, sizeof(int)));
}

static void test_depth_sizing_is_full_tree_bound(void) {
    /* depth 4 -> full tree = 2^4 - 1 = 15 nodes */
    ASSERT_EQ_INT((int)tree_pool_bytes(15, sizeof(int)),
                  (int)tree_pool_bytes_for_depth(4, sizeof(int)));
    ASSERT_EQ_INT(0, (int)tree_pool_bytes_for_depth(0, sizeof(int)));
}

static void test_init_rejects_bad_args(void) {
    uint8_t pool[TREE_POOL_BYTES(8, sizeof(int))];
    Tree t;
    ASSERT(!tree_init(NULL, TREE_BST, cmp_int, pool, sizeof pool, sizeof(int)));
    ASSERT(!tree_init(&t, TREE_BST, NULL, pool, sizeof pool, sizeof(int)));
    ASSERT(!tree_init(&t, TREE_BST, cmp_int, NULL, sizeof pool, sizeof(int)));
    ASSERT(!tree_init(&t, TREE_BST, cmp_int, pool, sizeof pool, 0));
    ASSERT(!tree_init(&t, (TreeDiscipline)7, cmp_int, pool, sizeof pool, sizeof(int)));
}

/* ============================================================
 *  Insert / find / contains  (both disciplines)
 * ============================================================ */

static void insert_find_suite(TreeDiscipline disc) {
    uint8_t pool[TREE_POOL_BYTES(64, sizeof(int))];
    Tree t;
    ASSERT(tree_init(&t, disc, cmp_int, pool, sizeof pool, sizeof(int)));

    int vals[] = { 50, 30, 70, 20, 40, 60, 80, 10 };
    int n = (int)(sizeof vals / sizeof vals[0]);
    for (int i = 0; i < n; i++) ASSERT(tree_insert(&t, &vals[i]));
    ASSERT_EQ_INT(n, (int)tree_count(&t));

    for (int i = 0; i < n; i++) {
        int out = -1;
        ASSERT(tree_find(&t, &vals[i], &out));
        ASSERT_EQ_INT(vals[i], out);
        ASSERT(tree_contains(&t, &vals[i]));
    }
    int missing = 999;
    ASSERT(!tree_find(&t, &missing, NULL));
    ASSERT(!tree_contains(&t, &missing));

    /* duplicate rejected (set semantics) */
    int dup = 50;
    ASSERT(!tree_insert(&t, &dup));
    ASSERT_EQ_INT(n, (int)tree_count(&t));
}

static void test_bst_insert_find(void) { insert_find_suite(TREE_BST); }
static void test_avl_insert_find(void) { insert_find_suite(TREE_AVL); }

/* ============================================================
 *  Shared in-order walk yields SORTED order (both disciplines)
 * ============================================================ */

static void walk_sorted_suite(TreeDiscipline disc) {
    uint8_t pool[TREE_POOL_BYTES(64, sizeof(int))];
    Tree t;
    tree_init(&t, disc, cmp_int, pool, sizeof pool, sizeof(int));

    int vals[] = { 5, 1, 9, 3, 7, 2, 8, 4, 6, 0 };
    int n = (int)(sizeof vals / sizeof vals[0]);
    for (int i = 0; i < n; i++) tree_insert(&t, &vals[i]);

    /* forward = ascending */
    int prev = -1, seen = 0;
    for (uint32_t it = tree_begin(&t); it != TREE_NIL; it = tree_next(&t, it)) {
        int v; tree_get(&t, it, &v);
        ASSERT(v > prev);
        prev = v; seen++;
    }
    ASSERT_EQ_INT(n, seen);

    /* reverse = descending */
    prev = 1000; seen = 0;
    for (uint32_t it = tree_rbegin(&t); it != TREE_NIL; it = tree_prev(&t, it)) {
        int v; tree_get(&t, it, &v);
        ASSERT(v < prev);
        prev = v; seen++;
    }
    ASSERT_EQ_INT(n, seen);
}

static void test_bst_walk_sorted(void) { walk_sorted_suite(TREE_BST); }
static void test_avl_walk_sorted(void) { walk_sorted_suite(TREE_AVL); }

/* ============================================================
 *  AVL invariant: stays balanced even on SORTED insertion
 *  (the case where a plain BST degenerates to a list)
 * ============================================================ */

static void test_avl_balanced_on_sorted_insert(void) {
    enum { N = 1000 };
    static uint8_t pool[TREE_POOL_BYTES(N, sizeof(int))];
    Tree t;
    ASSERT(tree_init(&t, TREE_AVL, cmp_int, pool, sizeof pool, sizeof(int)));

    for (int i = 0; i < N; i++) ASSERT(tree_insert(&t, &i));  /* ascending */
    ASSERT_EQ_INT(N, (int)tree_count(&t));

    int ok = 1;
    int h = check_height(&t, t.root, &ok);
    ASSERT(ok);                 /* every |balance| <= 1 */
    /* balanced height for N nodes is ~1.44*log2(N); for 1000 that's
     * about 14. Assert it's far below the degenerate N. */
    ASSERT(h <= 20);

    /* walk is still sorted */
    int prev = -1, seen = 0;
    for (uint32_t it = tree_begin(&t); it != TREE_NIL; it = tree_next(&t, it)) {
        int v; tree_get(&t, it, &v);
        ASSERT(v > prev); prev = v; seen++;
    }
    ASSERT_EQ_INT(N, seen);
}

/* ============================================================
 *  Remove (both disciplines): leaf, one-child, two-children, root
 * ============================================================ */

static void remove_suite(TreeDiscipline disc) {
    uint8_t pool[TREE_POOL_BYTES(64, sizeof(int))];
    Tree t;
    tree_init(&t, disc, cmp_int, pool, sizeof pool, sizeof(int));

    int vals[] = { 50, 30, 70, 20, 40, 60, 80 };
    int n = (int)(sizeof vals / sizeof vals[0]);
    for (int i = 0; i < n; i++) tree_insert(&t, &vals[i]);

    /* remove a leaf */
    int k = 20, out = 0;
    ASSERT(tree_remove(&t, &k, &out));
    ASSERT_EQ_INT(20, out);
    ASSERT(!tree_contains(&t, &k));
    ASSERT_EQ_INT(n - 1, (int)tree_count(&t));

    /* remove a node with two children */
    k = 30;
    ASSERT(tree_remove(&t, &k, NULL));
    ASSERT(!tree_contains(&t, &k));

    /* remove the root */
    k = 50;
    ASSERT(tree_remove(&t, &k, NULL));
    ASSERT(!tree_contains(&t, &k));

    /* removing absent key fails */
    k = 999;
    ASSERT(!tree_remove(&t, &k, NULL));

    /* survivors still walkable + sorted */
    int prev = -1;
    for (uint32_t it = tree_begin(&t); it != TREE_NIL; it = tree_next(&t, it)) {
        int v; tree_get(&t, it, &v);
        ASSERT(v > prev); prev = v;
    }
}

static void test_bst_remove(void) { remove_suite(TREE_BST); }
static void test_avl_remove(void) { remove_suite(TREE_AVL); }

/* AVL stays balanced through heavy interleaved insert/remove. */
static void test_avl_balanced_under_churn(void) {
    enum { N = 500 };
    static uint8_t pool[TREE_POOL_BYTES(N, sizeof(int))];
    Tree t;
    tree_init(&t, TREE_AVL, cmp_int, pool, sizeof pool, sizeof(int));

    srand(12345);
    int present[N];
    for (int i = 0; i < N; i++) present[i] = 0;

    for (int iter = 0; iter < 20000; iter++) {
        int k = rand() % N;
        if (present[k]) {
            ASSERT(tree_remove(&t, &k, NULL));
            present[k] = 0;
        } else {
            ASSERT(tree_insert(&t, &k));
            present[k] = 1;
        }
        /* spot-check the invariant periodically (full check is O(n)) */
        if ((iter & 0x3FF) == 0) {
            int ok = 1;
            check_height(&t, t.root, &ok);
            ASSERT(ok);
        }
    }
    /* final: invariant holds and every present key is findable */
    int ok = 1; check_height(&t, t.root, &ok); ASSERT(ok);
    int count = 0;
    for (int i = 0; i < N; i++) {
        if (present[i]) { ASSERT(tree_contains(&t, &i)); count++; }
        else            { ASSERT(!tree_contains(&t, &i)); }
    }
    ASSERT_EQ_INT(count, (int)tree_count(&t));
}

/* ============================================================
 *  Pool exhaustion + clear
 * ============================================================ */

static void test_insert_fails_when_pool_full(void) {
    uint8_t pool[TREE_POOL_BYTES(4, sizeof(int))];
    Tree t;
    tree_init(&t, TREE_AVL, cmp_int, pool, sizeof pool, sizeof(int));
    for (int i = 0; i < 4; i++) ASSERT(tree_insert(&t, &i));
    ASSERT(tree_full(&t));
    int x = 99;
    ASSERT(!tree_insert(&t, &x));      /* exhausted */
    /* free one, then it fits again */
    int k = 0;
    ASSERT(tree_remove(&t, &k, NULL));
    ASSERT(tree_insert(&t, &x));
}

static void test_clear_resets(void) {
    uint8_t pool[TREE_POOL_BYTES(8, sizeof(int))];
    Tree t;
    tree_init(&t, TREE_BST, cmp_int, pool, sizeof pool, sizeof(int));
    for (int i = 0; i < 5; i++) tree_insert(&t, &i);
    tree_clear(&t);
    ASSERT(tree_empty(&t));
    ASSERT_EQ_INT(0, (int)tree_count(&t));
    int x = 7;
    ASSERT(tree_insert(&t, &x));       /* usable after clear */
}

int main(void) {
    RUN(test_pool_bytes_macro_matches_function);
    RUN(test_depth_sizing_is_full_tree_bound);
    RUN(test_init_rejects_bad_args);

    RUN(test_bst_insert_find);
    RUN(test_avl_insert_find);
    RUN(test_bst_walk_sorted);
    RUN(test_avl_walk_sorted);

    RUN(test_avl_balanced_on_sorted_insert);

    RUN(test_bst_remove);
    RUN(test_avl_remove);
    RUN(test_avl_balanced_under_churn);

    RUN(test_insert_fails_when_pool_full);
    RUN(test_clear_resets);

    return TEST_SUITE_RESULT();
}
