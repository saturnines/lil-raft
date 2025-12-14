/**
 * raft.c - Core Raft state machine
 */

#include "raft_internal.h"
#include "raft_errors.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

// Election timeout range: 150-300ms
#define ELECTION_TIMEOUT_MIN_MS  150
#define ELECTION_TIMEOUT_MAX_MS  300

// Heartbeat interval: 50ms
#define HEARTBEAT_INTERVAL_MS    50

// ============================================================================
// Helpers
// ============================================================================

/**
 * Get current time in milliseconds
 */
uint64_t raft_get_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/**
 * Get random election timeout (150-300ms)
 */
uint64_t raft_random_election_timeout(void) {
    int range = ELECTION_TIMEOUT_MAX_MS - ELECTION_TIMEOUT_MIN_MS;
    return ELECTION_TIMEOUT_MIN_MS + (rand() % range);
}

/**
 * Get majority count for quorum
 */
static inline int quorum_size(int num_nodes) {
    return (num_nodes / 2) + 1;
}

// ============================================================================
// State Transitions
// ============================================================================

void raft_become_follower(raft_t *r, uint64_t term) {
    if (!r) return;

    printf("[Node %d] Became FOLLOWER for term %lu\n", r->my_id, term);

    r->state = RAFT_STATE_FOLLOWER;
    r->current_term = term;
    r->voted_for = -1;
    r->leader_id = -1;
    r->last_heartbeat_ms = raft_get_time_ms();
    r->election_timeout_ms = raft_random_election_timeout();

    // Clear votes
    r->votes_received = 0;
    if (r->votes_for_me) {
        memset(r->votes_for_me, 0, r->num_nodes * sizeof(int));
    }

    // Persist term and vote
    if (r->callbacks.persist_vote) {
        r->callbacks.persist_vote(r->callback_ctx, r->current_term, r->voted_for);
    }
}

void raft_become_candidate(raft_t *r) {
    if (!r) return;

    // Increment term
    r->current_term++;
    r->state = RAFT_STATE_CANDIDATE;
    r->voted_for = r->my_id;  // Vote for self
    r->leader_id = -1;
    r->last_heartbeat_ms = raft_get_time_ms();
    r->election_timeout_ms = raft_random_election_timeout();

    // Reset votes
    r->votes_received = 1;  // Vote for self
    if (r->votes_for_me) {
        memset(r->votes_for_me, 0, r->num_nodes * sizeof(int));
        r->votes_for_me[r->my_id] = 1;
    }

    printf("[Node %d] Became CANDIDATE for term %lu\n", r->my_id, r->current_term);

    // Persist term and vote
    if (r->callbacks.persist_vote) {
        r->callbacks.persist_vote(r->callback_ctx, r->current_term, r->voted_for);
    }

    // Send RequestVote to all peers (used in raft_election.c)
    raft_send_requestvote_all(r);
}

void raft_become_leader(raft_t *r) {
    if (!r) return;

    r->state = RAFT_STATE_LEADER;
    r->leader_id = r->my_id;
    r->last_heartbeat_ms = raft_get_time_ms();

    printf("[Node %d] Became LEADER for term %lu\n", r->my_id, r->current_term);

    // Initialize leader state
    uint64_t last_index = raft_log_last_index(&r->log);
    for (int i = 0; i < r->num_nodes; i++) {
        r->peers[i].next_index = last_index + 1;
        r->peers[i].match_index = 0;
        r->peers[i].ae_inflight = 0;
    }

    // Send immediate heartbeat (implemented in raft_replication.c)
    raft_send_heartbeats(r);
}

// ============================================================================
// Lifecycle
// ============================================================================

