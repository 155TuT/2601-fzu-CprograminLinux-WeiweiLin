#include "task4/backend.h"
#include "task4/files.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>

/* Freeze options with the turn; editing config affects new turns only. */
static int model_prepare(const char *config, const char *directory)
{
    char path[PATH_MAX];
    if (t4_path(path, directory, "model-config.json") < 0) return -1;
    int input = open(config, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (input < 0) return -1;
    int output = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    int result = -1;
    if (output < 0) goto cleanup;
    char buffer[4096];
    size_t total = 0;
    for (;;) {
        ssize_t n = read(input, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) goto cleanup;
        if (!n) break;
        total += (size_t)n;
        if (total > 65536) { errno = EFBIG; goto cleanup; }
        if (t4_write_all(output, buffer, (size_t)n) < 0) goto cleanup;
    }
    result = fsync(output);
cleanup:
    {
        int saved = errno;
        close(input);
        if (output >= 0) close(output);
        errno = saved;
        return result;
    }
}

static int model_start(const char *adapter, const char *directory,
                       const char *context, const char *runtime_config,
                       uint32_t offset,
                       struct t4_generation *generation)
{
    char config[PATH_MAX], log[PATH_MAX], cursor[11];
    if (t4_path(config, directory, "model-config.json") < 0 ||
        t4_path(log, directory, "generator.log") < 0) return -1;
    snprintf(cursor, sizeof(cursor), "%u", offset);
    char *const args[] = {"python3", (char *)adapter, "--turn", (char *)directory,
        "--context", (char *)context, "--offset", cursor, "--config", config,
        "--runtime-config", (char *)runtime_config, NULL};
    return t4_generation_spawn(args, log, generation);
}

const struct t4_backend t4_model_backend = {
    .name = "llama", .prepare = model_prepare, .start = model_start
};
