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
 * @param config      Configuration (NULL for defaults)
 * @param callbacks   User-provided callbacks
 * @param callback_ctx Context passed to all callbacks
 * @return Raft handle, or NULL on error
 *
 * Example:
 *   // Use defaults
 *   raft_t *r = raft_create(0, 3, NULL, &callbacks, ctx);
 *
 *   // Custom config
 *   raft_config_t cfg = RAFT_CONFIG_DEFAULT;
 *   cfg.election_timeout_min_ms = 200;
 *   cfg.flags &= ~RAFT_FLAG_PREVOTE_ENABLED;
 *   raft_t *r = raft_create(0, 3, &cfg, &callbacks, ctx);
 */
raft_t* raft_create(int my_id,
                    int num_nodes,
                    const raft_config_t *config,
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
// Configuration
// ============================================================================

/**
 * Set feature flags at runtime
 * See RAFT_FLAG_* constants in raft_types.h
 *
 * Note: Some flags may not take effect until next election cycle.
 *
 * @param flags Bitmask of RAFT_FLAG_* values
 */
void raft_set_flags(raft_t *r, uint32_t flags);

/**
 * Get current feature flags
 * @return Current flags bitmask
 */
uint32_t raft_get_flags(const raft_t *r);

/**
 * Get current configuration (read-only)
 * @return Pointer to internal config, or NULL if r is NULL
 */
const raft_config_t* raft_get_config(const raft_t *r);

// ============================================================================
// Periodic Tick
// ============================================================================

/**
 * Periodic tick - drives election timeouts and heartbeats
 * - Checks election timeout (followers/candidates)
 * - Sends heartbeats (leaders)
 * - Advances commit index
 * - Applies committed entries
 * - Triggers auto-snapshot if enabled and threshold exceeded
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
// Read Synchronization (NOOP-based)
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
 *         RAFT_ERR_NOT*_LEADER if not leader
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

/**
 * Get pending write index for ALR piggyback optimization
 *
 * Returns the last proposed index from current term if there are
 * uncommitted entries in flight.
 *
 * @return Last pending index from current term, or 0 if none
 */
uint64_t raft_get_pending_index(const raft_t *r);

// ============================================================================
// Snapshot Operations
// ============================================================================

/**
 * Manually trigger a snapshot
 *
 * Creates a snapshot at the current applied index. Only snapshots
 * entries that have been committed and applied to the state machine.
 *
 * The snapshot_create callback will be invoked to serialize state.
 * After successful completion, log entries covered by the snapshot
 * are discarded.
 *
 * Note: If AUTO_SNAPSHOT flag is set, snapshots are triggered
 * automatically when the log grows beyond snapshot_threshold.
 *
 * @return RAFT_OK on success
 *         RAFT_ERR_SNAPSHOT_IN_PROGRESS if already snapshotting
 *         RAFT_ERR_SNAPSHOT_FAILED if callback failed
 *         RAFT_ERR_INVALID_ARG if snapshot_create callback not set
 */
int raft_snapshot_create(raft_t *r);

/**
 * Get current snapshot metadata
 *
 * @param last_index Output: last index included in snapshot (0 if none)
 * @param last_term  Output: term of last index (0 if none)
 */
void raft_get_snapshot_info(const raft_t *r,
                            uint64_t *last_index,
                            uint64_t *last_term);

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

/**
 * Handle InstallSnapshot RPC
 *
 * Called when receiving a snapshot chunk from the leader.
 * Used when this node is too far behind to receive normal AppendEntries.
 *
 * @param req  Request from leader (includes snapshot chunk)
 * @param resp Response to send back (filled by this function)
 * @return RAFT_OK on success
 */
int raft_recv_installsnapshot(raft_t *r,
                               const raft_installsnapshot_req_t *req,
                               raft_installsnapshot_resp_t *resp);

/**
 * Handle InstallSnapshot response (from peer)
 * Called when peer responds to our InstallSnapshot
 *
 * @param peer_id Which peer responded
 * @param resp    Their response
 * @return RAFT_OK on success
 */
int raft_recv_installsnapshot_response(raft_t *r,
                                        int peer_id,
                                        const raft_installsnapshot_resp_t *resp);

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

/**
 * Get term of log entry at given index
 * @return Term at index, or 0 if index not in log (compacted/truncated/invalid)
 */
uint64_t raft_log_term_at(const raft_t *r, uint64_t index);


#ifdef __cplusplus
}
#endif

#endif // RAFT_H