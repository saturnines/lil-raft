#include "../src/raft_internal.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>

#define TEST(name) \
    do { \
        printf("  %-40s ", #name); \
        if (test_##name()) { \
            printf("✓\n"); \
        } else { \
            printf("✗\n"); \
            return 1; \
        } \
    } while(0)

static int test_init_destroy(void) {
    raft_log_t log;
    assert(raft_log_init(&log) == RAFT_OK);
    assert(log.count == 0);
    assert(log.capacity > 0);
    assert(raft_log_is_empty(&log));
    raft_log_free(&log);
    return 1;
}

static int test_append_single(void) {
    raft_log_t log;
    raft_log_init(&log);
    
    const char *data = "hello";
    assert(raft_log_append(&log, 1, data, strlen(data)) == RAFT_OK);
    
    assert(log.count == 1);
    assert(raft_log_last_index(&log) == 1);
    assert(raft_log_last_term(&log) == 1);
    assert(!raft_log_is_empty(&log));
    
    raft_log_free(&log);
    return 1;
}

static int test_append_multiple(void) {
    raft_log_t log;
    raft_log_init(&log);
    
    for (int i = 0; i < 10; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "entry-%d", i);
        assert(raft_log_append(&log, 1, buf, strlen(buf)) == RAFT_OK);
    }
    
    assert(log.count == 10);
    assert(raft_log_last_index(&log) == 10);
    
    raft_log_free(&log);
    return 1;
}

static int test_get(void) {
    raft_log_t log;
    raft_log_init(&log);
    
    raft_log_append(&log, 1, "one", 3);
    raft_log_append(&log, 2, "two", 3);
    raft_log_append(&log, 2, "three", 5);
    
    raft_entry_t *e = raft_log_get(&log, 2);
    assert(e != NULL);
    assert(e->index == 2);
    assert(e->term == 2);
    assert(e->len == 3);
    assert(memcmp(e->data, "two", 3) == 0);
    
    raft_log_free(&log);
    return 1;
}

static int test_truncate(void) {
    raft_log_t log;
    raft_log_init(&log);
    
    for (int i = 0; i < 10; i++) {
        raft_log_append(&log, 1, "data", 4);
    }
    
    assert(log.count == 10);
    
    // Truncate after index 5
    raft_log_truncate_after(&log, 5);
    assert(log.count == 5);
    assert(raft_log_last_index(&log) == 5);
    
    raft_log_free(&log);
    return 1;
}

static int test_batch_append(void) {
    raft_log_t log;
    raft_log_init(&log);
    
    raft_entry_t entries[3] = {
        {.term = 1, .type = RAFT_ENTRY_DATA, .data = "a", .len = 1},
        {.term = 1, .type = RAFT_ENTRY_DATA, .data = "b", .len = 1},
        {.term = 2, .type = RAFT_ENTRY_DATA, .data = "c", .len = 1}
    };

    assert(raft_log_append_batch(&log, entries, 3) == RAFT_OK);
    assert(log.count == 3);
    assert(raft_log_last_term(&log) == 2);

    raft_log_free(&log);
    return 1;
}

static int test_get_empty_log(void) {
    raft_log_t log;
    raft_log_init(&log);

    // Get on empty log
    assert(raft_log_get(&log, 1) == NULL);
    assert(raft_log_get(&log, 0) == NULL);

    raft_log_free(&log);
    return 1;
}

static int test_last_index_empty(void) {
    raft_log_t log;
    raft_log_init(&log);

    assert(raft_log_last_index(&log) == 0);
    assert(raft_log_last_term(&log) == 0);
    assert(raft_log_is_empty(&log));

    raft_log_free(&log);
    return 1;
}

static int test_zero_length_entry(void) {
    raft_log_t log;
    raft_log_init(&log);

    // Append zero length entry I believe that malloc is weird
    const char *data = "x";  // Non-NULL pointer
    assert(raft_log_append(&log, 1, data, 0) == RAFT_OK);

    assert(log.count == 1);
    raft_entry_t *e = raft_log_get(&log, 1);
    assert(e != NULL);
    assert(e->len == 0);
    // do not assert on e->data - malloc(0) is implementation defined

    raft_log_free(&log);
    return 1;
}

