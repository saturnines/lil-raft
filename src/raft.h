/**
 * raft.h - Public API for lil-raft
 *
 */

#ifndef RAFT_H
#define RAFT_H

#include "raft_types.h"
#include "raft_errors.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque Raft handle
typedef struct raft raft_t;

// ============================================================================
// Lifecycle
// ============================================================================

/**
 * Create Raft instance
 *
 * @param my_id       This node's ID (0-based)
 * @param num_nodes   Total number of nodes in cluster
 * @param callbacks   User-provided callbacks
 * @param callback_ctx Context passed to all callbacks
 * @return Raft handle, or NULL on error
 */
raft_t* raft_create(int my_id,
                    int num_nodes,
                    const raft_callbacks_t *callbacks,
                    void *callback_ctx);

/**
 * Destroy Raft instance
 * Does NOT flush pending entries you must call raft_shutdown() first
 */
void raft_destroy(raft_t *r);

/**
 * Graceful shutdown
 * Flushes pending operations, waits for current in flight RPCs
 *
 * @return RAFT_OK on success
 */
int raft_shutdown(raft_t *r);

// ============================================================================
// Periodic Tick
// ============================================================================

/**
 * Periodic tick - drives election timeouts and heartbeats
 * - Checks election timeout (followers/candidates)
 * - Sends heartbeats (leaders)
 * - Advances commit index
 * - Applies committed entries
 */
void raft_tick(raft_t *r);

// ============================================================================
// Client Operations
// ============================================================================

/**
 * Propose new entry (client write request)
 *
 * Only succeeds if this node is the leader.
 * Entry is replicated to followers and applied when committed.
 *
 * @param data User data to replicate
 * @param len  Data length
 * @return RAFT_OK if accepted (not yet committed!)
 *         RAFT_ERR_NOT_LEADER if not leader
 *         RAFT_ERR_SHUTDOWN if shutting down
 */
int raft_propose(raft_t *r, const void *data, size_t len);

/**
 * Propose multiple entries at once (batch optimization)
 *
 * More efficient than calling raft_propose() multiple times:
 * - Single AppendEntries RPC contains all entries
 * - Single fsync if log_append_batch() callback is implemented
 * - Reduces network round trips
 *
 * Recommended for high-throughput workloads.
 *
 * @param entries Array of data pointers
 * @param lengths Array of corresponding lengths
 * @param count   Number of entries
 * @return RAFT_OK if accepted
 *         RAFT_ERR_NOT_LEADER if not leader
 *         RAFT_ERR_INVALID_ARG if count == 0
 */
int raft_propose_batch(raft_t *r,
                       const void **entries,
                       const size_t *lengths,
                       size_t count);

// ============================================================================
// Linearizable Reads (This is only for Read queries)
// ============================================================================

/**
 * Get current read index for linearizable reads
 * 1. Leader records current commit_index
 * 2. Leader sends heartbeat to majority (confirms still leader)
 * 3. Once heartbeat succeeds, reads at commit_index are linearizable
 *
 * This should avoid appending read operations to the log while maintaining
 * linearizability.
 *
 * Usage:
 *   uint64_t read_index;
 *   if (raft_read_index(r, &read_index) == RAFT_OK) {
 *       // Safe to read state at read_index
 *       // (wait for last_applied >= read_index if needed)
 *       return read_local_state();
 *   }
 *
 * @param read_index Output: safe index to read at (if RAFT_OK)
 * @return RAFT_OK if read is safe now
 *         RAFT_ERR_NOT_LEADER if not leader
 */
int raft_read_index(raft_t *r, uint64_t *read_index);

/**
 * Barrier, wait until all preceding writes are committed
 *
 * Ensures that all entries up to current last_log_index are committed
 * before returning. Useful for:
 * - Read-after-write consistency
 * - Ensuring commit point after leader election
 * - Synchronization points
 *
 * This is a blocking operation that may take multiple heartbeat rounds.
 *
 * @return RAFT_OK when barrier passes (all entries committed)
 *         RAFT_ERR_NOT_LEADER if not leader
 */
