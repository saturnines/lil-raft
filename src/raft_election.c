/**
 * raft_election.c - RequestVote RPC logic
 */

#include "raft_internal.h"
#include "raft_errors.h"
#include <stdio.h>

// ============================================================================
// Send RequestVote to All Peers
// ============================================================================

void raft_send_requestvote_all(raft_t *r) {
    if (!r || r->state != RAFT_STATE_CANDIDATE) return;

    raft_requestvote_req_t req = {
        .term = r->current_term,
        .candidate_id = r->my_id,
        .last_log_index = raft_log_last_index(&r->log),
        .last_log_term = raft_log_last_term(&r->log),
    };

    for (int i = 0; i < r->num_nodes; i++) {
        if (i == r->my_id) continue;

        if (r->callbacks.send_requestvote) {
            r->callbacks.send_requestvote(r->callback_ctx, i, &req);
        }
    }
}

// ============================================================================
// Handle Incoming RequestVote
// ============================================================================

int raft_recv_requestvote(raft_t *r,
                          const raft_requestvote_req_t *req,
                          raft_requestvote_resp_t *resp) {
    if (!r || !req || !resp) {
        return RAFT_ERR_INVALID_ARG;
    }

    resp->term = r->current_term;
    resp->vote_granted = 0;

    // Rule 1: Reply false if term < currentTerm
    if (req->term < r->current_term) {
        printf("[Node %d] Rejecting vote for %d: stale term %lu < %lu\n",
               r->my_id, req->candidate_id, req->term, r->current_term);
        return RAFT_OK;
    }

    // If RPC request contains term > currentTerm, update and convert to follower
    if (req->term > r->current_term) {
        raft_become_follower(r, req->term);
    }

    // Rule 2: If votedFor is null or candidateId, and candidate's log is at least as up-to-date as receiver's log, grant vote
    int can_vote = (r->voted_for == -1 || r->voted_for == req->candidate_id);

    if (!can_vote) {
        printf("[Node %d] Rejecting vote for %d: already voted for %d\n",
               r->my_id, req->candidate_id, r->voted_for);
        return RAFT_OK;
    }

    // Check if candidate's log is at least as up-to-date as ours
    // If logs have last entries with different terms, the log with the later term is more up-to-date
    // If logs end with the same term, the longer log is more up-to-date
    uint64_t my_last_term = raft_log_last_term(&r->log);
    uint64_t my_last_index = raft_log_last_index(&r->log);

    int log_ok = (req->last_log_term > my_last_term) ||
                 (req->last_log_term == my_last_term &&
                  req->last_log_index >= my_last_index);

    if (!log_ok) {
        printf("[Node %d] Rejecting vote for %d: log not up-to-date "
               "(their: %lu/%lu, mine: %lu/%lu)\n",
               r->my_id, req->candidate_id,
               req->last_log_term, req->last_log_index,
               my_last_term, my_last_index);
        return RAFT_OK;
    }

    // Grant vote
    r->voted_for = req->candidate_id;
    r->last_heartbeat_ms = raft_get_time_ms();  // Reset election timeout

    // Persist vote
    if (r->callbacks.persist_vote) {
        r->callbacks.persist_vote(r->callback_ctx, r->current_term, r->voted_for);
    }

    resp->vote_granted = 1;
    resp->term = r->current_term;

    printf("[Node %d] Granted vote to %d for term %lu\n",
           r->my_id, req->candidate_id, req->term);

    return RAFT_OK;
}

// ============================================================================
// Handle RequestVote Response
// ============================================================================

int raft_recv_requestvote_response(raft_t *r,
                                   int peer_id,
                                   const raft_requestvote_resp_t *resp) {
    if (!r || !resp) {
        return RAFT_ERR_INVALID_ARG;
    }

    // Ignore if we're not a candidate anymore
    if (r->state != RAFT_STATE_CANDIDATE) {
        return RAFT_OK;
    }

    // If response contains higher term, step down
    if (resp->term > r->current_term) {
        raft_become_follower(r, resp->term);
        return RAFT_OK;
    }

    // Ignore stale responses
    if (resp->term < r->current_term) {
        return RAFT_OK;
    }

    // Count vote
    if (resp->vote_granted) {
        if (!r->votes_for_me[peer_id]) {
            r->votes_for_me[peer_id] = 1;
            r->votes_received++;

            printf("[Node %d] Received vote from %d (%d/%d)\n",
                   r->my_id, peer_id, r->votes_received, r->num_nodes);

            // Check if we have majority
            int majority = (r->num_nodes / 2) + 1;
            if (r->votes_received >= majority) {
                raft_become_leader(r);
            }
        }
    }

    return RAFT_OK;
}