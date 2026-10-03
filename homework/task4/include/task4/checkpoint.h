#ifndef TASK4_CHECKPOINT_H
#define TASK4_CHECKPOINT_H
#include "task4/protocol.h"

struct t4_checkpoint {
    char session[33];
    uint32_t request, offset, prompt_length, done;
    uint32_t file_lines;
    unsigned char prompt[T4_MAX_PROMPT + 1];
    unsigned char *answer;
    uint32_t saved_request, saved_offset, saved_done;
    int journal_loaded;
};
int t4_checkpoint_load(const char *path, struct t4_checkpoint *c);
int t4_checkpoint_save(const char *path, struct t4_checkpoint *c);
int t4_checkpoint_lock(const char *path);

#endif