raft_t* raft_create(int my_id,
                    int num_nodes,
                    const raft_callbacks_t *callbacks,
                    void *callback_ctx) {
    if (!callbacks || num_nodes <= 0 || my_id < 0 || my_id >= num_nodes) {
        return NULL;
    }

    raft_t *r = calloc(1, sizeof(raft_t));
    if (!r) {
        return NULL;
    }

    // Configuration
    r->my_id = my_id;
    r->num_nodes = num_nodes;
    r->callbacks = *callbacks;
    r->callback_ctx = callback_ctx;

    // Initialize log
    if (raft_log_init(&r->log) != RAFT_OK) {
        free(r);
        return NULL;
    }

    // Allocate peer state
    r->peers = calloc(num_nodes, sizeof(raft_peer_t));
    r->votes_for_me = calloc(num_nodes, sizeof(int));
    r->read_acks = calloc(num_nodes, sizeof(int));

    if (!r->peers || !r->votes_for_me || !r->read_acks) {
        raft_log_free(&r->log);
        free(r->peers);
        free(r->votes_for_me);
        free(r->read_acks);
        free(r);
        return NULL;
    }

    // Load persistent state
    uint64_t term = 0;
    int voted_for = -1;
    if (callbacks->load_vote) {
        callbacks->load_vote(callback_ctx, &term, &voted_for);
    }

    r->current_term = term;
    r->voted_for = voted_for;

    // Initialize volatile state
    r->state = RAFT_STATE_FOLLOWER;
    r->commit_index = 0;
    r->last_applied = 0;
    r->leader_id = -1;

    // Timing
    r->election_timeout_ms = raft_random_election_timeout();
    r->last_heartbeat_ms = raft_get_time_ms();
    clock_gettime(CLOCK_MONOTONIC, &r->last_tick);

    // Seed random for election timeouts
    srand((unsigned int)time(NULL) + my_id);

    printf("[Node %d] Created (term=%lu, nodes=%d)\n", my_id, term, num_nodes);

    return r;
}

void raft_destroy(raft_t *r) {
    if (!r) return;

    raft_log_free(&r->log);
    free(r->peers);
    free(r->votes_for_me);
    free(r->read_acks);
    free(r);
}

int raft_shutdown(raft_t *r) {
    if (!r) {
        return RAFT_ERR_INVALID_ARG;
    }

    r->shutdown = 1;

    // TODO: Flush pending operations
    // TODO: Wait for in-flight RPCs

    return RAFT_OK;
}

// ============================================================================
// Tick for progress,ections, heartbeats
// ============================================================================

void raft_tick(raft_t *r) {
    if (!r || r->shutdown) return;

    uint64_t now = raft_get_time_ms();
    uint64_t elapsed = now - r->last_heartbeat_ms;

    // Follower or Candidate: Check election timeout
    if (r->state == RAFT_STATE_FOLLOWER || r->state == RAFT_STATE_CANDIDATE) {
        if (elapsed >= r->election_timeout_ms) {
            // Election timeout, become candidate
            raft_become_candidate(r);
        }
    }

    // Leader: Send heartbeats
    if (r->state == RAFT_STATE_LEADER) {
        if (elapsed >= HEARTBEAT_INTERVAL_MS) {
            extern void raft_send_heartbeats(raft_t *r);
            raft_send_heartbeats(r);
            r->last_heartbeat_ms = now;
        }
    }

    // Apply committed entries (if any)
    // TODO: Move to separate function
    while (r->last_applied < r->commit_index) {
        r->last_applied++;

        raft_entry_t *entry = raft_log_get(&r->log, r->last_applied);
        if (entry && r->callbacks.apply_entry) {
            r->callbacks.apply_entry(r->callback_ctx,
                                    entry->index,
                                    entry->term,
                                    entry->data,
                                    entry->len);
        }
    }
}

// ============================================================================
// State Queries
// ============================================================================

int raft_is_leader(const raft_t *r) {
    return r && r->state == RAFT_STATE_LEADER;
}

uint64_t raft_get_term(const raft_t *r) {
    return r ? r->current_term : 0;
}

uint64_t raft_get_commit_index(const raft_t *r) {
    return r ? r->commit_index : 0;
}

uint64_t raft_get_last_applied(const raft_t *r) {
    return r ? r->last_applied : 0;
}

int raft_get_leader_id(const raft_t *r) {
    return r ? r->leader_id : -1;
}

int raft_get_node_id(const raft_t *r) {
    return r ? r->my_id : -1;
}