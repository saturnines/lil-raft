#include "../src/raft_internal.h"
#include <stdio.h>
#include <assert.h>
#include <unistd.h>

// Mock callbacks
static int mock_persist_vote(void *ctx, uint64_t term, int voted_for) {
    (void)ctx; (void)term; (void)voted_for;
    return 0;
}

static int mock_load_vote(void *ctx, uint64_t *term, int *voted_for) {
    (void)ctx;
    *term = 0;
    *voted_for = -1;
    return 0;
}

int main(void) {
    printf("\n=== raft.c core tests ===\n\n");
    
    // Setup callbacks
    raft_callbacks_t callbacks = {
        .persist_vote = mock_persist_vote,
        .load_vote = mock_load_vote,
    };
    
    // Test 1: Create and destroy
    printf("Test: create/destroy... ");
    raft_t *r = raft_create(0, 3, &callbacks, NULL);
    assert(r != NULL);
    assert(r->my_id == 0);
    assert(r->num_nodes == 3);
    assert(r->state == RAFT_STATE_FOLLOWER);
    assert(r->current_term == 0);
    assert(r->voted_for == -1);
    printf("PASS\n");

    // Test 2: Tick causes election timeout
    printf("Test: election timeout... ");

    int max_ticks = 200; // 2s budget
    while (max_ticks-- > 0 && r->state == RAFT_STATE_FOLLOWER) {
        raft_tick(r);
        usleep(10000);
    }

    assert(r->state == RAFT_STATE_CANDIDATE);
    assert(r->current_term == 1);
    assert(r->voted_for == 0);
    printf("PASS\n");

    // Test 2b: Candidate shouldn’t instantly restart elections
    printf("Test: candidate stability... ");
    uint64_t t = r->current_term;
    for (int i = 0; i < 10; i++) {  // 100ms total (still < 150ms min timeout)
        raft_tick(r);
        usleep(10000);
    }
    assert(r->current_term == t);
    printf("PASS\n");

    // Test 3: State queries
    printf("Test: state queries... ");
    assert(!raft_is_leader(r));
    assert(raft_get_term(r) == t);
    assert(raft_get_node_id(r) == 0);
    printf("PASS\n");


    
    raft_destroy(r);
    
    printf("\nAll core tests passed!\n\n");
    return 0;
}