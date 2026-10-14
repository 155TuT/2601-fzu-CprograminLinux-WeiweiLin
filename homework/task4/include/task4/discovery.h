#ifndef TASK4_DISCOVERY_H
#define TASK4_DISCOVERY_H

#include <limits.h>
#include <stdint.h>

struct t4_endpoint {
    char host[16];
    uint16_t port;
    char state[PATH_MAX];
    char instance[33];
};

/* Only the master retains the lease. Workers close their inherited copies. */
struct t4_service {
    int lease_fd, state_fd, registry_fd;
    uint64_t device, inode;
    char state[PATH_MAX], entry[80], instance[33];
    int published;
};

/* 0: newly owned lease; 1: matching live service in existing; -1: error.
 * EBUSY means another master owns the state but has not published its endpoint. */
int t4_service_acquire(const char *state, struct t4_service *service,
                       struct t4_endpoint *existing);
int t4_service_publish(struct t4_service *service, const char *registry,
                       const char *host, uint16_t port);
void t4_service_child_close(struct t4_service *service);
void t4_service_close(struct t4_service *service);

/* explicit_state bypasses registry scanning. token filters saved sessions.
 * ENOENT: none live; EEXIST: ambiguous. Neither chooses an arbitrary server. */
int t4_service_discover(const char *registry, const char *explicit_state,
                        const char *token, struct t4_endpoint *endpoint);
/* Recheck the state lease and instance before the same-connection wire probe. */
int t4_service_validate(const struct t4_endpoint *endpoint);

#endif
