#ifndef TASK4_BACKEND_H
#define TASK4_BACKEND_H

#include <stdint.h>
#include <sys/types.h>

struct t4_generation {
    pid_t pid;
    int fd; /* Nonblocking socket receiving the adapter's framed stdout. */
};

/*
 * prepare snapshots model configuration for a durable turn; start may be
 * called again after disconnect. Model output uses typed T4 frames, and its
 * own raw journal preserves the generated prefix across fresh inference jobs.
 */
struct t4_backend {
    const char *name;
    int (*prepare)(const char *model_config, const char *turn_dir);
    /* runtime_config supplies transport routing only; a model turn keeps its
     * frozen prompt, sampling options and limits across server port changes. */
    int (*start)(const char *adapter, const char *turn_dir,
                 const char *context_path, const char *runtime_config,
                 uint32_t offset,
                 struct t4_generation *generation);
};

extern const struct t4_backend t4_model_backend;

/* Closes the pipe, terminates and reaps the child, and sets both fields to -1. */
void t4_generation_stop(struct t4_generation *generation);
int t4_generation_spawn(char *const argv[], const char *log_path,
                        struct t4_generation *generation);

#endif
