/* ============================================================
 *  containers.h — aggregator for data-structure modules
 *
 *  Include this to pull in all containers (hashtable, avlhash,
 *  ring_buffer, fifo_queue, stack, slist, dlist, tree). Each module is
 *  independently usable; this header just saves you the #includes.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef GARBAGE_CONTAINERS_H
#define GARBAGE_CONTAINERS_H

#include "containers/hashtable.h"
#include "containers/avlhash.h"
#include "containers/ring_buffer.h"
#include "containers/fifo_queue.h"
#include "containers/stack.h"
#include "containers/slist.h"
#include "containers/dlist.h"
#include "containers/tree.h"

#endif /* GARBAGE_CONTAINERS_H */
