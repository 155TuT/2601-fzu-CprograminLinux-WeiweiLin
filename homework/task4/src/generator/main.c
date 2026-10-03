#define _POSIX_C_SOURCE 200809L

#include "task4/protocol.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct options {
    const char *source;
    const char *prompt;
    const char *context;
    uint32_t offset;
    uint32_t rate;
};

static void usage(const char *program)
{
    (void)fprintf(stderr,
                  "Usage: %s --source FILE [--prompt FILE] [--context FILE]\n"
                  "          [--offset BYTE_OFFSET] [--rate 0..10000]\n"
                  "The corpus backend ignores prompt/context content.\n"
                  "Rate 0 streams immediately; other rates are Unicode characters/s.\n",
                  program);
}

static int parse_options(int argc, char **argv, struct options *options)
{
    int index;
    unsigned int seen = 0;

    options->source = NULL;
    options->prompt = NULL;
    options->context = NULL;
    options->offset = 0;
    options->rate = 30;
    for (index = 1; index < argc; index++) {
        const char *name = argv[index];
        const char *value;
        unsigned int bit;

        if (strcmp(name, "--help") == 0 && argc == 2) {
            usage(argv[0]);
            return 1;
        }
        if (index + 1 >= argc) {
            return -1;
        }
        value = argv[++index];
        if (*value == '\0') {
            return -1;
        }
        if (strcmp(name, "--source") == 0) {
            bit = 1U;
            options->source = value;
        } else if (strcmp(name, "--prompt") == 0) {
            bit = 2U;
            options->prompt = value;
        } else if (strcmp(name, "--context") == 0) {
            bit = 4U;
            options->context = value;
        } else if (strcmp(name, "--offset") == 0) {
            bit = 8U;
            if (t4_uint(value, 0, T4_MAX_RESPONSE, &options->offset) == -1) {
                return -1;
            }
        } else if (strcmp(name, "--rate") == 0) {
            bit = 16U;
            if (t4_uint(value, 0, 10000, &options->rate) == -1) {
                return -1;
            }
        } else {
            return -1;
        }
        if ((seen & bit) != 0) {
            return -1;
        }
        seen |= bit;
    }
    return options->source == NULL ? -1 : 0;
}

/* Return one strictly valid UTF-8 code point, 0 for clean EOF, or -1. */
static int read_character(FILE *source, unsigned char bytes[4], size_t *length)
{
    int first = fgetc(source);
    size_t expected;
    size_t index;

    if (first == EOF) {
        if (ferror(source)) {
            if (errno == 0) {
                errno = EIO;
            }
            return -1;
        }
        return 0;
    }
    bytes[0] = (unsigned char)first;
    if (first >= 1 && first <= 0x7f) {
        expected = 1;
    } else if (first >= 0xc2 && first <= 0xdf) {
        expected = 2;
    } else if (first >= 0xe0 && first <= 0xef) {
        expected = 3;
    } else if (first >= 0xf0 && first <= 0xf4) {
        expected = 4;
    } else {
        errno = EILSEQ;
        return -1;
    }
    for (index = 1; index < expected; index++) {
        int next = fgetc(source);

        if (next == EOF) {
            if (!ferror(source) || errno == 0) {
                errno = EILSEQ;
            }
            return -1;
        }
        if (next < 0x80 || next > 0xbf) {
            errno = EILSEQ;
            return -1;
        }
        bytes[index] = (unsigned char)next;
    }
    /* Exclude overlong forms, surrogate code points, and values above U+10FFFF. */
    if ((first == 0xe0 && bytes[1] < 0xa0)
        || (first == 0xed && bytes[1] > 0x9f)
        || (first == 0xf0 && bytes[1] < 0x90)
        || (first == 0xf4 && bytes[1] > 0x8f)) {
        errno = EILSEQ;
        return -1;
    }
    *length = expected;
    return 1;
}

static int stream_source(FILE *source, const struct options *options)
{
    unsigned char character[4];
    uint32_t consumed = 0;
    unsigned int emitted = 0;
    struct timespec interval = { .tv_sec = 0, .tv_nsec = 0 };
    int result;

    if (options->rate != 0) {
        interval.tv_sec = 1 / options->rate;
        interval.tv_nsec = (long)(1000000000U / options->rate
                                 - (uint32_t)interval.tv_sec * 1000000000U);
    }
    for (;;) {
        size_t length;

        result = read_character(source, character, &length);
        if (result != 1) {
            break;
        }
        if (length > T4_MAX_RESPONSE - consumed) {
            errno = EFBIG;
            return 1;
        }
        if (consumed < options->offset
            && consumed + (uint32_t)length > options->offset) {
            (void)fprintf(stderr, "offset must lie on a UTF-8 character boundary\n");
            return 2;
        }
        if (consumed >= options->offset) {
            /* Only pacing uses sleep. I/O and server scheduling use blocking/poll. */
            if (emitted != 0 && options->rate != 0) {
                if (nanosleep(&interval, NULL) == -1) {
                    return 1;
                }
            }
            if (fwrite(character, 1, length, stdout) != length
                || fflush(stdout) == EOF) {
                return 1;
            }
            emitted = 1;
        }
        consumed += (uint32_t)length;
    }
    if (result == -1) {
        return 1;
    }
    if (consumed < options->offset) {
        (void)fprintf(stderr, "offset exceeds the source length\n");
        return 2;
    }
    return 0;
}

int main(int argc, char **argv)
{
    struct options options;
    struct stat metadata;
    FILE *source;
    int parsed = parse_options(argc, argv, &options);
    int result;

    if (parsed == 1) {
        return 0;
    }
    if (parsed == -1) {
        usage(argv[0]);
        return 2;
    }
    source = fopen(options.source, "rb");
    if (source == NULL) {
        perror("open source");
        return 1;
    }
    if (fstat(fileno(source), &metadata) == -1) {
        perror("stat source");
        (void)fclose(source);
        return 1;
    }
    if (!S_ISREG(metadata.st_mode) || metadata.st_size < 0
        || metadata.st_size > (off_t)T4_MAX_RESPONSE) {
        (void)fprintf(stderr, "source must be a regular file of at most %u bytes\n",
                      (unsigned int)T4_MAX_RESPONSE);
        (void)fclose(source);
        return 1;
    }
    if ((off_t)options.offset > metadata.st_size) {
        (void)fprintf(stderr, "offset exceeds the source length\n");
        (void)fclose(source);
        return 2;
    }
    (void)fprintf(stderr,
                  "backend=corpus source=%s offset=%u rate=%u characters/s\n"
                  "prompt=%s context=%s (reserved for future backends)\n",
                  options.source, (unsigned int)options.offset,
                  (unsigned int)options.rate,
                  options.prompt == NULL ? "(none)" : options.prompt,
                  options.context == NULL ? "(none)" : options.context);
    result = stream_source(source, &options);
    if (result == 1) {
        perror("stream source");
    }
    if (fclose(source) == EOF && result == 0) {
        perror("close source");
        result = 1;
    }
    return result;
}
