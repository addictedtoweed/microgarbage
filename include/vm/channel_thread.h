/* ============================================================
 *  channel_thread.h — desktop (pthread) transport factory
 *
 *  Desktop-only. Fills a ChannelTransport with a mutex+condvar
 *  backend whose ctx is heap-owned (freed by transport.destroy).
 *  The H745 backend is a separate factory built at bring-up.
 *
 *  Public domain (CC0). No warranty.
 * ============================================================ */

#ifndef CHANNEL_THREAD_H
#define CHANNEL_THREAD_H

#include "vm/service_channel.h"
#include <stdbool.h>

bool channel_thread_transport_make(ChannelTransport *out);

#endif /* CHANNEL_THREAD_H */
