/**
 * raft_replication.c - AppendEntries RPC logic
 */

#include "raft_internal.h"
#include "raft_errors.h"
#include <stdio.h>
#include <string.h>

// ============================================================================
// Send Heartbeats / AppendEntries to All Peers
// ============================================================================

void raft_send_heartbeats(raft_t *r) {
    if (!r || r->state != RAFT_STATE_LEADER) return;

    for (int i = 0; i < r->num_nodes; i++) {
        if (i == r->my_id) continue;

        uint64_t next_idx = r->peers[i].next_index;
        uint64_t prev_idx = next_idx - 1;
        uint64_t prev_term = 0;

        // Get prev_log_term
        if (prev_idx > 0) {
            raft_entry_t *prev_entry = raft_log_get(&r->log, prev_idx);
            if (prev_entry) {
                prev_term = prev_entry->term;
            }
        }

        raft_appendentries_req_t req = {
            .term = r->current_term,
            .leader_id = r->my_id,
            .prev_log_index = prev_idx,
            .prev_log_term = prev_term,
            .leader_commit = r->commit_index,
        };

        // Collect entries to send (from next_index to end of log)
        uint64_t last_idx = raft_log_last_index(&r->log);
        size_t n_entries = 0;
        raft_entry_t *entries = NULL;

        if (next_idx <= last_idx) {
            n_entries = (size_t)(last_idx - next_idx + 1);
            entries = raft_log_get(&r->log, next_idx);
        }

        if (r->callbacks.send_appendentries) {
            r->callbacks.send_appendentries(r->callback_ctx, i, &req,
                                            entries, n_entries);
        }
    }
}

// ============================================================================
// Handle Incoming AppendEntries
// ============================================================================

int raft_recv_appendentries(raft_t *r,
                            const raft_appendentries_req_t *req,
                            const raft_entry_t *entries,
                            size_t n_entries,
                            raft_appendentries_resp_t *resp) {
    if (!r || !req || !resp) {
        return RAFT_ERR_INVALID_ARG;
    }

    resp->term = r->current_term;
    resp->success = 0;
    resp->match_index = 0;

    // Rule 1: Reply false if term < currentTerm
    if (req->term < r->current_term) {
        printf("[Node %d] Rejecting AE from %d: stale term %lu < %lu\n",
               r->my_id, req->leader_id, req->term, r->current_term);
        return RAFT_OK;
    }

    // If RPC request contains term >= currentTerm, recognize leader
    if (req->term >= r->current_term) {
        if (req->term > r->current_term || r->state != RAFT_STATE_FOLLOWER) {
            raft_become_follower(r, req->term);
        }
        r->leader_id = req->leader_id;
        r->last_heartbeat_ms = raft_get_time_ms();
    }

    // Rule 2: Reply false if log doesn't contain an entry at prevLogIndex
    // whose term matches prevLogTerm
    if (req->prev_log_index > 0) {
        raft_entry_t *prev_entry = raft_log_get(&r->log, req->prev_log_index);

        if (!prev_entry) {
            printf("[Node %d] Rejecting AE: missing entry at prev_log_index %lu\n",
                   r->my_id, req->prev_log_index);
            resp->match_index = raft_log_last_index(&r->log);
            return RAFT_OK;
        }

        if (prev_entry->term != req->prev_log_term) {
            printf("[Node %d] Rejecting AE: term mismatch at %lu (got %lu, want %lu)\n",
                   r->my_id, req->prev_log_index, prev_entry->term, req->prev_log_term);
            // Truncate conflicting entries
            raft_log_truncate_after(&r->log, req->prev_log_index - 1);
            resp->match_index = req->prev_log_index - 1;
            return RAFT_OK;
        }
    }

    // Rule 3 & 4: Append any new entries not already in the log
    uint64_t insert_index = req->prev_log_index + 1;

    for (size_t i = 0; i < n_entries; i++) {
        uint64_t entry_index = insert_index + i;
        raft_entry_t *existing = raft_log_get(&r->log, entry_index);

        if (existing) {
            // Rule 3: If an existing entry conflicts with a new one, delete the existing entry and all that follow it
            if (existing->term != entries[i].term) {
                raft_log_truncate_after(&r->log, entry_index - 1);
                existing = NULL;
            }
        }

        if (!existing) {
            // Append to in mem log
            raft_log_append(&r->log, entries[i].term,
                           entries[i].data, entries[i].len);

            // Persist via callback
            if (r->callbacks.log_append) {
                r->callbacks.log_append(r->callback_ctx, entry_index,
                                       entries[i].term,
                                       entries[i].data, entries[i].len);
            }
        }
    }

    // Rule 5: If leaderCommit > commitIndex, set commitIndex =
    // min(leaderCommit, index of last new entry)
    if (req->leader_commit > r->commit_index) {
        uint64_t last_new_index = req->prev_log_index + n_entries;
        uint64_t last_log_idx = raft_log_last_index(&r->log);

        r->commit_index = req->leader_commit;
        if (last_new_index < r->commit_index) {
            r->commit_index = last_new_index;
        }
        if (last_log_idx < r->commit_index) {
            r->commit_index = last_log_idx;
        }
    }

    resp->success = 1;
    resp->match_index = raft_log_last_index(&r->log);

    return RAFT_OK;
}

