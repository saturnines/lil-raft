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
// Per-Peer State (for leaders)
// ============================================================================

typedef struct {
    uint64_t next_index;   // Next log index to send to this peer
    uint64_t match_index;  // Highest index known to be replicated on this peer

    // For tracking in-flight RPCs
    int ae_inflight;       // AppendEntries in flight?
    uint64_t ae_sent_ms;   // When was last AE sent?
} raft_peer_t;

// ============================================================================
// Main Raft Structure
// ============================================================================

struct raft {
    // Configuration
    int my_id;
    int num_nodes;
    raft_callbacks_t callbacks;
    void *callback_ctx;

    // Persistent state (Figure 2 - must survive crashes)
    uint64_t current_term;
    int voted_for;           // -1 if none
    raft_log_t log;          // In-memory copy (backed by callbacks)

    // Volatile state (Figure 2 - all servers)
    raft_state_t state;
    uint64_t commit_index;
    uint64_t last_applied;

    // Volatile state (Figure 2 - leaders only)
    raft_peer_t *peers;      // Per-peer replication state

    // Election state
    uint64_t election_timeout_ms;
    uint64_t last_heartbeat_ms;
    int votes_received;      // Count of votes in current election
    int *votes_for_me;       // Boolean array: did peer vote for me?

    // Leader state
    int leader_id;           // Current leader (-1 if unknown)

    // Read index state (for linearizable reads - Ongaro §6.4)
    uint64_t read_index;          // Last confirmed safe read index
    int read_index_pending;       // Waiting for heartbeat confirmation?
    uint64_t read_index_sent_ms;  // When heartbeat was sent
    int *read_acks;               // Heartbeat acks for read confirmation

    // Timing
    struct timespec last_tick;

    // Flags
    int shutdown;            // Shutting down?
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
uint64_t raft_random_election_timeout(void);

// ============================================================================
// Log Operations (raft_log.c)
// ============================================================================

int raft_log_init(raft_log_t *log);
void raft_log_free(raft_log_t *log);
int raft_log_append(raft_log_t *log, uint64_t term, const void *data, size_t len);
int raft_log_append_batch(raft_log_t *log, const raft_entry_t *entries, size_t count);
raft_entry_t* raft_log_get(raft_log_t *log, uint64_t index);
int raft_log_truncate_after(raft_log_t *log, uint64_t index);
uint64_t raft_log_last_index(const raft_log_t *log);
uint64_t raft_log_last_term(const raft_log_t *log);
uint64_t raft_log_first_index(const raft_log_t *log);
int raft_log_is_empty(const raft_log_t *log);

#endif // RAFT_INTERNAL_H