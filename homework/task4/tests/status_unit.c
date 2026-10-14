#include "task4/status.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int update(struct t4_client_status *status, const char *json)
{
    return t4_status_update(status, json, strlen(json));
}

int main(void)
{
    struct t4_client_status status;
    t4_status_init(&status);
    assert(!strcmp(status.ui.phase, "waiting"));
    assert(update(&status, "{\"phase\":\"generating\",\"model\":\"Qwen3.5-4B\","
                   "\"prompt_tokens\":321,\"context_tokens\":16384,"
                   "\"generated_tokens\":45,\"tokens_per_second\":72.5,\"elapsed_seconds\":1.25}") == 0);
    assert(status.ui.prompt_tokens == 321 && status.ui.context_tokens == 16384);
    assert(status.ui.generated_tokens == 45 && status.ui.tokens_per_second == 72.5);
    assert(status.ui.elapsed_seconds == 1.25 && !strcmp(status.ui.model, "Qwen3.5-4B"));
    puts("PASS complete 16k status and stable owned labels");

    assert(update(&status, "{\"timings\":{\"generated_tokens\":999,\"nested\":[{\"phase\":\"wrong\"}]},"
                           "\"generated_tokens\":46,\"unused\":[1,true,null]}") == 0);
    assert(status.ui.generated_tokens == 46 && status.ui.context_tokens == 16384);
    assert(!strcmp(status.ui.phase, "generating"));
    puts("PASS partial updates retain fields and skip nested unknown objects");

    assert(update(&status, "{\"model\":\"\\u6a21\\u578b/Qwen\\\"4B\\\"\"}") == 0);
    assert(!strcmp(status.ui.model, "模型/Qwen\"4B\""));
    puts("PASS JSON label escapes decoded without terminal control bytes");

    const char *invalid[] = {
        "{\"phase\":\"bad\",\"generated_tokens\":-1}",
        "{\"generated_tokens\":4294967296}", "{\"generated_tokens\":1.5}",
        "{\"generated_tokens\":true}", "{\"tokens_per_second\":1e999}",
        "{\"phase\":\"\\u001b[2J\"}", "{\"phase\":42}",
        "{\"phase\":\"unfinished\"", "[]", "{} {}", "queued locally"
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); ++i) {
        struct t4_client_status before = status;
        assert(update(&status, invalid[i]) < 0);
        assert(!memcmp(&before, &status, sizeof(status)));
    }
    puts("PASS invalid status cannot partially replace the visible state");

    char long_model[256];
    memset(long_model, 'x', sizeof(long_model));
    memcpy(long_model, "{\"model\":\"", 10);
    memcpy(long_model + sizeof(long_model) - 3, "\"}", 3);
    assert(update(&status, long_model) < 0);
    assert(status.ui.model == status.model && status.ui.phase == status.phase);
    puts("PASS oversized labels rejected; all 5 status parser checks passed");
    return 0;
}
