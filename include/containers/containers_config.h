/* ============================================================
 *  containers_config.h — default node-pool sizes for the
 *  pool-backed containers (slist, dlist, tree, avlhash).
 *
 *  These containers are caller-provides-the-pool: you hand them a
 *  byte buffer and they compute capacity from it (no malloc). The
 *  knobs here are DEFAULT node counts, used by the convenience
 *  `*_DEFAULT_POOL_BYTES(elem)` macros in each container header so
 *  a target can size its standard pools in one place.
 *
 *  Override exactly like config.h's GARBAGE_SCHED_MODE: define the
 *  knob before this header is reached — via a build flag
 *  (-DGARBAGE_SLIST_DEFAULT_NODES=128) or a host config that's
 *  included first — and the #ifndef guard leaves your value alone.
 *  Editing the defaults here for your target is also fine.
 *
 *  Node overhead per type is FIXED by the data structure and stays
 *  lean (it is NOT unified): slist 4 B (next), dlist 8 B (next+prev),
 *  tree/avlhash 16 B (left+right+parent+balance). Only the default
 *  node COUNT is configured here.
 *
 *  This header is integers only — no dependencies — so it is safe to
 *  pull into config.h and into any container header.
 *
 *  Public domain (CC0). No warranty.
 *  https://creativecommons.org/publicdomain/zero/1.0/
 * ============================================================ */

#ifndef GARBAGE_CONTAINERS_CONFIG_H
#define GARBAGE_CONTAINERS_CONFIG_H

/* Singly-linked list: default number of nodes in a standard pool. */
#ifndef GARBAGE_SLIST_DEFAULT_NODES
#define GARBAGE_SLIST_DEFAULT_NODES  32u
#endif

/* Doubly-linked list. */
#ifndef GARBAGE_DLIST_DEFAULT_NODES
#define GARBAGE_DLIST_DEFAULT_NODES  32u
#endif

/* Ordered tree (BST/AVL): default number of nodes. */
#ifndef GARBAGE_TREE_DEFAULT_NODES
#define GARBAGE_TREE_DEFAULT_NODES   32u
#endif

/* AVL-bucketed hash: default number of buckets (the bucket-root
 * array length). Collisions extend a bucket's AVL tree, so this need
 * not track the entry count closely — pick for hash spread. */
#ifndef GARBAGE_AVLHASH_DEFAULT_BUCKETS
#define GARBAGE_AVLHASH_DEFAULT_BUCKETS  16u
#endif

/* AVL-bucketed hash: default number of entry nodes in the shared
 * pool (total live entries across ALL buckets). */
#ifndef GARBAGE_AVLHASH_DEFAULT_NODES
#define GARBAGE_AVLHASH_DEFAULT_NODES    64u
#endif

#endif /* GARBAGE_CONTAINERS_CONFIG_H */