// ============================================================================
// Handle AppendEntries Response
// ============================================================================

int raft_recv_appendentries_response(raft_t *r,
                                     int peer_id,
                                     const raft_appendentries_resp_t *resp) {
    if (!r || !resp) {
        return RAFT_ERR_INVALID_ARG;
    }

    // Ignore if we're not leader
    if (r->state != RAFT_STATE_LEADER) {
        return RAFT_OK;
    }

    // If response contains higher term, step down
    if (resp->term > r->current_term) {
        raft_become_follower(r, resp->term);
        return RAFT_OK;
    }

    // Track read index acks (heartbeat confirmations for linearizable reads)
    if (r->read_index_pending && resp->term == r->current_term) {
        if (!r->read_acks[peer_id]) {
            r->read_acks[peer_id] = 1;

            int ack_count = 0;
            for (int i = 0; i < r->num_nodes; i++) {
                if (r->read_acks[i]) ack_count++;
            }

            int majority = (r->num_nodes / 2) + 1;
            if (ack_count >= majority) {
                r->read_index_pending = 0;
            }
        }
    }

    if (resp->success) {
        // Update next_index and match_index for follower
        if (resp->match_index > r->peers[peer_id].match_index) {
            r->peers[peer_id].match_index = resp->match_index;
            r->peers[peer_id].next_index = resp->match_index + 1;
        }

        // Try to advance commit_index
        // Find the highest N such that a majority of matchIndex[i] >= N
        // and log[N].term == currentTerm
        for (uint64_t n = r->commit_index + 1; n <= raft_log_last_index(&r->log); n++) {
            raft_entry_t *entry = raft_log_get(&r->log, n);
            if (!entry || entry->term != r->current_term) {
                continue;  // Only commit entries from current term
            }

            int replication_count = 1;  // Count self
            for (int i = 0; i < r->num_nodes; i++) {
                if (i != r->my_id && r->peers[i].match_index >= n) {
                    replication_count++;
                }
            }

            int majority = (r->num_nodes / 2) + 1;
            if (replication_count >= majority) {
                r->commit_index = n;
            }
        }
    } else {
        // AppendEntries failed decrement next_index and retry
        // Use match_index hint for faster rollback
        if (resp->match_index > 0 && resp->match_index < r->peers[peer_id].next_index) {
            r->peers[peer_id].next_index = resp->match_index + 1;
        } else if (r->peers[peer_id].next_index > 1) {
            r->peers[peer_id].next_index--;
        }
    }

    return RAFT_OK;
}

// ============================================================================
// Propose (Client Write)
// ============================================================================

