#ifndef TASK4_CLIENT_STATE_H
#define TASK4_CLIENT_STATE_H

#include <stddef.h>
#include <limits.h>
#include "task4/checkpoint.h"

struct t4_client_session {
    char id[15], path[PATH_MAX], status[24];
    long owner;
};
typedef int (*t4_client_session_fn)(const struct t4_client_session *, void *);
typedef int (*t4_client_history_fn)(const struct t4_checkpoint *, void *);

/* Create private parents and a unique session-XXXXXX/checkpoint.ckpt path.
 * The checkpoint itself does not yet exist. Returns 0, or -1 with errno. */
int t4_client_state_new(const char *session_dir, char *out, size_t capacity);

/* List managed checkpoint paths and lock/file status without loading, changing
 * or revealing their question/answer contents. Missing directories are empty. */
int t4_client_state_list(const char *session_dir);

/* Stable ID ordering, callbacks return 0 to continue or -1 to abort. A missing
 * directory is empty. Enumeration never loads or changes the checkpoint. */
int t4_client_state_scan(const char *session_dir, t4_client_session_fn visit, void *opaque);

/* Resolve a managed session-XXXXXX ID or existing checkpoint path. The caller
 * must then acquire t4_checkpoint_lock and load; resolving grants no lock. */
int t4_client_state_resolve(const char *session_dir, const char *id_or_path,
                          char *out, size_t capacity);

/* Soft-delete an unlocked managed ID into session_dir/.trash. Checkpoint and
 * lock inode move together, never unlinking the lock. Refuses arbitrary paths,
 * symlinks and active sessions. Returns the recoverable directory in out. */
int t4_client_state_delete(const char *session_dir, const char *id,
                         char *out, size_t capacity);

/* A compact, checksummed display archive alongside path as path+.history.
 * Only terminal prompt/answer are stored; status, thinking and steering are
 * excluded. Caller holds the active checkpoint lock. Same turn is idempotent;
 * conflicting content returns EEXIST and is never replaced. */
int t4_client_history_archive(const char *path, const struct t4_checkpoint *c);
/* Visit completed/cancelled archives in request order. session must match the
 * current checkpoint. Each borrowed checkpoint is valid during its callback. */
int t4_client_history_scan(const char *path, const char *session,
                         t4_client_history_fn visit, void *opaque);

/* Explain a lock/load failure using the caller's saved errno. */
void t4_client_state_lock_error(const char *path, int error);

#endif
