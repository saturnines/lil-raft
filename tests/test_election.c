
#include "../src/raft_internal.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

#define TEST(name) \
    do { \
        printf("  %-50s ", #name); \
        if (test_##name()) { \
            printf("✓\n"); \
        } else { \
            printf("✗\n"); \
            return 1; \
        } \
    } while(0)

// ============================================================================
// Mock Infrastructure
// ============================================================================

static int g_persist_called = 0;
static uint64_t g_persisted_term = 0;
static int g_persisted_vote = -1;

static int g_requestvote_sent[10];
static int g_requestvote_count = 0;
static raft_requestvote_req_t g_last_rv_req;

static int mock_persist_vote(void *ctx, uint64_t term, int voted_for) {
    (void)ctx;
    g_persist_called++;
    g_persisted_term = term;
    g_persisted_vote = voted_for;
    return 0;
}

static int mock_load_vote(void *ctx, uint64_t *term, int *voted_for) {
    (void)ctx;
    *term = 0;
    *voted_for = -1;
    return 0;
}

static int mock_send_requestvote(void *ctx, int peer_id,
                                  const raft_requestvote_req_t *req) {
    (void)ctx;
    g_requestvote_sent[peer_id] = 1;
    g_requestvote_count++;
    g_last_rv_req = *req;
    return 0;
}

static void reset_mocks(void) {
    g_persist_called = 0;
    g_persisted_term = 0;
    g_persisted_vote = -1;
    g_requestvote_count = 0;
    memset(g_requestvote_sent, 0, sizeof(g_requestvote_sent));
    memset(&g_last_rv_req, 0, sizeof(g_last_rv_req));
}

static raft_t* make_raft(int my_id, int num_nodes) {
    reset_mocks();
    raft_callbacks_t cb = {
        .persist_vote = mock_persist_vote,
        .load_vote = mock_load_vote,
        .send_requestvote = mock_send_requestvote,
    };
    return raft_create(my_id, num_nodes, &cb, NULL);
}

// ============================================================================
// RequestVote Request Tests
// ============================================================================

static int test_reject_stale_term(void) {
    raft_t *r = make_raft(0, 3);
    r->current_term = 5;

    raft_requestvote_req_t req = {
        .term = 3,  // Stale!
        .candidate_id = 1,
        .last_log_index = 10,
        .last_log_term = 3,
    };
    raft_requestvote_resp_t resp;

    raft_recv_requestvote(r, &req, &resp);

    assert(resp.vote_granted == 0);
    assert(resp.term == 5);
    assert(r->current_term == 5);  // Unchanged
    assert(r->voted_for == -1);    // Didn't vote

    raft_destroy(r);
    return 1;
}

static int test_step_down_on_higher_term(void) {
    raft_t *r = make_raft(0, 3);
    r->current_term = 1;
    r->state = RAFT_STATE_LEADER;  // We're leader

    raft_requestvote_req_t req = {
        .term = 5,  // Higher term!
        .candidate_id = 1,
        .last_log_index = 0,
        .last_log_term = 0,
    };
    raft_requestvote_resp_t resp;

    raft_recv_requestvote(r, &req, &resp);

    assert(r->state == RAFT_STATE_FOLLOWER);  // Stepped down
    assert(r->current_term == 5);
    assert(resp.vote_granted == 1);  // And granted vote

    raft_destroy(r);
    return 1;
}

static int test_candidate_steps_down_on_higher_term(void) {
    raft_t *r = make_raft(0, 3);
    r->current_term = 2;
    r->state = RAFT_STATE_CANDIDATE;
    r->voted_for = 0;  // Voted for self

    raft_requestvote_req_t req = {
        .term = 5,
        .candidate_id = 1,
        .last_log_index = 0,
        .last_log_term = 0,
    };
    raft_requestvote_resp_t resp;

    raft_recv_requestvote(r, &req, &resp);

    assert(r->state == RAFT_STATE_FOLLOWER);
    assert(r->current_term == 5);
    assert(resp.vote_granted == 1);

    raft_destroy(r);
    return 1;
}

static int test_already_voted_same_candidate(void) {
    raft_t *r = make_raft(0, 3);
    r->current_term = 1;
    r->voted_for = 2;  // Already voted for candidate 2

    raft_requestvote_req_t req = {
        .term = 1,
        .candidate_id = 2,  // Same candidate
        .last_log_index = 0,
        .last_log_term = 0,
    };
    raft_requestvote_resp_t resp;

    raft_recv_requestvote(r, &req, &resp);

    assert(resp.vote_granted == 1);  // Can vote again for same candidate

    raft_destroy(r);
    return 1;
}

static int test_already_voted_different_candidate(void) {
    raft_t *r = make_raft(0, 3);
    r->current_term = 1;
    r->voted_for = 2;  // Already voted for candidate 2

    raft_requestvote_req_t req = {
        .term = 1,
        .candidate_id = 1,  // Different candidate!
        .last_log_index = 0,
        .last_log_term = 0,
    };
    raft_requestvote_resp_t resp;

    raft_recv_requestvote(r, &req, &resp);

    assert(resp.vote_granted == 0);  // Rejected
    assert(r->voted_for == 2);       // Still voted for 2

    raft_destroy(r);
    return 1;
}

static int test_reject_outdated_log_lower_term(void) {
    raft_t *r = make_raft(0, 3);
    r->current_term = 1;

    // Our log: index=5, term=3
    for (int i = 0; i < 5; i++) {
        raft_log_append(&r->log, 3, "data", 4);
    }

    raft_requestvote_req_t req = {
        .term = 1,
        .candidate_id = 1,
        .last_log_index = 10,  // Longer but...
        .last_log_term = 2,    // Lower term!
    };
    raft_requestvote_resp_t resp;

    raft_recv_requestvote(r, &req, &resp);

    assert(resp.vote_granted == 0);  // Rejected - our log is more current

    raft_destroy(r);
    return 1;
}

static int test_reject_outdated_log_shorter(void) {
    raft_t *r = make_raft(0, 3);
    r->current_term = 1;

    // Our log: index=5, term=3
    for (int i = 0; i < 5; i++) {
        raft_log_append(&r->log, 3, "data", 4);
    }

    raft_requestvote_req_t req = {
        .term = 1,
        .candidate_id = 1,
        .last_log_index = 3,  // Shorter!
        .last_log_term = 3,   // Same term
    };
    raft_requestvote_resp_t resp;

    raft_recv_requestvote(r, &req, &resp);

    assert(resp.vote_granted == 0);  // Rejected our log is longer

    raft_destroy(r);
    return 1;
}

static int test_accept_more_uptodate_log_higher_term(void) {
    raft_t *r = make_raft(0, 3);
    r->current_term = 1;

    // Our log: index=3, term=2
    for (int i = 0; i < 3; i++) {
        raft_log_append(&r->log, 2, "data", 4);
    }

    raft_requestvote_req_t req = {
        .term = 1,
        .candidate_id = 1,
        .last_log_index = 2,  // Shorter but...
        .last_log_term = 3,   // Higher term!
    };
    raft_requestvote_resp_t resp;

    raft_recv_requestvote(r, &req, &resp);

    assert(resp.vote_granted == 1);  // Accepted - their term is higher

    raft_destroy(r);
    return 1;
}

static int test_accept_equal_log(void) {
    raft_t *r = make_raft(0, 3);
    r->current_term = 1;

    for (int i = 0; i < 5; i++) {
        raft_log_append(&r->log, 3, "data", 4);
    }

    raft_requestvote_req_t req = {
        .term = 1,
        .candidate_id = 1,
        .last_log_index = 5,  // Same
        .last_log_term = 3,   // Same
    };
    raft_requestvote_resp_t resp;

    raft_recv_requestvote(r, &req, &resp);

    assert(resp.vote_granted == 1);  // Equal is okay

    raft_destroy(r);
    return 1;
}

static int test_accept_longer_log_same_term(void) {
    raft_t *r = make_raft(0, 3);
    r->current_term = 1;

    for (int i = 0; i < 3; i++) {
        raft_log_append(&r->log, 2, "data", 4);
    }

    raft_requestvote_req_t req = {
        .term = 1,
        .candidate_id = 1,
        .last_log_index = 10,  // Longer
        .last_log_term = 2,    // Same term
    };
    raft_requestvote_resp_t resp;

    raft_recv_requestvote(r, &req, &resp);

    assert(resp.vote_granted == 1);

    raft_destroy(r);
    return 1;
}

static int test_vote_persisted(void) {
    raft_t *r = make_raft(0, 3);
    r->current_term = 1;

    raft_requestvote_req_t req = {
        .term = 1,
        .candidate_id = 2,
        .last_log_index = 0,
        .last_log_term = 0,
    };
    raft_requestvote_resp_t resp;

    reset_mocks();
    raft_recv_requestvote(r, &req, &resp);

    assert(resp.vote_granted == 1);
    assert(g_persist_called > 0);
    assert(g_persisted_vote == 2);

    raft_destroy(r);
    return 1;
}

static int test_election_timer_reset_on_vote(void) {
    raft_t *r = make_raft(0, 3);
    r->current_term = 1;
    r->last_heartbeat_ms = 0;  // Long time ago

    raft_requestvote_req_t req = {
        .term = 1,
        .candidate_id = 1,
        .last_log_index = 0,
        .last_log_term = 0,
    };
    raft_requestvote_resp_t resp;

    raft_recv_requestvote(r, &req, &resp);

    assert(resp.vote_granted == 1);
    assert(r->last_heartbeat_ms > 0);  // Timer was reset

    raft_destroy(r);
    return 1;
}

static int test_empty_log_comparison(void) {
    raft_t *r = make_raft(0, 3);
    r->current_term = 1;
    // Empty log

    raft_requestvote_req_t req = {
        .term = 1,
        .candidate_id = 1,
        .last_log_index = 0,  // Also empty
        .last_log_term = 0,
    };
    raft_requestvote_resp_t resp;

    raft_recv_requestvote(r, &req, &resp);

    assert(resp.vote_granted == 1);  // Both empty - okay

    raft_destroy(r);
    return 1;
}

static int test_candidate_empty_vs_nonempty(void) {
    raft_t *r = make_raft(0, 3);
    r->current_term = 1;

    // We have entries
    raft_log_append(&r->log, 1, "data", 4);

    raft_requestvote_req_t req = {
        .term = 1,
        .candidate_id = 1,
        .last_log_index = 0,  // Empty!
        .last_log_term = 0,
    };
    raft_requestvote_resp_t resp;

    raft_recv_requestvote(r, &req, &resp);

    assert(resp.vote_granted == 0);  // Reject!  we have entries, they don't

    raft_destroy(r);
    return 1;
}

static int test_new_term_clears_vote(void) {
    raft_t *r = make_raft(0, 3);
    r->current_term = 1;
    r->voted_for = 2;  // Already voted

    // Higher term should clear voted_for
    raft_requestvote_req_t req = {
        .term = 2,  // New term
        .candidate_id = 1,
        .last_log_index = 0,
        .last_log_term = 0,
    };
    raft_requestvote_resp_t resp;

    raft_recv_requestvote(r, &req, &resp);

    assert(resp.vote_granted == 1);  // Can vote in new term
    assert(r->voted_for == 1);

    raft_destroy(r);
    return 1;
}

// ============================================================================
// RequestVote Response Tests
// ============================================================================

static int test_response_ignore_if_not_candidate(void) {
    raft_t *r = make_raft(0, 3);
    r->state = RAFT_STATE_FOLLOWER;

    raft_requestvote_resp_t resp = {
        .term = 1,
        .vote_granted = 1,
    };

    int ret = raft_recv_requestvote_response(r, 1, &resp);

    assert(ret == RAFT_OK);
    assert(r->state == RAFT_STATE_FOLLOWER);  // Still follower

    raft_destroy(r);
    return 1;
}

static int test_response_ignore_if_leader(void) {
    raft_t *r = make_raft(0, 3);
    r->state = RAFT_STATE_LEADER;
    r->current_term = 1;

    raft_requestvote_resp_t resp = {
        .term = 1,
        .vote_granted = 1,
    };

    raft_recv_requestvote_response(r, 1, &resp);

    assert(r->state == RAFT_STATE_LEADER);  // Still leader

    raft_destroy(r);
    return 1;
}

static int test_response_step_down_higher_term(void) {
    raft_t *r = make_raft(0, 3);
    r->state = RAFT_STATE_CANDIDATE;
    r->current_term = 1;

    raft_requestvote_resp_t resp = {
        .term = 5,  // Higher!
        .vote_granted = 0,
    };

    raft_recv_requestvote_response(r, 1, &resp);

    assert(r->state == RAFT_STATE_FOLLOWER);
    assert(r->current_term == 5);

    raft_destroy(r);
    return 1;
}

static int test_response_ignore_stale(void) {
    raft_t *r = make_raft(0, 3);
    r->state = RAFT_STATE_CANDIDATE;
    r->current_term = 5;
    r->votes_received = 1;

    raft_requestvote_resp_t resp = {
        .term = 3,  // Stale
        .vote_granted = 1,
    };

    raft_recv_requestvote_response(r, 1, &resp);

    assert(r->votes_received == 1);  // Not counted

    raft_destroy(r);
    return 1;
}

static int test_response_count_vote(void) {
    raft_t *r = make_raft(0, 3);
    r->state = RAFT_STATE_CANDIDATE;
    r->current_term = 1;
    r->votes_received = 1;  // Self vote
    r->votes_for_me[0] = 1;

    raft_requestvote_resp_t resp = {
        .term = 1,
        .vote_granted = 1,
    };

    raft_recv_requestvote_response(r, 1, &resp);

    assert(r->votes_received == 2);
    assert(r->votes_for_me[1] == 1);

    raft_destroy(r);
    return 1;
}

static int test_response_no_double_count(void) {
    raft_t *r = make_raft(0, 3);
    r->state = RAFT_STATE_CANDIDATE;
    r->current_term = 1;
    r->votes_received = 1;
    r->votes_for_me[0] = 1;

    raft_requestvote_resp_t resp = {
        .term = 1,
        .vote_granted = 1,
    };

    // Same peer votes twice (network duplicate)
    raft_recv_requestvote_response(r, 1, &resp);
    raft_recv_requestvote_response(r, 1, &resp);

    assert(r->votes_received == 2);  // Not 3!

    raft_destroy(r);
    return 1;
}

static int test_response_become_leader_on_majority_3(void) {
    raft_t *r = make_raft(0, 3);
    r->state = RAFT_STATE_CANDIDATE;
    r->current_term = 1;
    r->votes_received = 1;  // Self
    r->votes_for_me[0] = 1;

    raft_requestvote_resp_t resp = {
        .term = 1,
        .vote_granted = 1,
    };

    // Get second vote -> majority in 3 node cluster
    raft_recv_requestvote_response(r, 1, &resp);

    assert(r->state == RAFT_STATE_LEADER);

    raft_destroy(r);
    return 1;
}

static int test_response_need_majority_5_nodes(void) {
    raft_t *r = make_raft(0, 5);
    r->state = RAFT_STATE_CANDIDATE;
    r->current_term = 1;
    r->votes_received = 1;  // Self
    r->votes_for_me[0] = 1;

    raft_requestvote_resp_t resp = {
        .term = 1,
        .vote_granted = 1,
    };

    // 2 votes - not enough for 5 node cluster
    raft_recv_requestvote_response(r, 1, &resp);
    assert(r->state == RAFT_STATE_CANDIDATE);
    assert(r->votes_received == 2);

    // 3 votes =-= majority!
    raft_recv_requestvote_response(r, 2, &resp);
    assert(r->state == RAFT_STATE_LEADER);

    raft_destroy(r);
    return 1;
}

static int test_response_need_majority_7_nodes(void) {
    raft_t *r = make_raft(0, 7);
    r->state = RAFT_STATE_CANDIDATE;
    r->current_term = 1;
    r->votes_received = 1;
    r->votes_for_me[0] = 1;

    raft_requestvote_resp_t resp = {
        .term = 1,
        .vote_granted = 1,
    };

    // Need 4 votes majority for 7 node cluster
    raft_recv_requestvote_response(r, 1, &resp);
    assert(r->state == RAFT_STATE_CANDIDATE);
    raft_recv_requestvote_response(r, 2, &resp);
    assert(r->state == RAFT_STATE_CANDIDATE);
    raft_recv_requestvote_response(r, 3, &resp);
    assert(r->state == RAFT_STATE_LEADER);  // 4 votes

    raft_destroy(r);
    return 1;
}

static int test_response_rejected_vote_not_counted(void) {
    raft_t *r = make_raft(0, 3);
    r->state = RAFT_STATE_CANDIDATE;
    r->current_term = 1;
    r->votes_received = 1;
    r->votes_for_me[0] = 1;

    raft_requestvote_resp_t resp = {
        .term = 1,
        .vote_granted = 0,  // Rejected!
    };

    raft_recv_requestvote_response(r, 1, &resp);

    assert(r->votes_received == 1);  // Not incremented
    assert(r->state == RAFT_STATE_CANDIDATE);

    raft_destroy(r);
    return 1;
}

static int test_response_all_reject_stay_candidate(void) {
    raft_t *r = make_raft(0, 3);
    r->state = RAFT_STATE_CANDIDATE;
    r->current_term = 1;
    r->votes_received = 1;
    r->votes_for_me[0] = 1;

    raft_requestvote_resp_t resp = {
        .term = 1,
        .vote_granted = 0,
    };

    raft_recv_requestvote_response(r, 1, &resp);
    raft_recv_requestvote_response(r, 2, &resp);

    assert(r->state == RAFT_STATE_CANDIDATE);  // Still candidate
    assert(r->votes_received == 1);  // Only self vote

    raft_destroy(r);
    return 1;
}

// ============================================================================
// Send RequestVote Tests
// ============================================================================

static int test_send_to_all_peers(void) {
    raft_t *r = make_raft(0, 5);
    r->state = RAFT_STATE_CANDIDATE;
    r->current_term = 1;

    reset_mocks();
    raft_send_requestvote_all(r);

    assert(g_requestvote_count == 4);  // 5 nodes - 1 (self)
    assert(g_requestvote_sent[0] == 0);  // Didn't send to self
    assert(g_requestvote_sent[1] == 1);
    assert(g_requestvote_sent[2] == 1);
    assert(g_requestvote_sent[3] == 1);
    assert(g_requestvote_sent[4] == 1);

    raft_destroy(r);
    return 1;
}

static int test_send_only_as_candidate(void) {
    raft_t *r = make_raft(0, 3);
    r->state = RAFT_STATE_FOLLOWER;

    reset_mocks();
    raft_send_requestvote_all(r);

    assert(g_requestvote_count == 0);

    r->state = RAFT_STATE_LEADER;
    raft_send_requestvote_all(r);

    assert(g_requestvote_count == 0);

    raft_destroy(r);
    return 1;
}

static int test_send_includes_log_info(void) {
    raft_t *r = make_raft(0, 3);
    r->state = RAFT_STATE_CANDIDATE;
    r->current_term = 5;

    raft_log_append(&r->log, 2, "a", 1);
    raft_log_append(&r->log, 3, "b", 1);
    raft_log_append(&r->log, 3, "c", 1);

    reset_mocks();
    raft_send_requestvote_all(r);

    assert(g_last_rv_req.term == 5);
    assert(g_last_rv_req.candidate_id == 0);
    assert(g_last_rv_req.last_log_index == 3);
    assert(g_last_rv_req.last_log_term == 3);

    raft_destroy(r);
    return 1;
}

static int test_send_empty_log(void) {
    raft_t *r = make_raft(0, 3);
    r->state = RAFT_STATE_CANDIDATE;
    r->current_term = 1;

    reset_mocks();
    raft_send_requestvote_all(r);

    assert(g_last_rv_req.last_log_index == 0);
    assert(g_last_rv_req.last_log_term == 0);

    raft_destroy(r);
    return 1;
}

// ============================================================================
// Split Vote / Edge Cases
// ============================================================================

static int test_single_node_cluster(void) {
    raft_t *r = make_raft(0, 1);
    r->state = RAFT_STATE_CANDIDATE;
    r->current_term = 1;
    r->votes_received = 1;
    r->votes_for_me[0] = 1;

    // Single node, already has majority
    // (This would typically be handled by become_candidate)
    int majority = (1 / 2) + 1;  // = 1
    assert(r->votes_received >= majority);

    raft_destroy(r);
    return 1;
}

static int test_two_node_cluster(void) {
    raft_t *r = make_raft(0, 2);
    r->state = RAFT_STATE_CANDIDATE;
    r->current_term = 1;
    r->votes_received = 1;
    r->votes_for_me[0] = 1;

    // 2 nodes - need 2 votes
    int majority = (2 / 2) + 1;  // = 2
    assert(r->votes_received < majority);

    raft_requestvote_resp_t resp = { .term = 1, .vote_granted = 1 };
    raft_recv_requestvote_response(r, 1, &resp);

    assert(r->state == RAFT_STATE_LEADER);

    raft_destroy(r);
    return 1;
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("\n=== raft_election.c tests ===\n\n");

    printf("RequestVote Request:\n");
    TEST(reject_stale_term);
    TEST(step_down_on_higher_term);
    TEST(candidate_steps_down_on_higher_term);
    TEST(already_voted_same_candidate);
    TEST(already_voted_different_candidate);
    TEST(reject_outdated_log_lower_term);
    TEST(reject_outdated_log_shorter);
    TEST(accept_more_uptodate_log_higher_term);
    TEST(accept_equal_log);
    TEST(accept_longer_log_same_term);
    TEST(vote_persisted);
    TEST(election_timer_reset_on_vote);
    TEST(empty_log_comparison);
    TEST(candidate_empty_vs_nonempty);
    TEST(new_term_clears_vote);

    printf("\nRequestVote Response:\n");
    TEST(response_ignore_if_not_candidate);
    TEST(response_ignore_if_leader);
    TEST(response_step_down_higher_term);
    TEST(response_ignore_stale);
    TEST(response_count_vote);
    TEST(response_no_double_count);
    TEST(response_become_leader_on_majority_3);
    TEST(response_need_majority_5_nodes);
    TEST(response_need_majority_7_nodes);
    TEST(response_rejected_vote_not_counted);
    TEST(response_all_reject_stay_candidate);

    printf("\nSend RequestVote:\n");
    TEST(send_to_all_peers);
    TEST(send_only_as_candidate);
    TEST(send_includes_log_info);
    TEST(send_empty_log);

    printf("\nEdge Cases:\n");
    TEST(single_node_cluster);
    TEST(two_node_cluster);

    printf("\nAll election tests passed! ✓\n\n");
    return 0;
}