int raft_propose(raft_t *r, const void *data, size_t len) {
    if (!r) {
        return RAFT_ERR_INVALID_ARG;
    }

    if (r->shutdown) {
        return RAFT_ERR_SHUTDOWN;
    }

    if (r->state != RAFT_STATE_LEADER) {
        return RAFT_ERR_NOT_LEADER;
    }

    // Append to in mem log
    int ret = raft_log_append(&r->log, r->current_term, data, len);
    if (ret != RAFT_OK) {
        return ret;
    }

    uint64_t index = raft_log_last_index(&r->log);

    // Persist via callback
    if (r->callbacks.log_append) {
        ret = r->callbacks.log_append(r->callback_ctx, index,
                                      r->current_term, data, len);
        if (ret != 0) {
            // Rollback in-memory on persist failure
            raft_log_truncate_after(&r->log, index - 1);
            return RAFT_ERR_INVALID_ARG;
        }
    }

    // Update own match_index
    r->peers[r->my_id].match_index = index;
    r->peers[r->my_id].next_index = index + 1;


    // This is such a lazy fix but it will work for single node clusters.
    if (r->num_nodes == 1) {
        r->commit_index = index;
    }

    // Send AppendEntries immediately
    raft_send_heartbeats(r);

    return RAFT_OK;
}

int raft_propose_batch(raft_t *r,
                       const void **entries,
                       const size_t *lengths,
                       size_t count) {
    if (!r || !entries || !lengths || count == 0) {
        return RAFT_ERR_INVALID_ARG;
    }

    if (r->shutdown) {
        return RAFT_ERR_SHUTDOWN;
    }

    if (r->state != RAFT_STATE_LEADER) {
        return RAFT_ERR_NOT_LEADER;
    }

    // Append all entries
    for (size_t i = 0; i < count; i++) {
        int ret = raft_log_append(&r->log, r->current_term, entries[i], lengths[i]);
        if (ret != RAFT_OK) {
            return ret;
        }
    }

    uint64_t last_index = raft_log_last_index(&r->log);
    uint64_t first_index = last_index - count + 1;

    // Persist all entries
    if (r->callbacks.log_append_batch) {
        raft_entry_t *batch = raft_log_get(&r->log, first_index);
        int ret = r->callbacks.log_append_batch(r->callback_ctx, batch, count);
        if (ret != 0) {
            raft_log_truncate_after(&r->log, first_index - 1);
            return RAFT_ERR_INVALID_ARG;
        }
    } else if (r->callbacks.log_append) {
        for (size_t i = 0; i < count; i++) {
            raft_entry_t *e = raft_log_get(&r->log, first_index + i);
            int ret = r->callbacks.log_append(r->callback_ctx,
                                              e->index, e->term, e->data, e->len);
            if (ret != 0) {
                raft_log_truncate_after(&r->log, first_index - 1);
                return RAFT_ERR_INVALID_ARG;
            }
        }
    }

    // Update own match_index
    r->peers[r->my_id].match_index = last_index;
    r->peers[r->my_id].next_index = last_index + 1;

    // Replicate
    raft_send_heartbeats(r);

    return RAFT_OK;
}

// ============================================================================
// Linearizable Reads
// ============================================================================

int raft_read_index(raft_t *r, uint64_t *read_index) {
    if (!r || !read_index) {
        return RAFT_ERR_INVALID_ARG;
    }

    if (r->state != RAFT_STATE_LEADER) {
        return RAFT_ERR_NOT_LEADER;
    }

    // Single-node cluster is always safe
    if (r->num_nodes == 1) {
        *read_index = r->commit_index;
        return RAFT_OK;
    }

    // Record commit_index as read point
    r->read_index = r->commit_index;
    r->read_index_pending = 1;
    r->read_index_sent_ms = raft_get_time_ms();

    // Reset acks, count self
    memset(r->read_acks, 0, r->num_nodes * sizeof(int));
    r->read_acks[r->my_id] = 1;

    // Send heartbeats to confirm leadership
    raft_send_heartbeats(r);

    *read_index = r->read_index;
    return RAFT_OK;
}

int raft_barrier(raft_t *r) {
    if (!r) {
        return RAFT_ERR_INVALID_ARG;
    }

    if (r->state != RAFT_STATE_LEADER) {
        return RAFT_ERR_NOT_LEADER;
    }

    if (r->num_nodes == 1) {
        return RAFT_OK;
    }

    // Push replication
    raft_send_heartbeats(r);

    // caller polls commit_index non blocking
    return RAFT_OK;
}