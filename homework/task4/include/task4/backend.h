#ifndef TASK4_BACKEND_H
#define TASK4_BACKEND_H

#include <stdint.h>
#include <sys/types.h>

struct t4_generation {
    pid_t pid;
    int fd; /* Nonblocking read end of the generator's stdout pipe. */
};

/*
 * prepare fixes the response source for a durable turn; start may be called
 * again with a byte offset after a disconnect. A future model backend can
 * implement the same interface while using prompt.txt and the context file.
 */
struct t4_backend {
    const char *name;
    int (*prepare)(const char *corpus_dir, const char *turn_dir);
    int (*start)(const char *generator, const char *turn_dir,
                 const char *context_path, uint32_t offset, uint32_t rate,
                 struct t4_generation *generation);
};

extern const struct t4_backend t4_corpus_backend;

/* Closes the pipe, terminates and reaps the child, and sets both fields to -1. */
void t4_generation_stop(struct t4_generation *generation);

#endif
