/*
 * visibox_store.c — Bounded Ring Buffer for Response History
 *
 * Stores OutputBuffer pointers for fetch_page and search_jump.
 * FIFO eviction when max_entries or max_total_bytes is reached.
 */

#include "visibox.h"

/* visibox_store is defined in visibox_core.c (the single definition) */

void visibox_store_init(void) {
    memset(&visibox_store, 0, sizeof(ResponseStore));
    pthread_mutex_init(&visibox_store.lock, NULL);
}

/*
 * visibox_store_add — Add a response's buffer to the store.
 * Evicts oldest entries if limits are exceeded.
 * Returns 0 on success, -1 on failure.
 */
int visibox_store_add(VisiboxResponse *res, OutputBuffer *buf) {
    if (!res || !buf) return -1;

    pthread_mutex_lock(&visibox_store.lock);

    size_t new_bytes = buf->used + sizeof(StoreEntry);

    /* Evict until we have room */
    while (visibox_store.count >= VISIBOX_STORE_MAX_ENTRIES ||
           (visibox_store.total_bytes_used + new_bytes > VISIBOX_STORE_MAX_BYTES &&
            visibox_store.count > 0)) {

        StoreEntry *oldest = &visibox_store.entries[visibox_store.head];

        /* Free the buffer */
        if (oldest->buffer) {
            visibox_output_buffer_free(oldest->buffer);
            oldest->buffer = NULL;
        }

        visibox_store.total_bytes_used -= oldest->approx_bytes;
        visibox_store.head = (visibox_store.head + 1) % VISIBOX_STORE_MAX_ENTRIES;
        visibox_store.count--;
    }

    /* Insert at head position */
    size_t slot = (visibox_store.head + visibox_store.count) % VISIBOX_STORE_MAX_ENTRIES;
    StoreEntry *entry = &visibox_store.entries[slot];

    strncpy(entry->response_id, res->response_id, VISIBOX_ID_LEN - 1);
    entry->response_id[VISIBOX_ID_LEN - 1] = '\0';
    entry->buffer = buf;
    entry->created_at = time(NULL);
    entry->approx_bytes = new_bytes;

    visibox_store.count++;
    visibox_store.total_bytes_used += new_bytes;

    pthread_mutex_unlock(&visibox_store.lock);
    return 0;
}

/*
 * visibox_store_get — Find and return the buffer for a given response_id.
 * Returns NULL if not found (expired/evicted).
 */
OutputBuffer *visibox_store_get(const char *response_id) {
    if (!response_id) return NULL;

    pthread_mutex_lock(&visibox_store.lock);

    for (size_t i = 0; i < visibox_store.count; i++) {
        size_t idx = (visibox_store.head + i) % VISIBOX_STORE_MAX_ENTRIES;
        StoreEntry *entry = &visibox_store.entries[idx];
        if (strcmp(entry->response_id, response_id) == 0) {
            OutputBuffer *buf = entry->buffer;
            pthread_mutex_unlock(&visibox_store.lock);
            return buf;
        }
    }

    pthread_mutex_unlock(&visibox_store.lock);
    return NULL;
}

/*
 * visibox_store_has — Check if a response_id exists in the store.
 */
int visibox_store_has(const char *response_id) {
    return visibox_store_get(response_id) != NULL;
}

/*
 * visibox_store_cleanup — Free all entries (called at shutdown).
 */
void visibox_store_cleanup(void) {
    pthread_mutex_lock(&visibox_store.lock);

    for (size_t i = 0; i < VISIBOX_STORE_MAX_ENTRIES; i++) {
        if (visibox_store.entries[i].buffer) {
            visibox_output_buffer_free(visibox_store.entries[i].buffer);
            visibox_store.entries[i].buffer = NULL;
        }
    }

    visibox_store.count = 0;
    visibox_store.total_bytes_used = 0;
    visibox_store.head = 0;

    pthread_mutex_unlock(&visibox_store.lock);
    pthread_mutex_destroy(&visibox_store.lock);
}