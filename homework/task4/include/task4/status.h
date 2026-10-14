#ifndef TASK4_STATUS_H
#define TASK4_STATUS_H

#include "task4/terminal.h"

/* STATUS is presentation data. Parsing never changes a checkpoint or prompt. */
struct t4_client_status {
    struct t4_ui_status ui;
    char phase[32], model[128];
};

void t4_status_init(struct t4_client_status *status);
/* Merge known top-level properties atomically; leave old values on failure. */
int t4_status_update(struct t4_client_status *status, const char *json, size_t length);

#endif
