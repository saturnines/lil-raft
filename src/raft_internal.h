/**
 * raft_internal.h - Internal state structure (private to lil-raft)
 *
 * DO NOT include this in user code - only in raft_*.c files
 */

#ifndef RAFT_INTERNAL_H
#define RAFT_INTERNAL_H

#include "raft.h"
#include "raft_types.h"
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

// ============================================================================
// Raft State
// ============================================================================

typedef enum {
    RAFT_STATE_FOLLOWER,
    RAFT_STATE_CANDIDATE,
    RAFT_STATE_LEADER
} raft_state_t;

// ============================================================================
// In-Memory Log (raft_log.c)
// ============================================================================

typedef struct {
    raft_entry_t *entries;  // Dynamic array
    size_t count;           // Number of entries
    size_t capacity;        // Allocated capacity
    uint64_t base_index;    // Index of entries[0] (after compaction)
} raft_log_t;

// ============================================================================
// Per-Peer State for leaders
// ============================================================================

typedef struct {
    uint64_t next_index;   // Next log index to send to this peer
    uint64_t match_index;  // Highest index known to be replicated on this peer

    // For tracking in-flight RPCs
    int ae_inflight;       // AppendEntries in flight?
    uint64_t ae_sent_ms;   // When was last AE sent?
} raft_peer_t;

// ============================================================================
// Snapshot send progress (for leader tracking transfers to each peer)
// ============================================================================

typedef struct {
    int in_progress;         // Currently sending snapshot to this peer?
    uint64_t offset;         // Current byte offset in snapshot
    uint64_t last_sent_ms;   // When was last chunk sent?
} raft_snapshot_progress_t;

// ============================================================================
// Main Raft Structure
// ============================================================================

struct raft {
    // Configuration
    int my_id;
    int num_nodes;
    raft_config_t config;
    raft_callbacks_t callbacks;
    void *callback_ctx;

    // Persistent state fig 2
    uint64_t current_term;
    int voted_for;           // -1 if none
    raft_log_t log;          // In-memory copy (backed by callbacks)

    // Volatile state fig 2
    raft_state_t state;
    uint64_t commit_index;
    uint64_t last_applied;

    // Volatile state leaders only fig 2
    raft_peer_t *peers;      // Per-peer replication state

    // Election state
    uint64_t election_timeout_ms;
    uint64_t last_heartbeat_ms;
    int votes_received;
    int *votes_for_me;

    // Pre-vote state
    int prevote_in_progress;
    int prevotes_received;
    int *prevotes_for_me;

    // Leader state
    int leader_id;           // Current leader (-1 if unknown)

    // Snapshot state (completed)
    uint64_t snapshot_last_index;   // Last index included in snapshot
    uint64_t snapshot_last_term;    // Term of that index
    int snapshot_in_progress;       // Currently creating a snapshot?

    // Snapshot state (pending, for async snapshots)
    uint64_t snapshot_pending_index;  // Index being snapshotted (async)
    uint64_t snapshot_pending_term;   // Term being snapshotted (async)

    // Per-peer snapshot transfer progress (leader only)
    raft_snapshot_progress_t *snapshot_send;

    // Timing
    struct timespec last_tick;

    // PRNG (xorshift32)
    uint32_t prng_state;

    // Flags
    int shutdown;
};

// ============================================================================
// State Transitions (raft.c)
// ============================================================================

void raft_become_follower(raft_t *r, uint64_t term);
void raft_become_candidate(raft_t *r);
void raft_become_leader(raft_t *r);

// ============================================================================
// Helpers (raft.c)
// ============================================================================

uint64_t raft_get_time_ms(void);
uint64_t raft_random_election_timeout(raft_t *r);

static inline int raft_quorum_size(int num_nodes) {
    return (num_nodes / 2) + 1;
}

// ============================================================================
// Log Operations (raft_log.c)
// ============================================================================

/**
 * Free entry data (internal use only)
 * Sets data to NULL to prevent double-free
 */
static inline void raft_entry_free(raft_entry_t *entry) {
    if (!entry) return;
    free(entry->data);
    entry->data = NULL;
    entry->len = 0;
}

int raft_log_init(raft_log_t *log);
void raft_log_free(raft_log_t *log);
int raft_log_append(raft_log_t *log, uint64_t term, const void *data, size_t len);
int raft_log_append_noop(raft_log_t *log, uint64_t term);
int raft_log_append_entry(raft_log_t *log, const raft_entry_t *entry);
int raft_log_append_batch(raft_log_t *log, const raft_entry_t *entries, size_t count);
raft_entry_t* raft_log_get(raft_log_t *log, uint64_t index);
int raft_log_truncate_after(raft_log_t *log, uint64_t index);
int raft_log_truncate_before(raft_log_t *log, uint64_t index);
void raft_log_clear(raft_log_t *log);
uint64_t raft_log_last_index(const raft_log_t *log);
uint64_t raft_log_last_term(const raft_log_t *log);
uint64_t raft_log_first_index(const raft_log_t *log);
int raft_log_is_empty(const raft_log_t *log);
size_t raft_log_copy_range(raft_log_t *log, uint64_t start_idx,
                           raft_entry_t *out, size_t max_entries);

// ============================================================================
// Replication (raft_replication.c)
// ============================================================================

void raft_send_heartbeats(raft_t *r);

// ============================================================================
// Election (raft_election.c)
// ============================================================================

void raft_send_requestvote_all(raft_t *r);
void raft_start_prevote(raft_t *r);

// ============================================================================
// Snapshot (raft_snapshot.c)
// ============================================================================

int raft_maybe_snapshot(raft_t *r);
int raft_snapshot_restore(raft_t *r);
int raft_send_installsnapshot(raft_t *r, int peer_id);
int raft_peer_needs_snapshot(const raft_t *r, int peer_id);

// Async snapshot support
void raft_snapshot_poll(raft_t *r);
void raft_snapshot_finish(raft_t *r);

#endif // RAFT_INTERNAL_H