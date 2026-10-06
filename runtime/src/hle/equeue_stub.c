/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Placeholder equeue implementation so other libraries can link against
 * equeue.h before workstream A lands. Workstream A deletes this file. */
#include "equeue.h"

bool equeue_add_event(equeue *eq, uint64_t ident, int16_t filter, uint16_t flags, void *udata) {
    (void)eq; (void)ident; (void)filter; (void)flags; (void)udata;
    return true;
}

bool equeue_remove_event(equeue *eq, uint64_t ident, int16_t filter) {
    (void)eq; (void)ident; (void)filter;
    return true;
}

void equeue_trigger(equeue *eq, uint64_t ident, int16_t filter, uint64_t data) {
    (void)eq; (void)ident; (void)filter; (void)data;
}
