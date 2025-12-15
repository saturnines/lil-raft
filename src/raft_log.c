/**
 * raft_log.c In-memory log for Raft algorithm
 *
 * This is NOT persistent storage
 * Persistence happens via callbacks in raft_replication.c
 */

#include "raft_internal.h"
#include "raft_errors.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define INITIAL_LOG_CAPACITY 256

// ============================================================================
// Lifecycle
// ============================================================================

int raft_log_init(raft_log_t *log) {
    if (!log) {
        return RAFT_ERR_INVALID_ARG;
    }

    log->entries = malloc(INITIAL_LOG_CAPACITY * sizeof(raft_entry_t));
    if (!log->entries) {
        return RAFT_ERR_NOMEM;
    }

    log->count = 0;
    log->capacity = INITIAL_LOG_CAPACITY;
    log->base_index = 0;  // No compaction yet

    return RAFT_OK;
}

void raft_log_free(raft_log_t *log) {
    if (!log) return;

    // Free all entry data
    for (size_t i = 0; i < log->count; i++) {
        free(log->entries[i].data);
    }

    free(log->entries);
    log->entries = NULL;
    log->count = 0;
    log->capacity = 0;
}

// ============================================================================
// Internal Helpers
// ============================================================================

/**
 * Ensure log has capacity for at least 'needed' entries
 */
static int ensure_capacity(raft_log_t *log, size_t needed) {
    if (needed <= log->capacity) {
        return RAFT_OK;
    }

    size_t new_cap = log->capacity * 2;
    while (new_cap < needed) {
        new_cap *= 2;
    }

    raft_entry_t *new_entries = realloc(log->entries,
                                        new_cap * sizeof(raft_entry_t));
    if (!new_entries) {
        return RAFT_ERR_NOMEM;
    }

    log->entries = new_entries;
    log->capacity = new_cap;

    return RAFT_OK;
}

/**
 * Convert Raft index to array offset
 */
static inline size_t index_to_offset(const raft_log_t *log, uint64_t index) {
    assert(index > log->base_index);
    return (size_t)(index - log->base_index - 1);
}

/**
 * Convert array offset to Raft index
 */
static inline uint64_t offset_to_index(const raft_log_t *log, size_t offset) {
    return log->base_index + offset + 1;
}

// ============================================================================
// Append Operations
// ============================================================================

int raft_log_append(raft_log_t *log, uint64_t term,
                    const void *data, size_t len) {
    if (!log || !data) {
        return RAFT_ERR_INVALID_ARG;
    }

    // Ensure capacity
    int ret = ensure_capacity(log, log->count + 1);
    if (ret != RAFT_OK) {
        return ret;
    }

    // Allocate and copy data
    void *data_copy = malloc(len);
    if (!data_copy) {
        return RAFT_ERR_NOMEM;
    }
    memcpy(data_copy, data, len);

    // Calculate index
    uint64_t index = offset_to_index(log, log->count);

    // Add entry
    log->entries[log->count].index = index;
    log->entries[log->count].term = term;
    log->entries[log->count].type = RAFT_ENTRY_DATA;
    log->entries[log->count].data = data_copy;
    log->entries[log->count].len = len;
    log->count++;

    return RAFT_OK;
}

/**
 * Append a NOOP entry
 */
int raft_log_append_noop(raft_log_t *log, uint64_t term) {
    if (!log) {
        return RAFT_ERR_INVALID_ARG;
    }

    // Ensure capacity
    int ret = ensure_capacity(log, log->count + 1);
    if (ret != RAFT_OK) {
        return ret;
    }

    // Calculate index
    uint64_t index = offset_to_index(log, log->count);

    // Add NOOP entry (no data)
    log->entries[log->count].index = index;
    log->entries[log->count].term = term;
    log->entries[log->count].type = RAFT_ENTRY_NOOP;
    log->entries[log->count].data = NULL;
    log->entries[log->count].len = 0;
    log->count++;

    return RAFT_OK;
}

/**
 * Append an entry preserving its type (used for follower replication)
 */
