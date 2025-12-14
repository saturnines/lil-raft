/**
 * raft_types.h - Public types for lil-raft
 */

#ifndef RAFT_TYPES_H
#define RAFT_TYPES_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// RequestVote RPC
// ============================================================================

typedef struct {
    uint64_t term;           // Candidate's term
    int candidate_id;        // Candidate requesting vote
    uint64_t last_log_index; // Index of candidate's last log entry
    uint64_t last_log_term;  // Term of candidate's last log entry
} raft_requestvote_req_t;

typedef struct {
    uint64_t term;        // Current term, for candidate to update itself
    int vote_granted;     // 1 if candidate received vote, 0 otherwise
} raft_requestvote_resp_t;

// ============================================================================
// AppendEntries RPC
// ============================================================================

typedef struct {
    uint64_t term;           // Leader's term
    int leader_id;           // So follower can redirect clients
    uint64_t prev_log_index; // Index of log entry immediately preceding new ones
    uint64_t prev_log_term;  // Term of prev_log_index entry
    uint64_t leader_commit;  // Leader's commit_index
} raft_appendentries_req_t;

typedef struct {
    uint64_t term;        // Current term, for leader to update itself
    int success;          // 1 if follower contained entry matching prev_log_index/term
    uint64_t match_index; // Highest index known to be replicated (for fast rollback)
} raft_appendentries_resp_t;

// ============================================================================
// Log Entry
// ============================================================================

typedef struct {
    uint64_t index;  // Raft log index
    uint64_t term;   // Term when entry was received by leader
    void *data;      // User data (payload)
    size_t len;      // Data length
} raft_entry_t;

// ============================================================================
// Callbacks
// ============================================================================

typedef struct {
    // --- Persistent State (must survive crashes) ---

    /**
     * Persist current term and voted_for to stable storage
     * Called before responding to RequestVote or starting election
     *
     * @return 0 on success, non-zero on error
     */
    int (*persist_vote)(void *ctx, uint64_t term, int voted_for);

    /**
     * Load current term and voted_for from stable storage
     * Called during raft_create()
     *
     * @return 0 on success, non-zero on error
     */
    int (*load_vote)(void *ctx, uint64_t *term, int *voted_for);

    /**
     * Append entry to persistent log
     * Called when leader receives client request or follower receives AppendEntries
     *
     * @return 0 on success, non-zero on error
     */
    int (*log_append)(void *ctx, uint64_t index, uint64_t term,
                      const void *data, size_t len);

    /**
     * Append multiple entries atomically (optional optimization)
     * If NULL, raft will call log_append() for each entry
     * More efficient for batched writes
     *
     * @param entries Array of entries to append
     * @param count   Number of entries
     * @return 0 on success, non-zero on error
     */
    int (*log_append_batch)(void *ctx,
                           const raft_entry_t *entries,
                           size_t count);

    /**
     * Get entry from persistent log
     *
     * @param buf Output buffer (NULL to query size)
     * @param len Input: buffer size, Output: actual entry size
     * @return 0 on success, non-zero if not found or error
     */
    int (*log_get)(void *ctx, uint64_t index, void *buf, size_t *len);

    /**
     * Remove entries after (and including) given index
     * Called when leader detects log conflict
     *
     * @return 0 on success, non-zero on error
     */
    int (*log_truncate_after)(void *ctx, uint64_t index);

    /**
     * Get first index in log (after compaction)
     * @return First index, or 0 if log is empty
     */
    uint64_t (*log_first_index)(void *ctx);

    /**
     * Get last index in log
     * @return Last index, or 0 if log is empty
     */
    uint64_t (*log_last_index)(void *ctx);

    /**
     * Get term of last entry in log
     * @return Last term, or 0 if log is empty
     */
    uint64_t (*log_last_term)(void *ctx);

    // --- State Machine Application ---

    /**
     * Apply committed entry to state machine
     * Called when entry is committed (in order, once per entry)
     *
     * @return 0 on success, non-zero on error
     */
    int (*apply_entry)(void *ctx, uint64_t index, uint64_t term,
                      const void *data, size_t len);

    /**
     * Apply multiple committed entries (optional optimization)
     * If NULL, raft will call apply_entry() for each entry
     * More efficient when committing many entries at once
     *
     * @param entries Array of entries to apply
     * @param count   Number of entries
     * @return 0 on success, non-zero on error
     */
    int (*apply_batch)(void *ctx,
                      const raft_entry_t *entries,
                      size_t count);

    // --- Network Communication ---

    /**
     * Send RequestVote RPC to peer
     * Implementation should be non-blocking
     * Response comes back via raft_recv_requestvote_response()
     *
     * @return 0 if sent, non-zero on error
     */
    int (*send_requestvote)(void *ctx, int peer_id,
                           const raft_requestvote_req_t *req);

    /**
     * Send AppendEntries RPC to peer
     * Implementation should be non-blocking
     * Response comes back via raft_recv_appendentries_response()
     *
     * @param entries Array of entries to send (can be NULL for heartbeat)
     * @param n_entries Number of entries
     * @return 0 if sent, non-zero on error
     */
    int (*send_appendentries)(void *ctx, int peer_id,
                             const raft_appendentries_req_t *req,
                             const raft_entry_t *entries,
                             size_t n_entries);

} raft_callbacks_t;

#ifdef __cplusplus
}
#endif

#endif // RAFT_TYPES_H