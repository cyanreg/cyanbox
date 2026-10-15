/* Copyright © 2026, Lynne. BSD-2-Clause, see LICENSE.md. */

/* The TAG LIST (rxbox/link.h, RXLinkTagList): the tracks a DJ collects
 * while browsing, on a deck or in the interface, in the order they were
 * added. Shared by every service of a set (their sessions, each on its
 * service's thread) and the owner, so under a lock; in memory only. */

#include "rxbox/link.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "rxbox/common.h"

struct RXLinkTagList {
    pthread_mutex_t lock;
    uint32_t ids[RX_LINK_TAG_LIST_MAX];
    int nb;
    atomic_uint generation;
};

int rx_link_tag_list_new(RXLinkTagList **out)
{
    RXLinkTagList *tl = calloc(1, sizeof(*tl));
    if (!tl)
        return RX_ERROR(ENOMEM);
    pthread_mutex_init(&tl->lock, NULL);
    *out = tl;
    return 0;
}

void rx_link_tag_list_free(RXLinkTagList **tl)
{
    if (!*tl)
        return;
    pthread_mutex_destroy(&(*tl)->lock);
    free(*tl);
    *tl = NULL;
}

static int find(const RXLinkTagList *tl, uint32_t id)
{
    for (int i = 0; i < tl->nb; i++)
        if (tl->ids[i] == id)
            return i;
    return -1;
}

int rx_link_tag_list_add(RXLinkTagList *tl, uint32_t id)
{
    pthread_mutex_lock(&tl->lock);
    int ret = find(tl, id) >= 0 ? 0 : tl->nb == RX_LINK_TAG_LIST_MAX ? RX_ERROR(ENOSPC) : 1;
    if (ret > 0) {
        tl->ids[tl->nb++] = id;
        atomic_fetch_add(&tl->generation, 1);
    }
    pthread_mutex_unlock(&tl->lock);
    return ret;
}

int rx_link_tag_list_remove(RXLinkTagList *tl, uint32_t id)
{
    pthread_mutex_lock(&tl->lock);
    int at = find(tl, id);
    if (at >= 0) {
        memmove(tl->ids + at, tl->ids + at + 1, (size_t)(tl->nb - at - 1) * sizeof(*tl->ids));
        tl->nb--;
        atomic_fetch_add(&tl->generation, 1);
    }
    pthread_mutex_unlock(&tl->lock);
    return at >= 0;
}

void rx_link_tag_list_clear(RXLinkTagList *tl)
{
    pthread_mutex_lock(&tl->lock);
    if (tl->nb) {
        tl->nb = 0;
        atomic_fetch_add(&tl->generation, 1);
    }
    pthread_mutex_unlock(&tl->lock);
}

bool rx_link_tag_list_has(RXLinkTagList *tl, uint32_t id)
{
    pthread_mutex_lock(&tl->lock);
    bool has = find(tl, id) >= 0;
    pthread_mutex_unlock(&tl->lock);
    return has;
}

int rx_link_tag_list_get(RXLinkTagList *tl, uint32_t *ids, int max)
{
    pthread_mutex_lock(&tl->lock);
    int n = tl->nb;
    memcpy(ids, tl->ids, (size_t)RX_CLAMP(n, 0, max) * sizeof(*ids));
    pthread_mutex_unlock(&tl->lock);
    return n;
}

unsigned rx_link_tag_list_generation(RXLinkTagList *tl)
{
    return atomic_load(&tl->generation);
}