int raft_log_append_entry(raft_log_t *log, const raft_entry_t *entry) {
    if (!log || !entry) {
        return RAFT_ERR_INVALID_ARG;
    }

    // Ensure capacity
    int ret = ensure_capacity(log, log->count + 1);
    if (ret != RAFT_OK) {
        return ret;
    }

    void *data_copy = NULL;

    // Only copy data for non NOOP entries
    if (entry->type != RAFT_ENTRY_NOOP && entry->data && entry->len > 0) {
        data_copy = malloc(entry->len);
        if (!data_copy) {
            return RAFT_ERR_NOMEM;
        }
        memcpy(data_copy, entry->data, entry->len);
    }

    // Calculate index
    uint64_t index = offset_to_index(log, log->count);

    // Add entry preserving type
    log->entries[log->count].index = index;
    log->entries[log->count].term = entry->term;
    log->entries[log->count].type = entry->type;
    log->entries[log->count].data = data_copy;
    log->entries[log->count].len = entry->len;
    log->count++;

    return RAFT_OK;
}

int raft_log_append_batch(raft_log_t *log,
                          const raft_entry_t *entries,
                          size_t count) {
    if (!log || !entries || count == 0) {
        return RAFT_ERR_INVALID_ARG;
    }

    // Ensure capacity for all entries
    int ret = ensure_capacity(log, log->count + count);
    if (ret != RAFT_OK) {
        return ret;
    }

    // Append each entry
    for (size_t i = 0; i < count; i++) {
        void *data_copy = NULL;

        // Only copy data for non NOOP entries
        if (entries[i].type != RAFT_ENTRY_NOOP && entries[i].data && entries[i].len > 0) {
            data_copy = malloc(entries[i].len);
            if (!data_copy) {
                // Rollback on OOM
                return RAFT_ERR_NOMEM;
            }
            memcpy(data_copy, entries[i].data, entries[i].len);
        }

        uint64_t index = offset_to_index(log, log->count);

        log->entries[log->count].index = index;
        log->entries[log->count].term = entries[i].term;
        log->entries[log->count].type = entries[i].type;
        log->entries[log->count].data = data_copy;
        log->entries[log->count].len = entries[i].len;
        log->count++;
    }

    return RAFT_OK;
}

// ============================================================================
// Lookup
// ============================================================================

raft_entry_t* raft_log_get(raft_log_t *log, uint64_t index) {
    if (!log || index <= log->base_index) {
        return NULL;
    }

    uint64_t last = offset_to_index(log, log->count - 1);
    if (log->count == 0 || index > last) {
        return NULL;
    }

    size_t offset = index_to_offset(log, index);
    return &log->entries[offset];
}

// ============================================================================
// Truncation
// ============================================================================

int raft_log_truncate_after(raft_log_t *log, uint64_t index) {
    if (!log) {
        return RAFT_ERR_INVALID_ARG;
    }

    if (log->count == 0) {
        return RAFT_OK;  // Nothing to truncate
    }

    // If truncating before base_index, clear everything
    if (index <= log->base_index) {
        for (size_t i = 0; i < log->count; i++) {
            free(log->entries[i].data);
        }
        log->count = 0;
        return RAFT_OK;
    }

    // Find truncation point
    uint64_t last = offset_to_index(log, log->count - 1);
    if (index >= last) {
        return RAFT_OK;  // Nothing to truncate
    }

    size_t new_count = index_to_offset(log, index) + 1;

    // Free entries being removed
    for (size_t i = new_count; i < log->count; i++) {
        free(log->entries[i].data);
    }

    log->count = new_count;

    return RAFT_OK;
}

// ============================================================================
// Queries
// ============================================================================

uint64_t raft_log_last_index(const raft_log_t *log) {
    if (!log || log->count == 0) {
        return 0;
    }
    return offset_to_index(log, log->count - 1);
}

uint64_t raft_log_last_term(const raft_log_t *log) {
    if (!log || log->count == 0) {
        return 0;
    }
    return log->entries[log->count - 1].term;
}

uint64_t raft_log_first_index(const raft_log_t *log) {
    if (!log || log->count == 0) {
        return 0;
    }
    return log->base_index + 1;
}

int raft_log_is_empty(const raft_log_t *log) {
    return !log || log->count == 0;
}