int raft_barrier(raft_t *r);

// ============================================================================
// Advanced Read Operations
// ============================================================================

/**
 * Propose NOOP entry for read synchronization
 *
 * Appends a NOOP entry to log and returns its index.
 * When this index is applied, reads are guaranteed linearizable.
 *
 * Useful for implementing optimized read paths that need occasional
 * synchronization with the log without going through full consensus.
 * Multiple concurrent reads can share one NOOP.
 *
 * @param sync_index Output: index of NOOP entry
 * @return RAFT_OK if accepted
 *         RAFT_ERR_NOT_LEADER if not leader
 */
int raft_propose_noop(raft_t *r, uint64_t *sync_index);

/**
 * Check if index has been applied locally
 *
 * Used to wait for operations to be reflected in local state machine.
 *
 * @param index Index to check
 * @return 1 if applied, 0 if not yet applied
 */
int raft_is_applied(const raft_t *r, uint64_t index);

/**
 * Get commit and applied indexes atomically
 *
 * Useful for implementing read optimizations that need to check
 * if the node is caught up with committed state.
 *
 * @param commit_index Output: current commit index
 * @param applied_index Output: current applied index
 */
void raft_get_indexes(const raft_t *r,
                      uint64_t *commit_index,
                      uint64_t *applied_index);

// ============================================================================
// RPC Handlers (called when receiving network messages)
// ============================================================================

/**
 * Handle RequestVote RPC
 *
 * @param req  Request from candidate
 * @param resp Response to send back (filled by this function)
 * @return RAFT_OK on success
 */
int raft_recv_requestvote(raft_t *r,
                          const raft_requestvote_req_t *req,
                          raft_requestvote_resp_t *resp);

/**
 * Handle AppendEntries RPC
 *
 * @param req       Request from leader
 * @param entries   Array of entries (NULL for heartbeat)
 * @param n_entries Number of entries
 * @param resp      Response to send back (filled by this function)
 * @return RAFT_OK on success
 */
int raft_recv_appendentries(raft_t *r,
                            const raft_appendentries_req_t *req,
                            const raft_entry_t *entries,
                            size_t n_entries,
                            raft_appendentries_resp_t *resp);

/**
 * Handle RequestVote response (from peer)
 * Called when peer responds to our RequestVote
 *
 * @param peer_id Which peer responded
 * @param resp    Their response
 * @return RAFT_OK on success
 */
int raft_recv_requestvote_response(raft_t *r,
                                   int peer_id,
                                   const raft_requestvote_resp_t *resp);

/**
 * Handle AppendEntries response (from peer)
 * Called when peer responds to our AppendEntries
 *
 * @param peer_id Which peer responded
 * @param resp    Their response
 * @return RAFT_OK on success
 */
int raft_recv_appendentries_response(raft_t *r,
                                     int peer_id,
                                     const raft_appendentries_resp_t *resp);

// ============================================================================
// State Queries
// ============================================================================

/**
 * Check if this node is the leader
 * @return 1 if leader, 0 otherwise
 */
int raft_is_leader(const raft_t *r);

/**
 * Get current term
 */
uint64_t raft_get_term(const raft_t *r);

/**
 * Get commit index (highest index known to be committed)
 */
uint64_t raft_get_commit_index(const raft_t *r);

/**
 * Get last applied index (highest index applied to state machine)
 */
uint64_t raft_get_last_applied(const raft_t *r);

/**
 * Get current leader ID
 * @return Leader ID, or -1 if unknown
 */
int raft_get_leader_id(const raft_t *r);

/**
 * Get node ID
 */
int raft_get_node_id(const raft_t *r);

#ifdef __cplusplus
}
#endif

#endif // RAFT_H