static int test_batch_with_zero_length(void) {
    raft_log_t log;
    raft_log_init(&log);

    raft_entry_t entries[3] = {
        {.term = 1, .type = RAFT_ENTRY_DATA, .data = "a", .len = 1},
        {.term = 1, .type = RAFT_ENTRY_DATA, .data = "b", .len = 0},  // Zero length
        {.term = 2, .type = RAFT_ENTRY_DATA, .data = "c", .len = 1}
    };

    assert(raft_log_append_batch(&log, entries, 3) == RAFT_OK);
    assert(log.count == 3);

    raft_log_free(&log);
    return 1;
}

static int test_truncate_to_zero(void) {
    raft_log_t log;
    raft_log_init(&log);

    raft_log_append(&log, 1, "data", 4);
    raft_log_append(&log, 1, "data", 4);

    // Truncate to 0, should clear everything
    raft_log_truncate_after(&log, 0);
    assert(log.count == 0);
    assert(raft_log_is_empty(&log));

    raft_log_free(&log);
    return 1;
}

static int test_truncate_past_end(void) {
    raft_log_t log;
    raft_log_init(&log);

    raft_log_append(&log, 1, "data", 4);
    raft_log_append(&log, 1, "data", 4);

    // Truncate past end  should be no-op
    raft_log_truncate_after(&log, 100);
    assert(log.count == 2);

    raft_log_free(&log);
    return 1;
}

static int test_truncate_empty_log(void) {
    raft_log_t log;
    raft_log_init(&log);

    // Truncate empty log  should not crash
    assert(raft_log_truncate_after(&log, 5) == RAFT_OK);
    assert(log.count == 0);

    raft_log_free(&log);
    return 1;
}

static int test_pointer_invalidation(void) {
    raft_log_t log;
    raft_log_init(&log);

    raft_log_append(&log, 1, "one", 3);
    raft_log_append(&log, 1, "two", 3);
    raft_log_append(&log, 1, "three", 5);

    raft_entry_t *e = raft_log_get(&log, 2);
    assert(e != NULL);

    // Truncate: pointer 'e' is now invalid
    raft_log_truncate_after(&log, 1);

    // Just verify log state because e is dangling
    assert(log.count == 1);
    assert(raft_log_get(&log, 2) == NULL);

    raft_log_free(&log);
    return 1;
}

static int test_double_free(void) {
    raft_log_t log;
    raft_log_init(&log);

    raft_log_append(&log, 1, "data", 4);

    raft_log_free(&log);
    raft_log_free(&log);  // Should not crash (entries == NULL check)

    return 1;
}

static int test_free_then_append(void) {
    raft_log_t log;
    raft_log_init(&log);

    raft_log_free(&log);

    // Append without re-init, should fail gracefully
    // This will likely crash or return error
    // For now, just document expected behavior

    return 1;
}

static int test_get_at_base_index(void) {
    raft_log_t log;
    raft_log_init(&log);
    log.base_index = 10;  // Simulate compaction

    raft_log_append(&log, 1, "data", 4);  // This is index 11

    assert(raft_log_get(&log, 10) == NULL);  // At base
    assert(raft_log_get(&log, 11) != NULL);  // First entry

    raft_log_free(&log);
    return 1;
}

int main(void) {
    printf("\n=== raft_log.c tests ===\n\n");
    
    printf("Basic:\n");
    TEST(init_destroy);
    TEST(append_single);
    TEST(append_multiple);
    TEST(get);
    TEST(truncate);
    TEST(batch_append);

    printf("\nEdge Cases:\n");
    TEST(get_empty_log);
    TEST(last_index_empty);
    TEST(zero_length_entry);
    TEST(batch_with_zero_length);

    printf("\nTruncation:\n");
    TEST(truncate_to_zero);
    TEST(truncate_past_end);
    TEST(truncate_empty_log);
    TEST(pointer_invalidation);

    printf("\nLifecycle:\n");
    TEST(double_free);
    TEST(free_then_append);
    TEST(get_at_base_index);
    
    printf("\nAll tests passed! ✓\n\n");
    return 0;
}