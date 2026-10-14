#include "task4/status.h"

#define JSMN_STATIC
#define JSMN_STRICT
#include "jsmn/jsmn.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static void bind_strings(struct t4_client_status *status)
{
    status->ui.phase = status->phase;
    status->ui.model = status->model;
}

void t4_status_init(struct t4_client_status *status)
{
    memset(status, 0, sizeof(*status));
    strcpy(status->phase, "waiting");
    bind_strings(status);
}

static int equals(const char *json, const jsmntok_t *token, const char *key)
{
    return token->type == JSMN_STRING && (size_t)(token->end - token->start) == strlen(key)
           && !memcmp(json + token->start, key, strlen(key));
}

/* Model and phase labels are short UTF-8 strings. Decode JSON escapes rather
 * than displaying backslashes, and reject truncated labels/control characters. */
static int label(const char *json, const jsmntok_t *token, char *out, size_t capacity)
{
    if (token->type != JSMN_STRING) return -1;
    size_t used = 0;
    for (int at = token->start; at < token->end;) {
        unsigned char byte = (unsigned char)json[at++];
        if (byte == '\\') {
            if (at == token->end) return -1;
            byte = (unsigned char)json[at++];
            if (byte == 'u') {
                if (token->end - at < 4) return -1;
                unsigned code = 0;
                for (int n = 0; n < 4; ++n) {
                    unsigned char digit = (unsigned char)json[at++];
                    unsigned value = digit >= '0' && digit <= '9' ? digit - '0' :
                                     digit >= 'a' && digit <= 'f' ? digit - 'a' + 10 :
                                     digit >= 'A' && digit <= 'F' ? digit - 'A' + 10 : 16;
                    if (value == 16) return -1;
                    code = code * 16 + value;
                }
                if (code < 32 || code == 127 || (code >= 0xd800 && code <= 0xdfff)) return -1;
                unsigned char utf8[3];
                size_t width;
                if (code < 128) { utf8[0] = (unsigned char)code; width = 1; }
                else if (code < 2048) {
                    utf8[0] = 0xc0U | (code >> 6); utf8[1] = 0x80U | (code & 63U); width = 2;
                } else {
                    utf8[0] = 0xe0U | (code >> 12); utf8[1] = 0x80U | ((code >> 6) & 63U);
                    utf8[2] = 0x80U | (code & 63U); width = 3;
                }
                if (used + width >= capacity) return -1;
                memcpy(out + used, utf8, width); used += width;
                continue;
            }
            if (byte != '"' && byte != '\\' && byte != '/') return -1;
        }
        if (byte < 32 || byte == 127 || used + 1 >= capacity) return -1;
        out[used++] = (char)byte;
    }
    out[used] = 0;
    return 0;
}

static int number(const char *json, const jsmntok_t *token, double *out, int integer)
{
    int size = token->end - token->start;
    char buffer[64], *end;
    if (token->type != JSMN_PRIMITIVE || size <= 0 || size >= (int)sizeof(buffer)) return -1;
    memcpy(buffer, json + token->start, (size_t)size); buffer[size] = 0;
    if (buffer[0] < '0' || buffer[0] > '9') return -1;
    if (integer) {
        for (int at = 0; at < size; ++at) if (buffer[at] < '0' || buffer[at] > '9') return -1;
    }
    errno = 0;
    double value = strtod(buffer, &end);
    if (errno || *end || !isfinite(value) || value < 0 || value > UINT32_MAX) return -1;
    *out = value;
    return 0;
}

int t4_status_update(struct t4_client_status *status, const char *json, size_t length)
{
    jsmn_parser parser;
    jsmntok_t tokens[256];
    jsmn_init(&parser);
    int count = jsmn_parse(&parser, json, length, tokens, 256);
    if (count < 1 || tokens[0].type != JSMN_OBJECT) return -1;
    for (size_t at = (size_t)tokens[0].end; at < length; ++at)
        if (json[at] != ' ' && json[at] != '\n' && json[at] != '\r' && json[at] != '\t') return -1;
    struct t4_client_status next = *status;
    for (int at = 1; at < count;) {
        const jsmntok_t *key = &tokens[at++];
        if (key->type != JSMN_STRING || at == count) return -1;
        const jsmntok_t *value = &tokens[at];
        double converted;
        if (equals(json, key, "phase")) {
            if (label(json, value, next.phase, sizeof(next.phase)) < 0) return -1;
        } else if (equals(json, key, "model")) {
            if (label(json, value, next.model, sizeof(next.model)) < 0) return -1;
        } else if (equals(json, key, "prompt_tokens")) {
            if (number(json, value, &converted, 1) < 0) return -1;
            next.ui.prompt_tokens = (uint32_t)converted;
        } else if (equals(json, key, "context_tokens")) {
            if (number(json, value, &converted, 1) < 0) return -1;
            next.ui.context_tokens = (uint32_t)converted;
        } else if (equals(json, key, "generated_tokens")) {
            if (number(json, value, &converted, 1) < 0) return -1;
            next.ui.generated_tokens = (uint32_t)converted;
        } else if (equals(json, key, "tokens_per_second")) {
            if (number(json, value, &converted, 0) < 0) return -1;
            next.ui.tokens_per_second = converted;
        } else if (equals(json, key, "elapsed_seconds")) {
            if (number(json, value, &converted, 0) < 0) return -1;
            next.ui.elapsed_seconds = converted;
        }
        int end = value->end;
        do { ++at; } while (at < count && tokens[at].start < end);
    }
    *status = next;
    bind_strings(status);
    return 0;
}
