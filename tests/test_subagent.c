#define _GNU_SOURCE

#include "../src/commands/context.h"

#include "clay/json.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char *mkdtemp(char *template);

typedef struct {
    ClayConnectedProvider *provider;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int entered, release, cancel, result;
} CodexFlightTest;

static int flight_abort(void *arg) {
    CodexFlightTest *test = arg;
    pthread_mutex_lock(&test->lock);
    test->entered = 1;
    pthread_cond_signal(&test->cond);
    while (!test->release) pthread_cond_wait(&test->cond, &test->lock);
    int cancel = test->cancel;
    pthread_mutex_unlock(&test->lock);
    return cancel;
}

static void *flight_waiter(void *arg) {
    CodexFlightTest *test = arg;
    test->result = clay_commands_codex_refresh_begin(test->provider, 0, flight_abort, test);
    if (test->result == 0) clay_commands_codex_refresh_end(test->provider, -1);
    return NULL;
}

static int already_cancelled(void *arg) {
    (void)arg;
    return 1;
}
static void test_codex_flight_foundation(void) {
    for (int scenario = 0; scenario < 4; scenario++) {
        ClayProviderConfig config = {0};
        config.access_token = strdup("old");
        config.refresh_token = strdup("refresh");
        config.account_id = strdup("account");
        ClayConnectedProvider provider = {0};
        provider.config = &config;
        provider.environment_override = scenario == 1 || scenario == 2;
        if (scenario == 0) config.id = "flight-test";
        CodexFlightTest test = {.provider = &provider, .cancel = scenario == 2};
        assert(!pthread_mutex_init(&test.lock, NULL));
        assert(!pthread_cond_init(&test.cond, NULL));
        assert(clay_commands_codex_refresh_begin(&provider, 0, NULL, NULL) == 0);
        pthread_t worker;
        assert(!pthread_create(&worker, NULL, flight_waiter, &test));
        pthread_mutex_lock(&test.lock);
        while (!test.entered) pthread_cond_wait(&test.cond, &test.lock);
        pthread_mutex_unlock(&test.lock);
        if (scenario == 0 || scenario == 3) {
            ClayCodexCredentials rotated = {"new", "rotated", NULL, "account", 123};
            ClayOpenAICodex *client = clay_openai_codex_create(&rotated, "other-model");
            assert(client);
            assert(clay_commands_save_codex_credentials(&provider, client, 0) == (scenario == 3 ? -1 : 0));
            clay_openai_codex_destroy(client);
        }
        if (scenario != 2) clay_commands_codex_refresh_end(&provider, scenario == 0 ? 0 : -1);
        if (scenario == 1) {
            assert(clay_commands_codex_refresh_begin(&provider, 0, NULL, NULL) == 0);
            clay_commands_codex_refresh_end(&provider, 0);
        }
        pthread_mutex_lock(&test.lock);
        test.release = 1;
        pthread_cond_signal(&test.cond);
        pthread_mutex_unlock(&test.lock);
        assert(!pthread_join(worker, NULL));
        assert(test.result == (scenario == 0 ? 1 : scenario == 2 ? 2 : -1));
        if (scenario == 2) clay_commands_codex_refresh_end(&provider, 0);
        if (scenario == 0) {
            ClayProviderConfig *saved = clay_config_load("flight-test");
            assert(saved && !strcmp(saved->refresh_token, "rotated"));
            clay_config_free(saved);
            uint64_t version;
            ClayOpenAICodex *client = clay_commands_codex_snapshot(&provider, "model", &version);
            ClayCodexCredentials copy = {0};
            assert(client && version == 1 && !clay_openai_codex_copy_credentials(client, &copy));
            assert(!strcmp(copy.refresh_token, "rotated"));
            clay_openai_codex_credentials_free(&copy);
            clay_openai_codex_destroy(client);
        }
        if (scenario == 3) {
            assert(!strcmp(config.refresh_token, "rotated"));
            assert(clay_commands_codex_refresh_begin(&provider, 0, NULL, NULL) == -1);
            assert(clay_commands_codex_refresh_begin(&provider, 1, NULL, NULL) == -1);
        } else {
            assert(clay_commands_codex_refresh_begin(&provider, provider.codex_version, NULL, NULL) == 0);
            clay_commands_codex_refresh_end(&provider, 0);
        }
        pthread_cond_destroy(&test.cond);
        pthread_mutex_destroy(&test.lock);
        free(config.access_token); free(config.refresh_token); free(config.account_id);
    }
}
static int cancel_after_two_polls(void *arg) {
    int *polls = arg;
    return ++*polls == 2;
}
typedef struct { ClayConnectedProvider *provider; int result, polls; } PublishedFlight;
static void *published_flight_waiter(void *arg) {
    PublishedFlight *test = arg;
    test->result = clay_commands_codex_refresh_begin(test->provider, 0,
                                                     cancel_after_two_polls, &test->polls);
    if (test->result == 0) clay_commands_codex_refresh_end(test->provider, -1);
    return NULL;
}
static void test_codex_publication_waits_for_end(void) {
    ClayProviderConfig config = {0};
    config.access_token = strdup("old"); config.refresh_token = strdup("refresh");
    config.account_id = strdup("account");
    ClayConnectedProvider provider = {.config = &config, .environment_override = 1};
    assert(clay_commands_codex_refresh_begin(&provider, 0, NULL, NULL) == 0);
    ClayCodexCredentials rotated = {"new", "rotated", NULL, "account", 123};
    ClayOpenAICodex *client = clay_openai_codex_create(&rotated, NULL);
    assert(client && clay_commands_save_codex_credentials(&provider, client, 0) == 0);
    clay_openai_codex_destroy(client);
    PublishedFlight test = {.provider = &provider};
    pthread_t worker;
    assert(!pthread_create(&worker, NULL, published_flight_waiter, &test));
    assert(!pthread_join(worker, NULL));
    assert(test.result == 2 && test.polls == 2);
    clay_commands_codex_refresh_end(&provider, 0);
    assert(clay_commands_codex_refresh_begin(&provider, 0, NULL, NULL) == 1);
    free(config.access_token); free(config.refresh_token); free(config.account_id);
}
#include "clay/http.h"
static int review_mock;
static int http_inflight, http_peak; /* review mock 18 */
static int http_calls; /* review mock 19 */
static ClayScope *http_scope;
static uint64_t http_pace_seen;
static uint64_t http_first_ms, http_second_ms; /* review mock 20 */

static uint64_t mono_ms_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static int abort_when_due(void *arg) {
    return mono_ms_now() >= *(uint64_t *)arg;
}
static ClayProviderConfig *grok_published;
static int fail_codex_create;
ClayOpenAICodex *__real_clay_openai_codex_create(const ClayCodexCredentials *, const char *);
ClayOpenAICodex *__wrap_clay_openai_codex_create(const ClayCodexCredentials *c, const char *model) {
    if (fail_codex_create) { fail_codex_create = 0; return NULL; }
    return __real_clay_openai_codex_create(c, model);
}
static pthread_mutex_t network_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t network_cond = PTHREAD_COND_INITIALIZER;
static int network_entered, network_release, network_waiting, network_cancel, network_requests;
static pthread_barrier_t review_barrier;
static int stale_status, stale_old, stale_new, stale_release;

#define CLAY_STRESS_FLOW_COUNT 5
typedef struct {
    const char *marker;
    int attempts;
    int is_main;
    int expect_tool;
    int expect_error;
    int missing_header;
} StressFlow;

typedef struct {
    pthread_mutex_t lock;
    StressFlow flows[CLAY_STRESS_FLOW_COUNT];
    int inflight, peak;
    int tool_calls, missing_header_retries;
} StressMock;
static StressMock *stress_mock;

static void subagent_budget_http(const ClayHttpRequest *req) {
    const char *body = req->body;
    if (strstr(body, "\"content\":\"budget-a\"")) {
        ClayJson *request = clay_json_parse(body, NULL);
        assert(request);
        ClayJson *messages = clay_json_object_get(request, "messages");
        size_t tool_count = 0;
        for (size_t i = 0; i < clay_json_array_count(messages); i++) {
            ClayJson *message = clay_json_array_get(messages, i);
            if (strcmp(clay_json_string_value(clay_json_object_get(message, "role")), "tool"))
                continue;
            const char *content = clay_json_string_value(clay_json_object_get(message, "content"));
            if (tool_count++ == 0) {
                if (clay_json_array_count(messages) == 4)
                    assert(strstr(content, "old-result-tail"));
                else
                    assert(strstr(content, "[collapsed - ") &&
                           !strstr(content, "old-result-tail"));
            } else {
                assert(strstr(content, "recent-result-tail"));
            }
        }
        clay_json_free(request);
        if (tool_count < 2) {
            ClayStr event;
            clay_str_init(&event);
            clay_str_printf(&event,
                "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call-%zu\","
                "\"function\":{\"name\":\"todowrite\",\"arguments\":"
                "\"{\\\"todos\\\":[{\\\"content\\\":\\\"", tool_count);
            for (int i = 0; i < 300; i++) clay_str_push_char(&event, 'x');
            clay_str_push(&event, tool_count == 0 ? "old-result-tail" : "recent-result-tail");
            clay_str_printf(&event,
                "\\\",\\\"status\\\":\\\"completed\\\"}]}\"}}]}}]}\n\n"
                "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":%d}}\n\n",
                tool_count == 0 ? 2000 : 8100);
            req->on_chunk(event.data, event.len, req->userdata);
            clay_str_free(&event);
            return;
        }
    }

    const char *prefix = strstr(body, "\"content\":\"budget-a\"") ? "A " :
                         strstr(body, "\"content\":\"budget-b\"") ? "B " : NULL;
    if (!prefix) {
        const char *event = "data: {\"choices\":[{\"delta\":{\"content\":\"short summary\"}}]}\n\n";
        req->on_chunk(event, strlen(event), req->userdata);
        return;
    }
    ClayStr event;
    clay_str_init(&event);
    for (int i = 0; i < 3; i++) {
        clay_str_clear(&event);
        clay_str_push(&event, "data: {\"choices\":[{\"delta\":{\"content\":\"");
        if (i == 0) clay_str_push(&event, prefix);
        for (int j = 0; j < 2000; j++) clay_str_push(&event, "中");
        clay_str_push(&event, "\"}}]}\n\n");
        req->on_chunk(event.data, event.len, req->userdata);
    }
    const char *end = "data: {\"choices\":[{\"delta\":{\"content\":\"summary-tail\"}}]}\n\n";
    req->on_chunk(end, strlen(end), req->userdata);
    clay_str_free(&event);
}

int __real_clay_http_request(const ClayHttpRequest *, ClayHttpResponse *);
int __wrap_clay_http_request(const ClayHttpRequest *req, ClayHttpResponse *resp) {
    if (!review_mock) return __real_clay_http_request(req, resp);
    memset(resp, 0, sizeof(*resp));
    if (review_mock == 23) {
        resp->status = 200;
        subagent_budget_http(req);
        return 0;
    }
    if (review_mock == 22) {
        const char *event =
            "data: {\"choices\":[{\"delta\":{\"content\":\"offline summary\"}}]}\n\n";
        const char *usage;
        if (strstr(req->body, "\"content\":\"plain follow-up\"")) {
            usage = "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":3000,\"completion_tokens\":30,\"cached_tokens\":0}}\n\n";
        } else if (strstr(req->body, "\"role\":\"tool\"")) {
            /* The parent sees each branch's report once in the tool response. */
            const char *a = strstr(req->body, "summary from branch A");
            const char *b = strstr(req->body, "summary from branch B");
            assert(a && !strstr(a + 1, "summary from branch A"));
            assert(b && !strstr(b + 1, "summary from branch B"));
            usage = "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":2000,\"completion_tokens\":20}}\n\n";
        } else if (strstr(req->body, "\"content\":\"cache-branch-a\"")) {
            event = "data: {\"choices\":[{\"delta\":{\"content\":\"summary from branch A\"}}]}\n\n";
            usage = "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":600,\"completion_tokens\":6,\"cached_tokens\":400}}\n\n";
        } else if (strstr(req->body, "\"content\":\"cache-branch-b\"")) {
            event = "data: {\"choices\":[{\"delta\":{\"content\":\"summary from branch B\"}}]}\n\n";
            usage = "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":1000,\"completion_tokens\":10,\"cached_tokens\":900}}\n\n";
        } else {
            event = "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"delegate\",\"function\":{\"name\":\"subagent\",\"arguments\":"
                    "\"{\\\"tasks\\\":[{\\\"description\\\":\\\"a\\\",\\\"prompt\\\":\\\"cache-branch-a\\\"},{\\\"description\\\":\\\"b\\\",\\\"prompt\\\":\\\"cache-branch-b\\\"}]}\"}}]}}]}\n\n";
            usage = "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":1000,\"completion_tokens\":10}}\n\n";
        }
        resp->status = 200;
        assert(req->on_chunk);
        req->on_chunk(event, strlen(event), req->userdata);
        req->on_chunk(usage, strlen(usage), req->userdata);
        return 0;
    }
    if (review_mock == 21) {
        StressMock *mock = stress_mock;
        assert(mock && req->body);
        StressFlow *flow = NULL;
        for (size_t i = 0; i < CLAY_STRESS_FLOW_COUNT; i++) {
            if (strstr(req->body, mock->flows[i].marker)) {
                flow = &mock->flows[i];
                break;
            }
        }
        assert(flow);
        pthread_mutex_lock(&mock->lock);
        int attempt = ++flow->attempts;
        if (++mock->inflight > mock->peak) mock->peak = mock->inflight;
        if (attempt == 1 && flow->missing_header) mock->missing_header_retries++;
        if (attempt == 2 && flow->expect_tool) mock->tool_calls++;
        pthread_mutex_unlock(&mock->lock);

        static const char tool_event[] =
            "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"stress\",\"function\":{\"name\":\"todowrite\",\"arguments\":\"{\\\"todos\\\":[{\\\"content\\\":\\\"stress step\\\",\\\"status\\\":\\\"completed\\\"}]}\"}}]}}]}\n\n";
        static const char content_event[] =
            "data: {\"choices\":[{\"delta\":{\"content\":\"offline summary\"}}]}\n\n";
        if (flow->expect_error) {
            resp->status = 500;
            resp->error_body = strdup("offline failure");
            resp->error_body_len = strlen(resp->error_body);
        } else if (attempt == 1) {
            resp->status = 429;
            resp->error_body = strdup("offline limited");
            resp->error_body_len = strlen(resp->error_body);
            if (!flow->missing_header) {
                resp->rate_header_count = 1;
                resp->rate_headers[0].name = "retry-after";
                strcpy(resp->rate_headers[0].value, "0");
            }
        } else if (attempt == 2 && flow->expect_tool) {
            resp->status = 200;
            assert(req->on_chunk);
            req->on_chunk(tool_event, strlen(tool_event), req->userdata);
        } else {
            resp->status = 200;
            assert(req->on_chunk);
            req->on_chunk(content_event, strlen(content_event), req->userdata);
        }
        pthread_mutex_lock(&mock->lock);
        mock->inflight--;
        pthread_mutex_unlock(&mock->lock);
        return 0;
    }
    if (review_mock >= 13 && review_mock <= 17) {
        if (strstr(req->url, "/oauth2/token")) {
            pthread_mutex_lock(&network_lock);
            network_requests++;
            network_entered = 1;
            pthread_cond_broadcast(&network_cond);
            while (review_mock != 16 && !network_release)
                pthread_cond_wait(&network_cond, &network_lock);
            pthread_mutex_unlock(&network_lock);
            assert(!req->should_abort);
            resp->status = review_mock == 15 ? 400 : 200;
            resp->body = strdup(review_mock == 15 ? "{\"error\":\"invalid_grant\"}" :
                "{\"access_token\":\"new-access\",\"refresh_token\":\"new-refresh\",\"expires_in\":3600}");
        } else if (review_mock == 16 && !strcmp(req->headers[0].value, "Bearer old")) {
            pthread_mutex_lock(&network_lock);
            int order = ++stale_old;
            pthread_cond_broadcast(&network_cond);
            while (stale_old < 2 || (order == 2 && !stale_release))
                pthread_cond_wait(&network_cond, &network_lock);
            pthread_mutex_unlock(&network_lock);
            resp->status = 401;
        } else {
            assert(!strcmp(req->headers[0].value, "Bearer new-access"));
            pthread_mutex_lock(&network_lock);
            stale_new++;
            pthread_cond_broadcast(&network_cond);
            pthread_mutex_unlock(&network_lock);
            resp->status = 200;
            resp->body = strdup("{\"data\":[{\"id\":\"grok-test\"}]}");
        }
        return 0;
    }
    if (review_mock == 12) {
        assert(strstr(req->url, "/oauth2/token"));
        resp->status = 400;
        resp->body = strdup("{\"error\":\"invalid_grant\"}");
        return 0;
    }
    if (review_mock == 11) {
        if (strstr(req->url, "/oauth/token")) {
            pthread_mutex_lock(&network_lock);
            network_requests++;
            pthread_mutex_unlock(&network_lock);
            resp->status = 200;
            resp->body = strdup("{\"access_token\":\"new-access\",\"refresh_token\":\"new-refresh\",\"expires_in\":3600}");
        } else if (!strcmp(req->headers[0].value, "Bearer old")) {
            pthread_mutex_lock(&network_lock);
            int order = ++stale_old;
            pthread_cond_broadcast(&network_cond);
            while (stale_old < 2 || (order == 2 && !stale_release))
                pthread_cond_wait(&network_cond, &network_lock);
            pthread_mutex_unlock(&network_lock);
            resp->status = stale_status;
        } else {
            assert(!strcmp(req->headers[0].value, "Bearer new-access"));
            pthread_mutex_lock(&network_lock);
            stale_new++;
            pthread_cond_broadcast(&network_cond);
            pthread_mutex_unlock(&network_lock);
            resp->status = 200;
            resp->body = strdup("{\"data\":[{\"id\":\"codex-test\"}]}");
        }
        return 0;
    }
    /* Review mock 18: count HTTP attempts in flight; one scope must never exceed one. */
    if (review_mock == 18) {
        pthread_mutex_lock(&network_lock);
        if (++http_inflight > http_peak) http_peak = http_inflight;
        pthread_mutex_unlock(&network_lock);
        usleep(20000);
        pthread_mutex_lock(&network_lock);
        http_inflight--;
        pthread_mutex_unlock(&network_lock);
        resp->status = 200;
        resp->body = strdup("");
        return 0;
    }
    /* Review mock 19: first attempt 429, later attempts 200; counts attempts. */
    if (review_mock == 19) {
        pthread_mutex_lock(&network_lock);
        int n = ++http_calls;
        pthread_mutex_unlock(&network_lock);
        resp->status = n == 1 ? 429 : 200;
        if (n == 2) http_pace_seen = clay_scope_test_pace(http_scope);
        resp->body = strdup("");
        return 0;
    }
    /* Review mock 20: the first attempt reports 17000 tokens of usage; times both attempts. */
    if (review_mock == 20) {
        pthread_mutex_lock(&network_lock);
        int n = ++http_calls;
        if (n == 1) http_first_ms = mono_ms_now();
        else if (n == 2) http_second_ms = mono_ms_now();
        pthread_mutex_unlock(&network_lock);
        if (n == 1) {
            static const char usage_event[] =
                "data: {\"type\":\"response.completed\",\"response\":{\"usage\":{\"input_tokens\":12000,\"output_tokens\":5000}}}\n\n";
            req->on_chunk(usage_event, strlen(usage_event), req->userdata);
        }
        resp->status = 200;
        resp->body = strdup("");
        return 0;
    }
    if (review_mock == 9 || review_mock == 10) {
        if (strstr(req->url, "/oauth/token")) {
            pthread_mutex_lock(&network_lock);
            network_requests++;
            network_entered = 1;
            pthread_cond_broadcast(&network_cond);
            while (!network_release) pthread_cond_wait(&network_cond, &network_lock);
            pthread_mutex_unlock(&network_lock);
            assert(!req->should_abort);
            resp->status = review_mock == 10 ? 400 : 200;
            resp->body = strdup(review_mock == 10 ? "{\"error\":\"invalid_grant\"}" : "{\"access_token\":\"new-access\",\"refresh_token\":\"new-refresh\",\"expires_in\":3600}");
        } else {
            assert(req->header_count && !strcmp(req->headers[0].value, "Bearer new-access"));
            resp->status = 200;
            resp->body = strdup("{\"data\":[{\"id\":\"codex-test\"}]}");
        }
        return 0;
    }
    if (review_mock <= 2) {
        pthread_barrier_wait(&review_barrier);
        pthread_barrier_wait(&review_barrier);
        resp->status = 200;
        if (review_mock == 2) {
            assert(req->should_abort);
            req->should_abort(req->abort_userdata);
            return -1;
        }
        return 0;
    }
    if (review_mock >= 6 && strstr(req->url, "/oauth/token")) {
        if (review_mock == 8) fail_codex_create = 1;
        resp->status = 200;
        resp->body = strdup("{\"access_token\":\"new-access\",\"refresh_token\":\"new-refresh\",\"expires_in\":3600}");
    } else if (review_mock >= 6) {
        assert(req->header_count && !strcmp(req->headers[0].value, "Bearer new-access"));
        resp->status = 200;
        resp->body = strdup("{\"data\":[{\"id\":\"codex-test\"}]}");
    } else if (strstr(req->url, "/oauth2/token")) {
        resp->status = 200;
        resp->body = strdup("{\"access_token\":\"new-access\",\"refresh_token\":\"new-refresh\",\"expires_in\":3600}");
    } else {
        assert(grok_published && !strcmp(grok_published->refresh_token, "new-refresh"));
        assert(req->header_count && !strcmp(req->headers[0].value, "Bearer new-access"));
        if (review_mock == 3) {
            ClayProviderConfig *saved = clay_config_load("grok-review");
            assert(saved && !strcmp(saved->refresh_token, "new-refresh"));
            clay_config_free(saved);
        }
        resp->status = review_mock == 3 ? 503 : 200;
        resp->body = strdup(review_mock == 3 ? "error" : "{\"data\":[{\"id\":\"grok-test\"}]}");
    }
    return 0;
}
static void test_codex_invalid_grant_removes_config(void) {
    char id[] = "codex-invalid-grant-review";
    ClayProviderConfig config = {0};
    config.id = id;
    config.access_token = strdup("old");
    config.refresh_token = strdup("old-refresh");
    config.account_id = strdup("account");
    config.expires_at = 1;
    assert(clay_config_save(&config) == 0);
    assert(clay_config_exists(id));

    ClayCommands commands = {0};
    clay_array_init(&commands.providers, sizeof(ClayConnectedProvider));
    commands.scopes = clay_scope_registry_create();
    commands.selected_provider = "openai-codex";
    commands.selected_model = "model";
    ClayConnectedProvider provider = {0};
    provider.config = &config;
    provider.type = clay_commands_find_provider_type("openai-codex");
    provider.generation = 1;
    clay_array_init(&provider.models, sizeof(char *));
    clay_array_push_val(&commands.providers, &provider);

    network_release = 1;
    network_cancel = 0;
    review_mock = 10;
    ClayJson *messages = clay_json_array();
    assert(clay_commands_run_completion(&commands, messages, NULL, 1, NULL, NULL) == -1);
    clay_json_free(messages);
    review_mock = 0;
    ClayConnectedProvider *saved = clay_array_get(&commands.providers, 0);
    assert(saved->codex_auth_invalid);
    assert(!clay_config_exists(id));
    assert(!strcmp(config.refresh_token, "old-refresh"));

    ClayCodexCredentials stale = {config.access_token, config.refresh_token,
                                  NULL, config.account_id, config.expires_at};
    ClayOpenAICodex *client = clay_openai_codex_create(&stale, "model");
    assert(client);
    assert(clay_commands_save_codex_credentials(saved, client,
                                                 saved->codex_version) == 0);
    clay_openai_codex_destroy(client);
    assert(!clay_config_exists(id));

    for (size_t i = 0; i < saved->models.count; i++)
        free(*(char **)clay_array_get(&saved->models, i));
    clay_array_free(&saved->models);
    clay_scope_registry_destroy(commands.scopes);
    clay_array_free(&commands.providers);
    free(config.access_token);
    free(config.refresh_token);
    free(config.account_id);
}

typedef struct { ClayCommands *commands; int result; } ReviewRun;
static void *review_run(void *arg) {
    ReviewRun *run = arg;
    ClayJson *messages = clay_json_array();
    ClayOpenAICallbacks callbacks = {0};
    callbacks.should_abort = already_cancelled;
    run->result = clay_commands_run_completion(run->commands, messages, NULL, 1, NULL,
                                               review_mock == 2 ? &callbacks : NULL);
    clay_json_free(messages);
    return NULL;
}
static void test_stale_completion_results(void) {
    for (int scenario = 1; scenario <= 2; scenario++) {
        ClayCommands c = {0};
        clay_array_init(&c.providers, sizeof(ClayConnectedProvider));
        c.scopes = clay_scope_registry_create();
        c.selected_provider = "openai-codex"; c.selected_model = "model";
        ClayProviderConfig config = {0};
        config.access_token = strdup("old"); config.refresh_token = strdup("old-refresh");
        config.account_id = strdup("account"); config.expires_at = 4102444800LL;
        ClayConnectedProvider provider = {0};
        provider.config = &config; provider.environment_override = 1; provider.generation = 1;
        provider.type = clay_commands_find_provider_type("openai-codex");
        clay_array_push_val(&c.providers, &provider);
        ClayConnectedProvider *shared = clay_array_get(&c.providers, 0);
        review_mock = scenario;
        assert(!pthread_barrier_init(&review_barrier, NULL, 2));
        ReviewRun run = {&c, -99}; pthread_t thread;
        assert(!pthread_create(&thread, NULL, review_run, &run));
        pthread_barrier_wait(&review_barrier);
        ClayCodexCredentials rotated = {"new", "new-refresh", NULL, "account", 4102444800LL};
        ClayOpenAICodex *client = clay_openai_codex_create(&rotated, NULL);
        assert(client && clay_commands_save_codex_credentials(shared, client, 0) == 0);
        clay_openai_codex_destroy(client);
        pthread_barrier_wait(&review_barrier);
        assert(!pthread_join(thread, NULL));
        assert(run.result == (scenario == 1 ? 0 : 1));
        assert(!strcmp(config.refresh_token, "new-refresh"));
        pthread_barrier_destroy(&review_barrier);
        clay_scope_registry_destroy(c.scopes); clay_array_free(&c.providers);
        free(config.access_token); free(config.refresh_token); free(config.account_id);
    }
    review_mock = 0;
}
static int network_abort(void *arg) {
    (void)arg;
    pthread_mutex_lock(&network_lock);
    network_waiting = 1;
    pthread_cond_broadcast(&network_cond);
    int cancel = network_cancel;
    pthread_mutex_unlock(&network_lock);
    return cancel;
}
typedef struct { ClayConnectedProvider *provider; int result; } NetworkModels;
static void *network_models(void *arg) {
    NetworkModels *run = arg;
    ClayArray out; clay_array_init(&out, sizeof(ClayModelItem));
    run->result = clay_commands_fetch_models(run->provider, &out);
    clay_array_free(&out);
    return NULL;
}
static void *network_completion(void *arg) {
    ReviewRun *run = arg;
    ClayJson *messages = clay_json_array();
    ClayOpenAICallbacks callbacks = {0};
    callbacks.should_abort = network_abort;
    run->result = clay_commands_run_completion(run->commands, messages, NULL, 1, NULL, &callbacks);
    clay_json_free(messages);
    return NULL;
}
static void test_codex_network_singleflight(void) {
    for (int scenario = 0; scenario < 4; scenario++) {
        int cancel = scenario == 1;
        ClayCommands c = {0};
        clay_array_init(&c.providers, sizeof(ClayConnectedProvider));
        c.scopes = clay_scope_registry_create();
        c.selected_provider = "openai-codex"; c.selected_model = "model";
        ClayProviderConfig config = {0};
        config.access_token = strdup("old"); config.refresh_token = strdup("old-refresh");
        config.account_id = strdup("account"); config.expires_at = 1;
        ClayConnectedProvider provider = {0};
        provider.config = &config; provider.environment_override = scenario != 3; provider.generation = 1;
        provider.type = clay_commands_find_provider_type("openai-codex");
        clay_array_init(&provider.models, sizeof(char *));
        clay_array_push_val(&c.providers, &provider);
        ClayConnectedProvider *shared = clay_array_get(&c.providers, 0);
        network_entered = network_release = network_waiting = network_requests = 0;
        network_cancel = cancel; review_mock = scenario == 2 ? 10 : 9;
        NetworkModels models = {shared, -99}; ReviewRun run = {&c, -99};
        pthread_t leader, waiter;
        assert(!pthread_create(&leader, NULL, network_models, &models));
        pthread_mutex_lock(&network_lock);
        while (!network_entered) pthread_cond_wait(&network_cond, &network_lock);
        pthread_mutex_unlock(&network_lock);
        assert(!pthread_create(&waiter, NULL, network_completion, &run));
        pthread_mutex_lock(&network_lock);
        while (!network_waiting) pthread_cond_wait(&network_cond, &network_lock);
        if (cancel) {
            pthread_mutex_unlock(&network_lock);
            assert(!pthread_join(waiter, NULL));
            assert(run.result == 1);
            pthread_mutex_lock(&network_lock);
        }
        network_release = 1;
        pthread_cond_broadcast(&network_cond);
        pthread_mutex_unlock(&network_lock);
        assert(!pthread_join(leader, NULL));
        if (!cancel) assert(!pthread_join(waiter, NULL));
        assert(models.result == (scenario >= 2 ? -1 : 0) && run.result == (scenario >= 2 ? -1 : cancel ? 1 : 0));
        assert(network_requests == 1 && !strcmp(config.refresh_token, scenario == 2 ? "old-refresh" : "new-refresh"));
        for (size_t i = 0; i < shared->models.count; i++) free(*(char **)clay_array_get(&shared->models, i));
        clay_array_free(&shared->models); clay_scope_registry_destroy(c.scopes); clay_array_free(&c.providers);
        free(config.access_token); free(config.refresh_token); free(config.account_id);
    }
    review_mock = 0;
}
static void test_grok_network_singleflight(void) {
    for (int scenario = 0; scenario < 5; scenario++) {
        ClayCommands c = {0};
        clay_array_init(&c.providers, sizeof(ClayConnectedProvider));
        c.scopes = clay_scope_registry_create();
        c.selected_provider = "grok"; c.selected_model = "model";
        ClayProviderConfig config = {0};
        config.access_token = strdup("old"); config.refresh_token = strdup("old-refresh");
        config.expires_at = scenario == 3 ? 4102444800LL : 1;
        ClayGrokCredentials credentials = {config.access_token, config.refresh_token, NULL, config.expires_at};
        ClayConnectedProvider provider = {0};
        provider.config = &config; provider.environment_override = scenario != 4; provider.generation = 1;
        provider.type = clay_commands_find_provider_type("grok");
        provider.grok_client = clay_grok_create(&credentials, NULL);
        assert(provider.grok_client);
        clay_array_init(&provider.models, sizeof(char *));
        clay_array_push_val(&c.providers, &provider);
        ClayConnectedProvider *shared = clay_array_get(&c.providers, 0);
        network_entered = network_release = network_waiting = network_requests = 0;
        stale_old = stale_new = stale_release = 0;
        network_cancel = scenario == 1; review_mock = 13 + scenario;
        /* Distinct models mean distinct scopes: requests in one scope are serialized by design. */
        ClayCommands c2 = c;
        c2.selected_model = "model-b";
        ReviewRun a = {&c, -99}, b = {&c2, -99};
        NetworkModels models = {shared, -99};
        pthread_t leader, waiter;
        assert(!pthread_create(&leader, NULL, scenario == 3 ? network_completion : network_models,
                               scenario == 3 ? (void *)&a : (void *)&models));
        if (scenario != 3) {
            pthread_mutex_lock(&network_lock);
            while (!network_entered) pthread_cond_wait(&network_cond, &network_lock);
            pthread_mutex_unlock(&network_lock);
        }
        assert(!pthread_create(&waiter, NULL, network_completion, &b));
        pthread_mutex_lock(&network_lock);
        if (scenario == 3) {
            while (!stale_new) pthread_cond_wait(&network_cond, &network_lock);
            stale_release = 1;
        } else {
            while (!network_waiting) pthread_cond_wait(&network_cond, &network_lock);
            if (scenario == 1) {
                pthread_mutex_unlock(&network_lock);
                assert(!pthread_join(waiter, NULL));
                assert(b.result == 1);
                pthread_mutex_lock(&network_lock);
            }
            network_release = 1;
        }
        pthread_cond_broadcast(&network_cond);
        pthread_mutex_unlock(&network_lock);
        assert(!pthread_join(leader, NULL));
        if (scenario != 1) assert(!pthread_join(waiter, NULL));
        assert((scenario == 3 ? a.result : models.result) == (scenario == 2 || scenario == 4 ? -1 : 0));
        assert(b.result == (scenario == 1 ? 1 : scenario == 2 || scenario == 4 ? -1 : 0));
        assert(network_requests == 1);
        assert(!strcmp(config.refresh_token, scenario == 2 ? "old-refresh" : "new-refresh"));
        if (scenario == 3) assert(stale_old == 2 && stale_new == 2);
        for (size_t i = 0; i < shared->models.count; i++) free(*(char **)clay_array_get(&shared->models, i));
        clay_array_free(&shared->models); clay_grok_destroy(shared->grok_client);
        clay_scope_registry_destroy(c.scopes); clay_array_free(&c.providers);
        free(config.access_token); free(config.refresh_token);
    }
    review_mock = 0;
}
static void test_codex_stale_http(void) {
    for (int status = 401; status <= 403; status += 2) {
        for (int use_models = 0; use_models < 2; use_models++) {
            ClayCommands c = {0};
            clay_array_init(&c.providers, sizeof(ClayConnectedProvider));
            c.scopes = clay_scope_registry_create();
            c.selected_provider = "openai-codex"; c.selected_model = "model";
            ClayProviderConfig config = {0};
            config.access_token = strdup("old"); config.refresh_token = strdup("old-refresh");
            config.account_id = strdup("account"); config.expires_at = 4102444800LL;
            ClayConnectedProvider provider = {0};
            provider.config = &config; provider.environment_override = 1; provider.generation = 1;
            provider.type = clay_commands_find_provider_type("openai-codex");
            clay_array_init(&provider.models, sizeof(char *));
            clay_array_push_val(&c.providers, &provider);
            ClayConnectedProvider *shared = clay_array_get(&c.providers, 0);
            review_mock = 11; stale_status = status;
            stale_old = stale_new = stale_release = network_requests = 0;
            /* Two in-flight stale 401s need two scopes; one scope admits one request at a time. */
            ClayCommands c2 = c;
            c2.selected_model = "model-b";
            ReviewRun a = {&c, -99}, b = {&c2, -99};
            NetworkModels models = {shared, -99};
            pthread_t first, second;
            assert(!pthread_create(&first, NULL, network_completion, &a));
            assert(!pthread_create(&second, NULL, use_models ? network_models : network_completion,
                                   use_models ? (void *)&models : (void *)&b));
            pthread_mutex_lock(&network_lock);
            while (!stale_new) pthread_cond_wait(&network_cond, &network_lock);
            stale_release = 1;
            pthread_cond_broadcast(&network_cond);
            pthread_mutex_unlock(&network_lock);
            assert(!pthread_join(first, NULL)); assert(!pthread_join(second, NULL));
            assert(a.result == 0 && (use_models ? models.result : b.result) == 0);
            assert(stale_old == 2 && stale_new == 2 && network_requests == 1);
            assert(!strcmp(config.refresh_token, "new-refresh"));
            for (size_t i = 0; i < shared->models.count; i++) free(*(char **)clay_array_get(&shared->models, i));
            clay_array_free(&shared->models); clay_scope_registry_destroy(c.scopes); clay_array_free(&c.providers);
            free(config.access_token); free(config.refresh_token); free(config.account_id);
        }
    }
    review_mock = 0;
}
static void test_codex_refresh_ownership(void) {
    for (int scenario = 6; scenario <= 8; scenario++) {
        ClayProviderConfig config = {0};
        config.access_token = strdup("old"); config.refresh_token = strdup("old-refresh");
        config.account_id = strdup("account"); config.expires_at = 1;
        ClayConnectedProvider provider = {0};
        provider.config = &config;
        provider.type = clay_commands_find_provider_type("openai-codex");
        provider.environment_override = scenario != 7;
        clay_array_init(&provider.models, sizeof(char *));
        ClayArray out; clay_array_init(&out, sizeof(ClayModelItem));
        review_mock = scenario;
        int rc = clay_commands_fetch_models(&provider, &out);
        assert(rc == (scenario == 6 ? 0 : -1));
        assert(provider.codex_version == 1);
        assert(!strcmp(config.access_token, "new-access"));
        assert(!strcmp(config.refresh_token, "new-refresh"));
        for (size_t i = 0; i < provider.models.count; i++)
            free(*(char **)clay_array_get(&provider.models, i));
        clay_array_free(&provider.models); clay_array_free(&out);
        free(config.access_token); free(config.refresh_token); free(config.account_id);
    }
    review_mock = 0;
}
static void test_grok_catalog_refresh_failure(void) {
    for (int scenario = 3; scenario <= 5; scenario++) {
        ClayProviderConfig config = {0};
        config.access_token = strdup("old"); config.refresh_token = strdup("old-refresh");
        config.expires_at = 1;
        ClayGrokCredentials initial = {config.access_token, config.refresh_token, NULL, 1};
        ClayConnectedProvider provider = {0};
        provider.config = &config; provider.type = clay_commands_find_provider_type("grok");
        config.id = scenario == 5 ? NULL : "grok-review";
        provider.environment_override = scenario == 4;
        provider.grok_client = clay_grok_create(&initial, NULL);
        assert(provider.grok_client);
        clay_array_init(&provider.models, sizeof(char *));
        ClayArray out; clay_array_init(&out, sizeof(ClayModelItem));
        review_mock = scenario;
        grok_published = &config;
        int rc = clay_commands_fetch_models(&provider, &out);
        grok_published = NULL;
        assert(rc == (scenario == 4 ? 0 : -1));
        assert(provider.grok_version == 1);
        assert(!strcmp(config.refresh_token, "new-refresh"));
        if (scenario == 3) {
            ClayProviderConfig *saved = clay_config_load("grok-review");
            assert(saved && !strcmp(saved->refresh_token, "new-refresh"));
            clay_config_free(saved);
        }
        for (size_t i = 0; i < provider.models.count; i++) free(*(char **)clay_array_get(&provider.models, i));
        clay_array_free(&provider.models); clay_array_free(&out);
        clay_grok_destroy(provider.grok_client);
        free(config.access_token); free(config.refresh_token);
    }
    review_mock = 0;
}

static void test_grok_invalid_grant_preserves_config(void) {
    ClayProviderConfig config = {0};
    config.id = "grok-invalid-review";
    config.access_token = strdup("old");
    config.refresh_token = strdup("old-refresh");
    config.expires_at = 1;
    assert(clay_config_save(&config) == 0);
    ClayGrokCredentials initial = {config.access_token, config.refresh_token, NULL, 1};
    ClayConnectedProvider provider = {0};
    provider.config = &config;
    provider.type = clay_commands_find_provider_type("grok");
    provider.grok_client = clay_grok_create(&initial, NULL);
    assert(provider.grok_client);
    clay_array_init(&provider.models, sizeof(char *));
    ClayArray out;
    clay_array_init(&out, sizeof(ClayModelItem));
    review_mock = 12;
    assert(clay_commands_fetch_models(&provider, &out) == -1);
    ClayProviderConfig *saved = clay_config_load(config.id);
    assert(saved && !strcmp(saved->refresh_token, "old-refresh"));
    clay_config_free(saved);
    clay_array_free(&provider.models);
    clay_array_free(&out);
    clay_grok_destroy(provider.grok_client);
    free(config.access_token);
    free(config.refresh_token);
    review_mock = 0;
}
typedef struct {
    ClayConnectedProvider *provider;
    pthread_barrier_t barrier;
    int result[2];
} CodexRace;

typedef struct { CodexRace *race; int index; } CodexWorker;

static void *codex_worker(void *arg) {
    CodexWorker *worker = arg;
    CodexRace *race = worker->race;
    uint64_t version;
    ClayOpenAICodex *snapshot = clay_commands_codex_snapshot(race->provider, NULL, &version);
    assert(snapshot);
    pthread_barrier_wait(&race->barrier);
    ClayCodexCredentials rotated = {worker->index ? "new-b" : "new-a",
                                   worker->index ? "refresh-b" : "refresh-a",
                                   NULL, "account", 123};
    ClayOpenAICodex *client = clay_openai_codex_create(&rotated, NULL);
    assert(client);
    race->result[worker->index] = clay_commands_save_codex_credentials(race->provider, client, version);
    for (int i = 0; i < 1000; i++) {
        uint64_t current;
        ClayOpenAICodex *copy = clay_commands_codex_snapshot(race->provider, NULL, &current);
        assert(copy);
        ClayCodexCredentials creds = {0};
        assert(clay_openai_codex_copy_credentials(copy, &creds) == 0);
        assert((!strcmp(creds.access_token, "new-a") && !strcmp(creds.refresh_token, "refresh-a")) ||
               (!strcmp(creds.access_token, "new-b") && !strcmp(creds.refresh_token, "refresh-b")));
        clay_openai_codex_credentials_free(&creds);
        clay_openai_codex_destroy(copy);
    }
    clay_openai_codex_destroy(snapshot);
    clay_openai_codex_destroy(client);
    return NULL;
}

static void test_codex_concurrent_snapshot(void) {
    ClayProviderConfig config = {0};
    config.access_token = strdup("old");
    config.refresh_token = strdup("refresh");
    config.account_id = strdup("account");
    ClayConnectedProvider provider = {0};
    provider.config = &config;
    provider.environment_override = 1;
    CodexRace race = {.provider = &provider};
    CodexWorker workers[2] = {{&race, 0}, {&race, 1}};
    pthread_t threads[2];
    assert(!pthread_barrier_init(&race.barrier, NULL, 2));
    for (int i = 0; i < 2; i++) assert(!pthread_create(&threads[i], NULL, codex_worker, &workers[i]));
    for (int i = 0; i < 2; i++) assert(!pthread_join(threads[i], NULL));
    assert(race.result[0] + race.result[1] == 1);
    assert(provider.codex_version == 1);
    assert(!strcmp(config.access_token, "new-a") || !strcmp(config.access_token, "new-b"));
    assert(!pthread_barrier_destroy(&race.barrier));
    free(config.access_token); free(config.refresh_token); free(config.account_id);
}

static void *grok_worker(void *arg) {
    CodexWorker *worker = arg;
    CodexRace *race = worker->race;
    uint64_t version;
    ClayGrok *snapshot = clay_commands_grok_snapshot(race->provider, NULL, &version);
    assert(snapshot);
    pthread_barrier_wait(&race->barrier);
    ClayGrokCredentials rotated = {worker->index ? "new-b" : "new-a",
                                   worker->index ? "refresh-b" : "refresh-a", NULL, 123};
    ClayGrok *client = clay_grok_create(&rotated, NULL);
    assert(client);
    race->result[worker->index] = clay_commands_save_grok_credentials(race->provider, client, version);
    ClayGrokCredentials old = {0};
    assert(clay_grok_copy_credentials(snapshot, &old) == 0);
    assert(!strcmp(old.access_token, "old") && !strcmp(old.refresh_token, "refresh"));
    clay_grok_credentials_free(&old);
    for (int i = 0; i < 1000; i++) {
        uint64_t current;
        ClayGrok *copy = clay_commands_grok_snapshot(race->provider, NULL, &current);
        assert(copy);
        ClayGrokCredentials creds = {0};
        assert(clay_grok_copy_credentials(copy, &creds) == 0);
        assert((!strcmp(creds.access_token, "new-a") && !strcmp(creds.refresh_token, "refresh-a")) ||
               (!strcmp(creds.access_token, "new-b") && !strcmp(creds.refresh_token, "refresh-b")));
        clay_grok_credentials_free(&creds);
        clay_grok_destroy(copy);
    }
    clay_grok_destroy(snapshot);
    clay_grok_destroy(client);
    return NULL;
}

static void test_grok_concurrent_snapshot(void) {
    ClayProviderConfig config = {0};
    config.access_token = strdup("old");
    config.refresh_token = strdup("refresh");
    ClayGrokCredentials initial = {config.access_token, config.refresh_token, NULL, 0};
    ClayConnectedProvider provider = {0};
    provider.config = &config;
    provider.environment_override = 1;
    provider.grok_client = clay_grok_create(&initial, NULL);
    assert(provider.grok_client);
    ClayGrok *persistent = provider.grok_client;
    CodexRace race = {.provider = &provider};
    CodexWorker workers[2] = {{&race, 0}, {&race, 1}};
    pthread_t threads[2];
    assert(!pthread_barrier_init(&race.barrier, NULL, 2));
    for (int i = 0; i < 2; i++) assert(!pthread_create(&threads[i], NULL, grok_worker, &workers[i]));
    for (int i = 0; i < 2; i++) assert(!pthread_join(threads[i], NULL));
    assert(race.result[0] + race.result[1] == 1);
    assert(provider.grok_version == 1 && provider.grok_client == persistent);
    assert(!strcmp(config.access_token, "new-a") || !strcmp(config.access_token, "new-b"));
    ClayGrokCredentials original = {0};
    assert(clay_grok_copy_credentials(persistent, &original) == 0);
    assert(!strcmp(original.access_token, "old"));
    clay_grok_credentials_free(&original);
    clay_grok_destroy(persistent);
    assert(!pthread_barrier_destroy(&race.barrier));
    free(config.access_token); free(config.refresh_token);
}
static ClayProviderConfig *retry_config(void) {
    ClayProviderConfig *config = calloc(1, sizeof(*config));
    assert(config);
    config->id = strdup("retry-test");
    config->access_token = strdup("new");
    config->refresh_token = strdup("new-r");
    config->base_url = strdup("");
    config->expires_at = 1;
    assert(config->id && config->access_token && config->refresh_token && config->base_url);
    return config;
}

static void test_unpersisted_retry(void) {
    char root[] = "/tmp/clay_retry_XXXXXX";
    assert(mkdtemp(root));
    char blocked[256], ok[256], path[512];
    snprintf(blocked, sizeof(blocked), "%s/blocked", root);
    snprintf(ok, sizeof(ok), "%s/ok", root);
    FILE *file = fopen(blocked, "w");  /* a regular file as HOME makes ~/.clay creation fail */
    assert(file);
    fclose(file);
    const char *home = getenv("HOME");
    char *saved_home = home ? strdup(home) : NULL;
    assert(!setenv("HOME", blocked, 1));

    /* Snapshot path: a failed save keeps the flag; a later retry persists and clears it. */
    ClayConnectedProvider provider = {0};
    provider.config = retry_config();
    provider.grok_refresh_unpersisted = 1;
    uint64_t version;
    ClayGrok *client = clay_commands_grok_snapshot(&provider, NULL, &version);
    assert(client && provider.grok_refresh_unpersisted == 1);
    clay_grok_destroy(client);
    assert(!mkdir(ok, 0700));
    assert(!setenv("HOME", ok, 1));
    provider.grok_retry_at = 0;  /* skip the failure backoff */
    client = clay_commands_grok_snapshot(&provider, NULL, &version);
    assert(client && provider.grok_refresh_unpersisted == 0);
    clay_grok_destroy(client);
    ClayProviderConfig *loaded = clay_config_load("retry-test");
    assert(loaded && !strcmp(loaded->access_token, "new") && !strcmp(loaded->refresh_token, "new-r"));
    clay_config_free(loaded);

    /* Shutdown path: destroy forces a retry even during backoff. Destroy frees the commands struct, so it is heap-allocated. */
    ClayCommands *commands = calloc(1, sizeof(*commands));
    assert(commands);
    clay_array_init(&commands->providers, sizeof(ClayConnectedProvider));
    ClayConnectedProvider pending = {0};
    pending.config = retry_config();
    pending.codex_refresh_unpersisted = 1;
    pending.codex_retry_at = time(NULL) + 3600;
    clay_array_push_val(&commands->providers, &pending);
    assert(!setenv("HOME", blocked, 1));
    clay_commands_destroy(commands);  /* save fails: warning only, nothing persisted */
    commands = calloc(1, sizeof(*commands));
    assert(commands);
    clay_array_init(&commands->providers, sizeof(ClayConnectedProvider));
    pending = (ClayConnectedProvider){0};
    pending.config = retry_config();
    pending.codex_refresh_unpersisted = 1;
    pending.codex_retry_at = time(NULL) + 3600;
    clay_array_push_val(&commands->providers, &pending);
    assert(!setenv("HOME", ok, 1));
    clay_commands_destroy(commands);
    loaded = clay_config_load("retry-test");
    assert(loaded && !strcmp(loaded->access_token, "new") && !strcmp(loaded->refresh_token, "new-r"));
    clay_config_free(loaded);
    snprintf(path, sizeof(path), "%s/.clay/providers/retry-test.json", ok);
    assert(!access(path, F_OK));

    /* A direct save failure marks the generation, so exit retries the write. */
    ClayCommands *direct_commands = calloc(1, sizeof(*direct_commands));
    assert(direct_commands);
    clay_array_init(&direct_commands->providers, sizeof(ClayConnectedProvider));
    ClayConnectedProvider direct = {0};
    direct.config = retry_config();
    clay_array_push_val(&direct_commands->providers, &direct);
    assert(!setenv("HOME", blocked, 1));
    ClayCodexCredentials rotated = {"rotated-access", "rotated-r", NULL,
                                    "account", 4102444800LL};
    ClayOpenAICodex *codex_client = clay_openai_codex_create(&rotated, "model");
    assert(codex_client);
    ClayConnectedProvider *direct_saved =
        clay_array_get(&direct_commands->providers, 0);
    assert(clay_commands_save_codex_credentials(direct_saved, codex_client, 0) < 0);
    assert(direct_saved->codex_refresh_unpersisted &&
           direct_saved->codex_retry_at > time(NULL));
    clay_openai_codex_destroy(codex_client);
    assert(!setenv("HOME", ok, 1));
    clay_commands_destroy(direct_commands);
    loaded = clay_config_load("retry-test");
    assert(loaded && !strcmp(loaded->access_token, "rotated-access"));
    clay_config_free(loaded);

    if (saved_home) assert(!setenv("HOME", saved_home, 1));
    else assert(!unsetenv("HOME"));
    free(saved_home);
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);  /* root comes from mkdtemp, no quoting hazards */
    assert(system(cmd) == 0);
}

static ClayJson *call(ClayCommands *commands, const char *json_args) {
    ClayJson *args = clay_json_parse(json_args, NULL);
    assert(args);
    ClayJson *result = subagent_tool(args, commands);
    clay_json_free(args);
    return result;
}

static int ok(ClayJson *result) {
    return clay_json_bool_value(clay_json_object_get(result, "ok"));
}

static const char *error_of(ClayJson *result) {
    return clay_json_string_value(clay_json_object_get(result, "error"));
}

static int has_tool(ClayToolSet *set, const char *name) {
    for (size_t i = 0; i < set->tools.count; i++) {
        const ClayTool *tool = clay_array_get(&set->tools, i);
        if (strcmp(tool->name, name) == 0) return 1;
    }
    return 0;
}

static void stress_mock_init(StressMock *mock) {
    memset(mock, 0, sizeof(*mock));
    assert(!pthread_mutex_init(&mock->lock, NULL));
    mock->flows[0] = (StressFlow){"branch-a", 0, 0, 1, 0, 0};
    mock->flows[1] = (StressFlow){"branch-b", 0, 0, 1, 0, 0};
    mock->flows[2] = (StressFlow){"branch-c", 0, 0, 1, 0, 1};
    mock->flows[3] = (StressFlow){"branch-d", 0, 0, 0, 1, 0};
    mock->flows[4] = (StressFlow){"main-session", 0, 1, 0, 0, 0};
}

typedef struct { ClayCommands *commands; ClayJson *result; } StressBatch;

static void *stress_run_batch(void *arg) {
    StressBatch *batch = arg;
    batch->result = call(batch->commands,
        "{\"tasks\":["
        "{\"description\":\"a\",\"prompt\":\"branch-a stress\"},"
        "{\"description\":\"b\",\"prompt\":\"branch-b stress\"},"
        "{\"description\":\"c\",\"prompt\":\"branch-c stress\"},"
        "{\"description\":\"d\",\"prompt\":\"branch-d stress\"}]}");
    return NULL;
}

static void *stress_main_session(void *arg) {
    ClayCommands *commands = arg;
    ClayJson *messages = clay_json_array();
    clay_json_array_push(messages, clay_openai_message("user", "main-session stress"));
    int rc = clay_commands_run_completion(commands, messages, NULL, 1, NULL, NULL);
    clay_json_free(messages);
    assert(rc == 0);
    return NULL;
}

static void stress_commands_init(ClayCommands *commands) {
    memset(commands, 0, sizeof(*commands));
    clay_array_init(&commands->providers, sizeof(ClayConnectedProvider));
    clay_array_init(&commands->tasks, sizeof(ClayBackgroundTask *));
    clay_array_init(&commands->plan.todos, sizeof(ClayTodoItem));
    clay_array_init(&commands->mcp_servers, sizeof(ClayMcpServer *));
    commands->mcp_connect_attempted = 1;
    commands->mode = CLAY_MODE_ACT;
    commands->scopes = clay_scope_registry_create();
    commands->chat = clay_chat_create("system");
    assert(commands->scopes && commands->chat);
    pthread_mutexattr_t attributes;
    assert(!pthread_mutexattr_init(&attributes));
    assert(!pthread_mutexattr_settype(&attributes, PTHREAD_MUTEX_RECURSIVE));
    assert(!pthread_mutex_init(&commands->tool_lock, &attributes));
    pthread_mutexattr_destroy(&attributes);
}

static void stress_commands_free(ClayCommands *commands, const char *logout_id) {
    commands->selected_provider = NULL;
    commands->selected_model = NULL;
    ClayConnectedProvider *provider = clay_commands_find_provider(commands, logout_id);
    if (provider) {
        assert(clay_config_save(provider->config) == 0);
        assert(clay_commands_logout_provider(commands, logout_id) == 0);
    }
    clay_array_free(&commands->providers);
    clay_scope_registry_destroy(commands->scopes);
    clay_chat_destroy(commands->chat);
    clay_array_free(&commands->tasks);
    clay_array_free(&commands->plan.todos);
    clay_array_free(&commands->mcp_servers);
    pthread_mutex_destroy(&commands->tool_lock);
}

static void stress_assert_transcript(ClayJson *entry, int tool_calls, const char *summary) {
    const char *path = clay_json_string_value(clay_json_object_get(entry, "transcript"));
    assert(path && *path);
    ClayStr text;
    clay_str_init(&text);
    assert(!clay_term_read_file(path, 1 << 20, &text));
    ClayJson *document = clay_json_parse(text.data, NULL);
    assert(document);
    assert((int)clay_json_number_value(clay_json_object_get(document, "tool_calls")) == tool_calls);
    const char *saved = clay_json_string_value(clay_json_object_get(document, "summary"));
    assert(saved && strstr(saved, summary));
    clay_json_free(document);
    clay_str_free(&text);
}

static void test_subagent_parent_usage(void) {
    assert(setenv("OPENAI_API_KEY", "offline-key", 1) == 0);
    ClayApp *app = clay_app_create();
    assert(app);
    ClayCommands *commands = clay_commands_create(app);
    assert(commands && clay_commands_find_provider(commands, "openai"));
    assert(clay_commands_select_model(commands, "openai", "cache-test") == 0);
    commands->mcp_connect_attempted = 1;
    commands->mode = CLAY_MODE_ACT;

    review_mock = 22;
    clay_commands_run_message(commands, "delegate cache work");
    ClayChatRequestTotals requests;
    clay_chat_request_totals(commands->chat, &requests);
    assert(requests.requests == 4 && requests.cached_tokens == 1300);

    ClayChat *saved_chat = clay_chat_load(clay_chat_id(commands->chat));
    assert(saved_chat);
    ClayChatUsage usage;
    clay_chat_get_usage(saved_chat, &usage);
    assert(usage.input_tokens == 4600 && usage.output_tokens == 46);
    assert(usage.cached_input_tokens_known && usage.cached_input_tokens == 1300);
    assert(usage.total_input_tokens == 4600 && usage.total_output_tokens == 46);
    assert(usage.last_prompt_tokens == 2000);
    assert(commands->last_prompt_tokens == 2000);
    clay_chat_destroy(saved_chat);

    clay_commands_run_message(commands, "plain follow-up");
    review_mock = 0;
    clay_chat_get_usage(commands->chat, &usage);
    assert(usage.input_tokens == 3000 && usage.output_tokens == 30);
    assert(usage.cached_input_tokens_known && usage.cached_input_tokens == 0);
    assert(usage.total_input_tokens == 7600 && usage.total_output_tokens == 76);
    clay_commands_destroy(commands);
    clay_app_destroy(app);
}

static void test_subagent_budget_and_summary(void) {
    ClayCommands commands;
    stress_commands_init(&commands);
    assert(setenv("OPENAI_API_KEY", "offline-key", 1) == 0);
    clay_commands_load_provider(&commands, clay_commands_find_provider_type("openai"));
    commands.selected_provider = "openai";
    commands.selected_model = "budget-test";
    commands.last_prompt_tokens = 123;
    commands.accounted_messages = 1;
    commands.conversation = clay_json_array();
    clay_json_array_push(commands.conversation, clay_openai_message("system", "parent system"));

    long budget = clay_config_context_token_budget();
    assert(clay_config_set_context_token_budget(8192) == 0);
    review_mock = 23;
    ClayJson *result = call(&commands,
        "{\"tasks\":[{\"description\":\"a\",\"prompt\":\"budget-a\"},"
        "{\"description\":\"b\",\"prompt\":\"budget-b\"},"
        "{\"description\":\"c\",\"prompt\":\"budget-c\"}]}");
    review_mock = 0;
    assert(clay_config_set_context_token_budget(budget) == 0);

    assert(ok(result));
    assert(commands.last_prompt_tokens == 123 && commands.accounted_messages == 1);
    assert(clay_json_array_count(commands.conversation) == 1);
    const char *output = clay_json_string_value(clay_json_object_get(result, "output"));
    const char *a = strstr(output, "[a] A 中");
    const char *b = strstr(output, "[b] B 中");
    const char *c = strstr(output, "[c] short summary");
    assert(a == output && b && c && a < b && b < c);
    assert(!strstr(output, "summary-tail"));
    size_t share = (16 * 1024) / 3;
    size_t preview_len = 2 + ((share - 2) / strlen("中")) * strlen("中");
    assert(strstr(a, " […]") == a + strlen("[a] ") + preview_len);
    assert(strstr(b, " […]") == b + strlen("[b] ") + preview_len);
    assert(!strstr(c, " […]"));

    ClayJson *entries = clay_json_object_get(result, "results");
    assert(clay_json_array_count(entries) == 3);
    for (size_t i = 0; i < 3; i++) {
        ClayJson *entry = clay_json_array_get(entries, i);
        const char *path = clay_json_string_value(clay_json_object_get(entry, "transcript"));
        ClayStr file;
        clay_str_init(&file);
        assert(*path && clay_term_read_file(path, 1 << 20, &file) == 0);
        ClayJson *transcript = clay_json_parse(file.data, NULL);
        assert(transcript);
        const char *summary = clay_json_string_value(clay_json_object_get(transcript, "summary"));
        if (i < 2) {
            assert(strlen(summary) == 2 + 18000 + strlen("summary-tail"));
            assert(strstr(summary, "summary-tail"));
        } else {
            assert(!strcmp(summary, "short summary"));
        }
        if (i == 0) {
            ClayJson *messages = clay_json_object_get(transcript, "messages");
            const char *old = clay_json_string_value(clay_json_object_get(
                clay_json_array_get(messages, 3), "content"));
            assert(strstr(old, "old-result-tail") && !strstr(old, "[collapsed - "));
        }
        clay_json_free(transcript);
        clay_str_free(&file);
    }
    clay_json_free(result);
    clay_json_free(commands.conversation);
    stress_commands_free(&commands, "openai");
}

/* One main completion and four subagents over one offline HTTP scope: 429 recovery,
   a real tool round trip, a missing header, and a hard error are all exercised. */
static void test_subagent_stress_http(void) {
    ClayCommands commands;
    stress_commands_init(&commands);
    assert(setenv("OPENAI_API_KEY", "offline-key", 1) == 0);
    const ClayProviderType *type = clay_commands_find_provider_type("openai");
    assert(type);
    clay_commands_load_provider(&commands, type);
    commands.selected_provider = "openai";
    commands.selected_model = "stress-model";

    StressMock mock;
    stress_mock_init(&mock);
    stress_mock = &mock;
    review_mock = 21;
    StressBatch batch = {&commands, NULL};
    pthread_t batch_thread, main_thread;
    assert(!pthread_create(&batch_thread, NULL, stress_run_batch, &batch));
    assert(!pthread_create(&main_thread, NULL, stress_main_session, &commands));
    assert(!pthread_join(batch_thread, NULL));
    assert(!pthread_join(main_thread, NULL));
    review_mock = 0;
    stress_mock = NULL;

    assert(mock.inflight == 0 && mock.peak == 1);
    assert(mock.tool_calls == 3 && mock.missing_header_retries == 1);
    assert(mock.flows[0].attempts == 3 && mock.flows[1].attempts == 3);
    assert(mock.flows[2].attempts == 3 && mock.flows[3].attempts == 1);
    assert(mock.flows[4].attempts == 2);

    assert(batch.result && !ok(batch.result));
    ClayJson *results = clay_json_object_get(batch.result, "results");
    assert(results && clay_json_array_count(results) == 4);
    for (size_t i = 0; i < 4; i++) {
        ClayJson *entry = clay_json_array_get(results, i);
        if (i == 3) {
            assert(!ok(entry));
            assert(strstr(error_of(entry), "no summary"));
            assert((int)clay_json_number_value(clay_json_object_get(entry, "tool_calls")) == 0);
            stress_assert_transcript(entry, 0, "");
        } else {
            assert(ok(entry));
            assert((int)clay_json_number_value(clay_json_object_get(entry, "tool_calls")) == 1);
            stress_assert_transcript(entry, 1, "offline summary");
        }
    }
    clay_json_free(batch.result);
    pthread_mutex_destroy(&mock.lock);
    assert(unsetenv("OPENAI_API_KEY") == 0);
    stress_commands_free(&commands, "openai");
}

typedef struct {
    ClayScope *expected_scope;
    pthread_mutex_t lock;
    int cancel_first;
    int calls, tool_calls, cancelled;
} MixedProbe;

static int mixed_scope_probe(ClayScope *scope, const ClayOpenAICallbacks *callbacks, void *userdata) {
    MixedProbe *probe = userdata;
    assert(scope == probe->expected_scope);
    pthread_mutex_lock(&probe->lock);
    probe->calls++;
    int cancel = probe->cancel_first && callbacks && callbacks->userdata && !probe->cancelled;
    if (cancel) probe->cancelled = 1;
    pthread_mutex_unlock(&probe->lock);
    if (cancel) return 1;
    if (callbacks && callbacks->on_tool_call)
        callbacks->on_tool_call("todowrite",
            "{\"todos\":[{\"content\":\"mixed step\",\"status\":\"completed\"}]}",
            callbacks->userdata);
    if (callbacks && callbacks->on_token)
        callbacks->on_token("mixed summary", callbacks->userdata);
    if (callbacks && callbacks->userdata) {
        pthread_mutex_lock(&probe->lock);
        probe->tool_calls++;
        pthread_mutex_unlock(&probe->lock);
    }
    return 0;
}

static ClayScope *stress_scope(ClayCommands *commands, const char *id, const char *model) {
    ClayConnectedProvider *provider = clay_commands_find_provider(commands, id);
    assert(provider);
    ClayScope *scope = clay_scope_get(commands->scopes, provider->generation,
                                      provider->config->base_url, model);
    assert(scope);
    return scope;
}

/* Two sessions with different providers/models run four branches plus their own main
   completion; the second session cancels one branch without disturbing the others. */
static void test_subagent_stress_mixed(void) {
    ClayCommands openai, grok;
    stress_commands_init(&openai);
    stress_commands_init(&grok);
    assert(setenv("OPENAI_API_KEY", "offline-key", 1) == 0);
    assert(setenv("GROK_ACCESS_TOKEN", "offline-access", 1) == 0);
    assert(setenv("GROK_REFRESH_TOKEN", "offline-refresh", 1) == 0);
    clay_commands_load_provider(&openai, clay_commands_find_provider_type("openai"));
    clay_commands_load_provider(&grok, clay_commands_find_provider_type("grok"));
    openai.selected_provider = "openai";
    openai.selected_model = "model-a";
    grok.selected_provider = "grok";
    grok.selected_model = "model-b";

    MixedProbe first = {.expected_scope = stress_scope(&openai, "openai", "model-a")};
    MixedProbe second = {.expected_scope = stress_scope(&grok, "grok", "model-b"), .cancel_first = 1};
    assert(first.expected_scope != second.expected_scope);
    assert(!pthread_mutex_init(&first.lock, NULL));
    assert(!pthread_mutex_init(&second.lock, NULL));
    openai.completion_test_hook = mixed_scope_probe;
    openai.completion_test_userdata = &first;
    grok.completion_test_hook = mixed_scope_probe;
    grok.completion_test_userdata = &second;

    StressBatch batch_a = {&openai, NULL}, batch_b = {&grok, NULL};
    pthread_t threads[2];
    assert(!pthread_create(&threads[0], NULL, stress_run_batch, &batch_a));
    assert(!pthread_create(&threads[1], NULL, stress_main_session, &openai));
    assert(!pthread_join(threads[0], NULL));
    assert(!pthread_join(threads[1], NULL));
    assert(!pthread_create(&threads[0], NULL, stress_run_batch, &batch_b));
    assert(!pthread_create(&threads[1], NULL, stress_main_session, &grok));
    assert(!pthread_join(threads[0], NULL));
    assert(!pthread_join(threads[1], NULL));

    assert(first.calls == 5 && first.tool_calls == 4 && !first.cancelled);
    assert(second.calls == 5 && second.tool_calls == 3 && second.cancelled);
    openai.completion_test_hook = NULL;
    grok.completion_test_hook = NULL;

    assert(batch_a.result && ok(batch_a.result));
    assert(batch_b.result && !ok(batch_b.result));
    ClayJson *results_a = clay_json_object_get(batch_a.result, "results");
    ClayJson *results_b = clay_json_object_get(batch_b.result, "results");
    assert(clay_json_array_count(results_a) == 4 && clay_json_array_count(results_b) == 4);
    for (size_t i = 0; i < 4; i++) {
        ClayJson *entry = clay_json_array_get(results_a, i);
        assert(ok(entry));
        assert((int)clay_json_number_value(clay_json_object_get(entry, "tool_calls")) == 1);
        stress_assert_transcript(entry, 1, "mixed summary");

        entry = clay_json_array_get(results_b, i);
        if (ok(entry)) {
            assert((int)clay_json_number_value(clay_json_object_get(entry, "tool_calls")) == 1);
            stress_assert_transcript(entry, 1, "mixed summary");
        } else {
            assert(strstr(error_of(entry), "cancelled"));
        }
    }

    clay_json_free(batch_a.result);
    clay_json_free(batch_b.result);
    pthread_mutex_destroy(&first.lock);
    pthread_mutex_destroy(&second.lock);
    assert(unsetenv("OPENAI_API_KEY") == 0);
    assert(unsetenv("GROK_ACCESS_TOKEN") == 0);
    assert(unsetenv("GROK_REFRESH_TOKEN") == 0);
    stress_commands_free(&openai, "openai");
    stress_commands_free(&grok, "grok");
}

typedef struct {
    pthread_barrier_t barrier;
    int results[2];
} InterruptRace;

typedef struct {
    InterruptRace *race;
    int index;
} InterruptWorker;

static void *interrupt_worker(void *arg) {
    InterruptWorker *worker = arg;
    for (int i = 0; i < 1000; i++) {
        pthread_barrier_wait(&worker->race->barrier);
        worker->race->results[worker->index] = clay_term_take_interrupt();
        pthread_barrier_wait(&worker->race->barrier);
    }
    return NULL;
}

static void test_interrupt_consumed_once(void) {
    InterruptRace race = {0};
    InterruptWorker workers[2] = {{&race, 0}, {&race, 1}};
    pthread_t threads[2];
    clay_term_init();
    assert(!clay_term_take_interrupt());
    assert(!pthread_barrier_init(&race.barrier, NULL, 3));
    for (int i = 0; i < 2; i++)
        assert(!pthread_create(&threads[i], NULL, interrupt_worker, &workers[i]));
    for (int i = 0; i < 1000; i++) {
        assert(!raise(SIGINT));
        pthread_barrier_wait(&race.barrier);
        pthread_barrier_wait(&race.barrier);
        assert(race.results[0] + race.results[1] == 1);
        assert(!clay_term_take_interrupt());
    }
    for (int i = 0; i < 2; i++) assert(!pthread_join(threads[i], NULL));
    assert(!pthread_barrier_destroy(&race.barrier));
}

typedef struct {
    ClayScopeRegistry *registry;
    pthread_barrier_t *barrier;
    ClayScope *scope;
} ScopeThread;

static void *scope_thread(void *arg) {
    ScopeThread *thread = arg;
    pthread_barrier_wait(thread->barrier);
    for (int i = 0; i < 1000; i++) {
        ClayScope *scope = clay_scope_get(thread->registry, 1,
                                         "https://user:secret@example.test", "model-secret");
        assert(scope && (!thread->scope || scope == thread->scope));
        thread->scope = scope;
    }
    return NULL;
}

typedef struct {
    ClayScopeRegistry *registry;
    pthread_barrier_t *barrier;
    ClayScope *scope;
} FailureThread;

static void *failure_thread(void *arg) {
    FailureThread *thread = arg;
    pthread_barrier_wait(thread->barrier);
    thread->scope = clay_scope_get(thread->registry, 1, "endpoint", "model");
    return NULL;
}

static void test_concurrent_scope_failure(void) {
    ClayScopeRegistry *registry = clay_scope_registry_create();
    assert(registry);
    ClayScope *expected = clay_scope_get(registry, 1, "endpoint", "model");
    assert(expected);
    pthread_barrier_t barrier;
    assert(!pthread_barrier_init(&barrier, NULL, 8));
    pthread_t threads[8];
    FailureThread runs[8] = {0};
    clay_scope_test_fail_allocation(registry, 1);
    for (int i = 0; i < 8; i++) {
        runs[i].registry = registry;
        runs[i].barrier = &barrier;
        assert(!pthread_create(&threads[i], NULL, failure_thread, &runs[i]));
    }
    int failures = 0;
    for (int i = 0; i < 8; i++) {
        assert(!pthread_join(threads[i], NULL));
        if (!runs[i].scope) failures++;
        else assert(runs[i].scope == expected);
    }
    assert(failures == 1);
    pthread_barrier_destroy(&barrier);
    clay_scope_registry_destroy(registry);
}

typedef struct {
    ClayScope *scope;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int checked;
    int cancelled;
    int result;
} WaitProbe;

static int wait_abort(void *arg) {
    WaitProbe *p = arg;
    pthread_mutex_lock(&p->lock);
    p->checked++;
    pthread_cond_signal(&p->cond);
    int cancelled = p->cancelled;
    pthread_mutex_unlock(&p->lock);
    return cancelled;
}

static void *wait_scope(void *arg) {
    WaitProbe *p = arg;
    p->result = clay_scope_acquire(p->scope, wait_abort, p);
    if (p->result) clay_scope_release(p->scope);
    return NULL;
}

static int abort_after_acquire(void *arg) {
    int *calls = arg;
    return ++*calls == 2;
}

static void *wake_scope(void *arg) {
    ClayScope *scope = arg;
    if (clay_scope_acquire(scope, NULL, NULL)) clay_scope_release(scope);
    return NULL;
}

/* Two workers of one batch and the main session race for one scope; every
   guaranteed (callbacks-driven) acquire must come back with the permit free. */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    ClayScope *scope;
    int in_flight, peak, acquire_failures, releases;
} ScopeRace;

static int race_probe(ClayScope *scope, const ClayOpenAICallbacks *callbacks, void *userdata) {
    ScopeRace *race = userdata;
    pthread_mutex_lock(&race->lock);
    if (!clay_scope_acquire(scope, NULL, NULL)) {
        race->acquire_failures++;
    } else {
        if (++race->in_flight > race->peak) race->peak = race->in_flight;
        clay_scope_release(scope);
        race->in_flight--;
        race->releases++;
    }
    pthread_mutex_unlock(&race->lock);
    if (callbacks && callbacks->on_token) callbacks->on_token("offline summary", callbacks->userdata);
    return 0;
}

static void *run_race_batch(void *arg) {
    ClayJson *result = call(arg, "{\"tasks\":[{\"description\":\"a\",\"prompt\":\"a\"},{\"description\":\"b\",\"prompt\":\"b\"}]}");
    assert(ok(result));
    clay_json_free(result);
    return NULL;
}

static void test_scope_sharing_under_workers(void) {
    ClayCommands c = {0};
    clay_array_init(&c.providers, sizeof(ClayConnectedProvider));
    clay_array_init(&c.tasks, sizeof(ClayBackgroundTask *));
    clay_array_init(&c.plan.todos, sizeof(ClayTodoItem));
    clay_array_init(&c.mcp_servers, sizeof(ClayMcpServer *));
    c.mcp_connect_attempted = 1;
    c.mode = CLAY_MODE_ACT;
    c.scopes = clay_scope_registry_create();
    c.chat = clay_chat_create("system");
    assert(c.scopes && c.chat);
    assert(setenv("OPENAI_API_KEY", "offline-key", 1) == 0);
    clay_commands_load_provider(&c, clay_commands_find_provider_type("openai"));
    c.selected_provider = "openai";
    c.selected_model = "model-a";
    ScopeRace race = {0};
    assert(!pthread_mutex_init(&race.lock, NULL));
    assert(!pthread_cond_init(&race.cond, NULL));
    c.completion_test_hook = race_probe;
    c.completion_test_userdata = &race;
    for (int round = 0; round < 20; round++) {
        pthread_t batch;
        assert(!pthread_create(&batch, NULL, run_race_batch, &c));
        assert(clay_commands_run_completion(&c, NULL, NULL, 1, NULL, NULL) == 0);
        assert(!pthread_join(batch, NULL));
    }
    /* Main plus two workers: only one holder at a time, every acquire released. */
    assert(race.acquire_failures == 0);
    assert(race.peak == 1);
    assert(race.releases == 60);
    c.completion_test_hook = NULL;
    c.completion_test_userdata = NULL;
    c.selected_provider = NULL;
    c.selected_model = NULL;
    ClayConnectedProvider *provider = clay_commands_find_provider(&c, "openai");
    assert(clay_config_save(provider->config) == 0);
    assert(clay_commands_logout_provider(&c, "openai") == 0);
    clay_array_free(&c.providers);
    clay_scope_registry_destroy(c.scopes);
    clay_chat_destroy(c.chat);
    pthread_cond_destroy(&race.cond);
    pthread_mutex_destroy(&race.lock);
    assert(unsetenv("OPENAI_API_KEY") == 0);
}

static void test_scope_wakeup(void) {
    ClayScopeRegistry *registry = clay_scope_registry_create();
    assert(registry);
    ClayScope *scope = clay_scope_get(registry, 1, "endpoint", "wake");
    assert(scope);
    /* Cancellation polling is pushed far out so only release() can wake the waiter. */
    clay_scope_test_cancel_poll_ms(scope, 5000);
    assert(clay_scope_acquire(scope, NULL, NULL));
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, wake_scope, scope));
    struct timespec started, now, deadline;
    assert(!clock_gettime(CLOCK_MONOTONIC, &started));
    while (!clay_scope_test_waits(scope)) {
        assert(!clock_gettime(CLOCK_MONOTONIC, &now));
        assert((now.tv_sec - started.tv_sec) * 1000 + (now.tv_nsec - started.tv_nsec) / 1000000 < 1000);
        clay_term_sleep_ms(1);
    }
    int waits = clay_scope_test_waits(scope);
    clay_term_sleep_ms(50);
    /* Temporal detector: a waiter that only polls still advances waits. */
    int waited = clay_scope_test_waits(scope);
    assert(waited == waits);
    clay_scope_release(scope);
    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_sec++;
    assert(!pthread_timedjoin_np(thread, NULL, &deadline));
    assert(!clock_gettime(CLOCK_MONOTONIC, &now));
    long elapsed_ms = (now.tv_sec - started.tv_sec) * 1000 + (now.tv_nsec - started.tv_nsec) / 1000000;
    assert(elapsed_ms < 1000);
    /* The woken waiter returned its permit; nobody is stuck holding it. */
    assert(clay_scope_acquire(scope, NULL, NULL));
    clay_scope_release(scope);
    clay_scope_test_cancel_poll_ms(scope, 1);
    clay_scope_registry_destroy(registry);
}

static void test_scope_wait(void) {
    ClayScopeRegistry *registry = clay_scope_registry_create();
    assert(registry);
    ClayScope *scope = clay_scope_get(registry, 1, "endpoint", "a");
    ClayScope *other = clay_scope_get(registry, 1, "endpoint", "b");
    assert(scope && other);
    assert(clay_scope_acquire(scope, NULL, NULL));
    WaitProbe independent = {.scope = other};
    assert(!pthread_mutex_init(&independent.lock, NULL));
    assert(!pthread_cond_init(&independent.cond, NULL));
    pthread_t other_thread;
    assert(!pthread_create(&other_thread, NULL, wait_scope, &independent));
    struct timespec deadline;
    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_sec++;
    assert(!pthread_timedjoin_np(other_thread, NULL, &deadline));
    assert(independent.result);
    pthread_cond_destroy(&independent.cond);
    pthread_mutex_destroy(&independent.lock);
    WaitProbe p = {.scope = scope};
    assert(!pthread_mutex_init(&p.lock, NULL));
    assert(!pthread_cond_init(&p.cond, NULL));
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, wait_scope, &p));
    pthread_mutex_lock(&p.lock);
    while (p.checked < 2) pthread_cond_wait(&p.cond, &p.lock);
    p.cancelled = 1;
    pthread_mutex_unlock(&p.lock);
    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_sec++;
    assert(!pthread_timedjoin_np(thread, NULL, &deadline));
    assert(!p.result);
    pthread_mutex_lock(&p.lock);
    p.checked = p.cancelled = 0;
    pthread_mutex_unlock(&p.lock);
    assert(!pthread_create(&thread, NULL, wait_scope, &p));
    pthread_mutex_lock(&p.lock);
    while (p.checked < 2) pthread_cond_wait(&p.cond, &p.lock);
    pthread_mutex_unlock(&p.lock);
    clay_scope_release(scope);
    assert(!clock_gettime(CLOCK_REALTIME, &deadline));
    deadline.tv_sec++;
    assert(!pthread_timedjoin_np(thread, NULL, &deadline));
    assert(p.result);
    int calls = 0;
    assert(!clay_scope_acquire(scope, abort_after_acquire, &calls));
    assert(calls == 2);
    assert(clay_scope_acquire(scope, NULL, NULL));
    clay_scope_release(scope);
    pthread_cond_destroy(&p.cond);
    pthread_mutex_destroy(&p.lock);
    clay_scope_registry_destroy(registry);
}

static void test_scopes(void) {
    ClayScopeRegistry *registry = clay_scope_registry_create();
    ClayScopeRegistry *other = clay_scope_registry_create();
    assert(registry && other);
    pthread_barrier_t barrier;
    assert(pthread_barrier_init(&barrier, NULL, 8) == 0);
    pthread_t threads[8];
    ScopeThread runs[8] = {0};
    for (size_t i = 0; i < 8; i++) {
        runs[i].registry = registry;
        runs[i].barrier = &barrier;
        assert(pthread_create(&threads[i], NULL, scope_thread, &runs[i]) == 0);
    }
    for (size_t i = 0; i < 8; i++) {
        assert(pthread_join(threads[i], NULL) == 0);
        assert(runs[i].scope == runs[0].scope);
    }
    pthread_barrier_destroy(&barrier);
    ClayScope *same = runs[0].scope;
    assert(clay_scope_get(registry, 2, "https://user:secret@example.test", "model-secret") != same);
    assert(clay_scope_get(registry, 1, "https://other.test", "model-secret") != same);
    assert(clay_scope_get(registry, 1, "https://user:secret@example.test", "other") != same);
    assert(clay_scope_get(other, 1, "https://user:secret@example.test", "model-secret") != same);
    assert(!clay_scope_get(registry, 0, "x", "y"));
    /* Scope consists only of generation, two hashes and the next pointer. */
    struct registry_layout { pthread_mutex_t lock; unsigned char key[32]; void *head; };
    assert(!memmem(registry, sizeof(struct registry_layout), "secret", 6));
    size_t scope_size = sizeof(uint64_t) + 64 + sizeof(void *);
    assert(!memmem(same, scope_size, "secret", 6));
    assert(clay_scope_get(registry, 1, "https://user:secret@example.test", "model-secret") == same);
    clay_scope_registry_destroy(other);
    clay_scope_registry_destroy(registry);
}

/* Three completions in one scope reach the transport one at a time. */
static void test_scope_admission_limits_http(void) {
    ClayCommands c = {0};
    clay_array_init(&c.providers, sizeof(ClayConnectedProvider));
    c.scopes = clay_scope_registry_create();
    c.selected_provider = "openai-codex"; c.selected_model = "model";
    ClayProviderConfig config = {0};
    config.access_token = strdup("old"); config.refresh_token = strdup("old-refresh");
    config.account_id = strdup("account"); config.expires_at = 4102444800LL;
    ClayConnectedProvider provider = {0};
    provider.config = &config; provider.environment_override = 1; provider.generation = 1;
    provider.type = clay_commands_find_provider_type("openai-codex");
    clay_array_init(&provider.models, sizeof(char *));
    clay_array_push_val(&c.providers, &provider);
    review_mock = 18; http_inflight = http_peak = 0;
    ReviewRun runs[3] = {{&c, -99}, {&c, -99}, {&c, -99}};
    pthread_t threads[3];
    for (int i = 0; i < 3; i++)
        assert(!pthread_create(&threads[i], NULL, review_run, &runs[i]));
    for (int i = 0; i < 3; i++)
        assert(!pthread_join(threads[i], NULL));
    assert(http_peak == 1 && http_inflight == 0);
    review_mock = 0;
    ClayConnectedProvider *shared = clay_array_get(&c.providers, 0);
    for (size_t i = 0; i < shared->models.count; i++) free(*(char **)clay_array_get(&shared->models, i));
    clay_array_free(&shared->models);
    clay_scope_registry_destroy(c.scopes); clay_array_free(&c.providers);
    free(config.access_token); free(config.refresh_token); free(config.account_id);
}

static void add_rate_header(ClayHttpResponse *r, const char *name, const char *value) {
    ClayHttpRateHeader *h = &r->rate_headers[r->rate_header_count++];
    h->name = name;
    snprintf(h->value, sizeof(h->value), "%s", value);
}

/* 429 cooldown and pacing are per scope; a success halves the spacing back to zero. */
static void test_scope_cooldown_and_pacing(void) {
    ClayScopeRegistry *registry = clay_scope_registry_create();
    assert(registry);
    ClayScope *scope = clay_scope_get(registry, 1, "endpoint", "cool");
    ClayScope *other = clay_scope_get(registry, 1, "endpoint", "other");
    assert(scope && other);
    ClayHttpResponse limited;
    memset(&limited, 0, sizeof(limited));
    limited.status = 429;

    /* Missing Retry-After: base cooldown and base pacing. */
    clay_scope_note_response(scope, &limited, 0);
    assert(clay_scope_test_pace(scope) == 1000);
    uint64_t left = clay_scope_test_cooldown_left(scope);
    assert(left > 0 && left <= 1000);
    assert(!clay_scope_try_acquire(scope));
    /* Isolation: a 429 on one scope does not delay another. */
    assert(clay_scope_try_acquire(other));
    clay_scope_release(other);

    /* A waiting admission sleeps out the cooldown; the next is paced 1000 ms after it. */
    uint64_t t0 = mono_ms_now();
    assert(clay_scope_acquire(scope, NULL, NULL) == 1);
    assert(mono_ms_now() - t0 >= 900);
    clay_scope_release(scope);
    t0 = mono_ms_now();
    assert(clay_scope_acquire(scope, NULL, NULL) == 1);
    assert(mono_ms_now() - t0 >= 900);
    clay_scope_release(scope);

    /* Duplicated Retry-After is a conflict: strictest cap, and cancel during the wait
       returns without a permit. */
    ClayScope *cancelled = clay_scope_get(registry, 1, "endpoint", "cancel");
    ClayHttpResponse conflict = limited;
    add_rate_header(&conflict, "retry-after", "5");
    add_rate_header(&conflict, "retry-after", "6");
    clay_scope_note_response(cancelled, &conflict, 0);
    assert(clay_scope_test_cooldown_left(cancelled) >= 29000);
    uint64_t abort_after = mono_ms_now() + 50;
    t0 = mono_ms_now();
    assert(clay_scope_acquire(cancelled, abort_when_due, &abort_after) == 0);
    assert(mono_ms_now() - t0 < 2000);

    /* A valid Retry-After is honoured; dropped headers are unknown, so strictest. */
    ClayScope *honoured = clay_scope_get(registry, 1, "endpoint", "retry");
    ClayHttpResponse valid = limited;
    add_rate_header(&valid, "retry-after", "2");
    clay_scope_note_response(honoured, &valid, 0);
    left = clay_scope_test_cooldown_left(honoured);
    assert(left > 1500 && left <= 2000);
    ClayScope *dropped = clay_scope_get(registry, 1, "endpoint", "dropped");
    ClayHttpResponse drop = limited;
    drop.rate_headers_dropped = 1;
    clay_scope_note_response(dropped, &drop, 0);
    assert(clay_scope_test_cooldown_left(dropped) >= 29000);

    /* Permanent quota is not a rate limit: no cooldown, no pacing. */
    ClayScope *quota = clay_scope_get(registry, 1, "endpoint", "quota");
    clay_scope_note_response(quota, &limited, 1);
    assert(clay_scope_test_pace(quota) == 0 && clay_scope_test_cooldown_left(quota) == 0);

    /* Recovery: each 429 doubles pacing; each success halves it back to zero. */
    ClayScope *recover = clay_scope_get(registry, 1, "endpoint", "recover");
    clay_scope_note_response(recover, &limited, 0);
    clay_scope_note_response(recover, &limited, 0);
    assert(clay_scope_test_pace(recover) == 2000);
    ClayHttpResponse ok;
    memset(&ok, 0, sizeof(ok));
    ok.status = 200;
    clay_scope_note_response(recover, &ok, 0);
    assert(clay_scope_test_pace(recover) == 1000);
    clay_scope_note_response(recover, &ok, 0);
    assert(clay_scope_test_pace(recover) == 0);
    clay_scope_registry_destroy(registry);
}

/* One 429 then 200 through the real Codex loop: the 429 cools and paces the scope,
   and the success after it halves the pacing (1000 -> 500). */
static void test_scope_rate_limit_http(void) {
    ClayCommands c = {0};
    clay_array_init(&c.providers, sizeof(ClayConnectedProvider));
    c.scopes = clay_scope_registry_create();
    c.selected_provider = "openai-codex"; c.selected_model = "model";
    ClayProviderConfig config = {0};
    config.access_token = strdup("old"); config.refresh_token = strdup("old-refresh");
    config.account_id = strdup("account"); config.expires_at = 4102444800LL;
    ClayConnectedProvider provider = {0};
    provider.config = &config; provider.environment_override = 1; provider.generation = 1;
    provider.type = clay_commands_find_provider_type("openai-codex");
    clay_array_init(&provider.models, sizeof(char *));
    clay_array_push_val(&c.providers, &provider);
    review_mock = 19; http_calls = 0;
    http_scope = clay_scope_get(c.scopes, 1, NULL, "model");
    ReviewRun run = {&c, -99};
    review_run(&run);
    assert(http_calls == 2);
    ClayScope *scope = clay_scope_get(c.scopes, 1, NULL, "model");
    assert(http_pace_seen == 1000); /* the 429 paced the scope before the retry */
    assert(scope && clay_scope_test_pace(scope) == 0); /* the success halved 1000 below base */
    review_mock = 0;
    ClayConnectedProvider *shared = clay_array_get(&c.providers, 0);
    for (size_t i = 0; i < shared->models.count; i++) free(*(char **)clay_array_get(&shared->models, i));
    clay_array_free(&shared->models);
    clay_scope_registry_destroy(c.scopes); clay_array_free(&c.providers);
    free(config.access_token); free(config.refresh_token); free(config.account_id);
}

/* Tokens-per-minute budget per scope: estimates, settlement, rejection, cancel, window roll. */
static void test_scope_tpm_budget(void) {
    ClayScopeRegistry *registry = clay_scope_registry_create();
    assert(registry);
    ClayScope *scope = clay_scope_get(registry, 1, "endpoint", "tpm");
    assert(scope);
    uint64_t ticket = 99;
    /* No tpm configured: no ticket and nothing recorded. */
    assert(clay_scope_admit(scope, 2000, NULL, NULL, &ticket) == 1 && ticket == 0);
    clay_scope_finish(scope, ticket, 0, 0);
    assert(clay_scope_test_spent(scope) == 0);

    clay_scope_set_tpm(scope, 10000);
    clay_scope_test_tpm_window(scope, 300);
    /* Estimate for 2000 request bytes: 1000 + 4096 = 5096 tokens. */
    assert(clay_scope_admit(scope, 2000, NULL, NULL, &ticket) == 1 && ticket != 0);
    clay_scope_finish(scope, ticket, 1, 3000); /* usage below the estimate frees budget */
    assert(clay_scope_test_spent(scope) == 3000);
    assert(clay_scope_admit(scope, 2000, NULL, NULL, &ticket) == 1);
    clay_scope_finish(scope, ticket, 0, 0); /* no usage: the estimate stays */
    assert(clay_scope_test_spent(scope) == 3000 + 5096);

    /* Can never fit: refused at once, and nothing is held. */
    assert(clay_scope_admit(scope, 20000, NULL, NULL, &ticket) == -1);
    assert(clay_scope_try_acquire(scope));
    clay_scope_release(scope);

    /* Over budget waits; cancel while waiting holds nothing. */
    uint64_t abort_after = mono_ms_now() + 50;
    uint64_t t0 = mono_ms_now();
    assert(clay_scope_admit(scope, 2000, abort_when_due, &abort_after, &ticket) == 0);
    assert(mono_ms_now() - t0 < 250);
    assert(clay_scope_try_acquire(scope));
    clay_scope_release(scope);

    /* Once the window rolls, the same request is admitted. */
    t0 = mono_ms_now();
    assert(clay_scope_admit(scope, 2000, NULL, NULL, &ticket) == 1);
    assert(mono_ms_now() - t0 >= 200);
    clay_scope_finish(scope, ticket, 1, 100);
    assert(clay_scope_test_spent(scope) == 100);
    clay_scope_registry_destroy(registry);
}

/* Codex loop with tpm 20000: the first attempt reports 17000 tokens, so the second must
   wait for that entry to leave the window before it is sent. */
static void test_provider_tpm_window(void) {
    ClayCommands c = {0};
    clay_array_init(&c.providers, sizeof(ClayConnectedProvider));
    c.scopes = clay_scope_registry_create();
    c.selected_provider = "openai-codex"; c.selected_model = "model";
    ClayProviderConfig config = {0};
    config.access_token = strdup("old"); config.refresh_token = strdup("old-refresh");
    config.account_id = strdup("account"); config.expires_at = 4102444800LL;
    config.tpm = 20000;
    ClayConnectedProvider provider = {0};
    provider.config = &config; provider.environment_override = 1; provider.generation = 1;
    provider.type = clay_commands_find_provider_type("openai-codex");
    clay_array_init(&provider.models, sizeof(char *));
    clay_array_push_val(&c.providers, &provider);
    ClayScope *scope = clay_scope_get(c.scopes, 1, NULL, "model");
    clay_scope_test_tpm_window(scope, 300);
    review_mock = 20; http_calls = 0;
    ReviewRun run = {&c, -99};
    review_run(&run);
    review_run(&run);
    assert(http_calls == 2);
    assert(http_second_ms - http_first_ms >= 250);
    uint64_t spent = clay_scope_test_spent(scope);
    assert(spent >= 4096 && spent <= 20000); /* second attempt: estimate only */
    review_mock = 0;
    ClayConnectedProvider *shared = clay_array_get(&c.providers, 0);
    for (size_t i = 0; i < shared->models.count; i++) free(*(char **)clay_array_get(&shared->models, i));
    clay_array_free(&shared->models);
    clay_scope_registry_destroy(c.scopes); clay_array_free(&c.providers);
    free(config.access_token); free(config.refresh_token); free(config.account_id);
}

/* tpm is optional: saved when set, absent or zero means unknown. */
static void test_provider_tpm_config(void) {
    ClayProviderConfig config = {0};
    config.id = "tpm-check";
    config.base_url = "https://example.test/v1";
    config.tpm = 1234;
    assert(clay_config_save(&config) == 0);
    ClayProviderConfig *loaded = clay_config_load("tpm-check");
    assert(loaded && loaded->tpm == 1234);
    clay_config_free(loaded);
    config.tpm = 0;
    assert(clay_config_save(&config) == 0);
    loaded = clay_config_load("tpm-check");
    assert(loaded && loaded->tpm == 0);
    clay_config_free(loaded);
    assert(clay_config_remove("tpm-check") == 0);
}

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int entered, release;
} LogoutGate;

static int logout_gate_hook(ClayScope *scope, const ClayOpenAICallbacks *callbacks,
                            void *userdata) {
    LogoutGate *gate = userdata;
    (void)scope; (void)callbacks;
    pthread_mutex_lock(&gate->lock);
    gate->entered = 1;
    pthread_cond_broadcast(&gate->cond);
    while (!gate->release) pthread_cond_wait(&gate->cond, &gate->lock);
    pthread_mutex_unlock(&gate->lock);
    return 0;
}

static void *logout_gate_run(void *arg) {
    ClayCommands *commands = arg;
    ClayJson *messages = clay_json_array();
    int rc = clay_commands_run_completion(commands, messages, NULL, 1, NULL, NULL);
    clay_json_free(messages);
    return (void *)(intptr_t)rc;
}

typedef struct {
    ClayCommands *commands;
    LogoutGate *gate;
    int rc, done;
} LogoutRun;

static void *logout_gate_logout(void *arg) {
    LogoutRun *run = arg;
    run->rc = clay_commands_logout_provider(run->commands, "openai-codex");
    pthread_mutex_lock(&run->gate->lock);
    run->done = 1;
    pthread_cond_broadcast(&run->gate->cond);
    pthread_mutex_unlock(&run->gate->lock);
    return NULL;
}

static void test_logout_waits_for_workers(void) {
    assert(setenv("OPENAI_CODEX_ACCESS_TOKEN", "old-access", 1) == 0);
    assert(setenv("OPENAI_CODEX_REFRESH_TOKEN", "old-refresh", 1) == 0);
    assert(setenv("OPENAI_CODEX_ACCOUNT_ID", "account", 1) == 0);
    ClayCommands c = {0};
    clay_array_init(&c.providers, sizeof(ClayConnectedProvider));
    c.scopes = clay_scope_registry_create();
    c.selected_provider = strdup("openai-codex");
    c.selected_model = strdup("model");
    const ClayProviderType *type = clay_commands_find_provider_type("openai-codex");
    clay_commands_load_provider(&c, type);
    ClayConnectedProvider *provider = clay_commands_find_provider(&c, type->id);
    assert(provider && clay_config_save(provider->config) == 0);

    LogoutGate gate = {0};
    assert(!pthread_mutex_init(&gate.lock, NULL));
    assert(!pthread_cond_init(&gate.cond, NULL));
    c.completion_test_hook = logout_gate_hook;
    c.completion_test_userdata = &gate;

    pthread_t worker, logout_thread;
    assert(!pthread_create(&worker, NULL, logout_gate_run, &c));
    pthread_mutex_lock(&gate.lock);
    while (!gate.entered) pthread_cond_wait(&gate.cond, &gate.lock);
    pthread_mutex_unlock(&gate.lock);

    LogoutRun run = {&c, &gate, -99, 0};
    assert(!pthread_create(&logout_thread, NULL, logout_gate_logout, &run));
    pthread_mutex_lock(&gate.lock);
    struct timespec deadline;
    assert(clock_gettime(CLOCK_REALTIME, &deadline) == 0);
    deadline.tv_nsec += 50000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    while (!run.done) {
        int rc = pthread_cond_timedwait(&gate.cond, &gate.lock, &deadline);
        if (rc == ETIMEDOUT) break;
        assert(rc == 0);
    }
    assert(!run.done);
    pthread_mutex_unlock(&gate.lock);
    pthread_mutex_lock(&gate.lock);
    gate.release = 1;
    pthread_cond_broadcast(&gate.cond);
    pthread_mutex_unlock(&gate.lock);

    void *worker_rc = NULL;
    assert(!pthread_join(worker, &worker_rc));
    assert(!pthread_join(logout_thread, NULL));
    assert(worker_rc == 0 && run.rc == 0 && run.done);
    assert(!clay_commands_find_provider(&c, type->id));

    pthread_cond_destroy(&gate.cond);
    pthread_mutex_destroy(&gate.lock);
    clay_array_free(&c.providers);
    clay_scope_registry_destroy(c.scopes);
    free(c.selected_provider);
    free(c.selected_model);
}

static void test_provider_replacement_isolation(void) {
    assert(setenv("OPENAI_CODEX_ACCESS_TOKEN", "old-access", 1) == 0);
    assert(setenv("OPENAI_CODEX_REFRESH_TOKEN", "old-refresh", 1) == 0);
    assert(setenv("OPENAI_CODEX_ACCOUNT_ID", "account", 1) == 0);
    ClayCommands c = {0};
    clay_array_init(&c.providers, sizeof(ClayConnectedProvider));
    c.scopes = clay_scope_registry_create();
    const ClayProviderType *type = clay_commands_find_provider_type("openai-codex");
    clay_commands_load_provider(&c, type);
    ClayConnectedProvider *provider = clay_commands_find_provider(&c, type->id);
    assert(provider);

    provider->codex_version = 7;
    provider->grok_version = 3;
    provider->codex_refresh_epoch = 5;
    provider->grok_refresh_epoch = 4;
    provider->codex_refresh_active = 1;
    provider->grok_refresh_active = 1;
    provider->codex_refresh_failure_epoch = 5;
    provider->grok_refresh_failure_epoch = 4;
    provider->codex_refresh_start_version = 7;
    provider->grok_refresh_start_version = 3;
    provider->codex_refresh_unpersisted = 1;
    provider->grok_refresh_unpersisted = 1;
    provider->codex_auth_invalid = 1;
    provider->codex_retry_at = provider->grok_retry_at = time(NULL) + 3600;
    uint64_t generation = provider->generation;
    ClayCodexCredentials stale = {"stale", "stale-r", NULL, "account", 1};
    ClayOpenAICodex *client = clay_openai_codex_create(&stale, "model");
    assert(client);

    clay_commands_load_provider(&c, type);
    assert(provider->generation > generation);
    assert(provider->codex_version == 8 && provider->grok_version == 4);
    assert(provider->codex_refresh_epoch == 6 && provider->grok_refresh_epoch == 5);
    assert(!provider->codex_refresh_active && !provider->grok_refresh_active);
    assert(!provider->codex_refresh_failure_epoch && !provider->grok_refresh_failure_epoch);
    assert(!provider->codex_refresh_unpersisted && !provider->grok_refresh_unpersisted);
    assert(!provider->codex_auth_invalid);
    assert(!provider->codex_retry_at && !provider->grok_retry_at);
    assert(clay_commands_save_codex_credentials(provider, client, 7) == 1);
    assert(strcmp(provider->config->refresh_token, "stale-r") != 0);
    clay_openai_codex_destroy(client);

    provider->codex_version = UINT64_MAX;
    provider->codex_refresh_epoch = UINT64_MAX;
    generation = provider->generation;
    ClayProviderConfig *config = provider->config;
    assert(clay_commands_codex_refresh_begin(provider, UINT64_MAX, NULL, NULL) == -1);
    clay_commands_codex_refresh_end(provider, -1);
    assert(provider->codex_refresh_epoch == UINT64_MAX);
    clay_commands_load_provider(&c, type);
    assert(provider->generation == generation && provider->config == config);

    while (c.providers.count) {
        ClayConnectedProvider *p = clay_array_get(&c.providers, 0);
        assert(clay_config_save(p->config) == 0);
        assert(clay_commands_logout_provider(&c, p->type->id) == 0);
    }
    clay_array_free(&c.providers);
    clay_scope_registry_destroy(c.scopes);
}

static void test_provider_generations(void) {
    ClayCommands c = {0};
    clay_array_init(&c.providers, sizeof(ClayConnectedProvider));
    static const ClayProviderType types[] = {
      {"one", "one", "https://example.com"}, {"two", "two", "https://example.com"},
      {"three", "three", "https://example.com"}, {"four", "four", "https://example.com"},
      {"five", "five", "https://example.com"}, {"six", "six", "https://example.com"},
      {"seven", "seven", "https://example.com"}, {"eight", "eight", "https://example.com"},
      {"nine", "nine", "https://example.com"}
    };
    const char *envs[] = {"CLAY_ONE_API_KEY", "CLAY_TWO_API_KEY", "CLAY_THREE_API_KEY", "CLAY_FOUR_API_KEY", "CLAY_FIVE_API_KEY", "CLAY_SIX_API_KEY", "CLAY_SEVEN_API_KEY", "CLAY_EIGHT_API_KEY", "CLAY_NINE_API_KEY"};
    for (size_t i = 0; i < 9; i++) {
        assert(setenv(envs[i], "key", 1) == 0);
        clay_commands_load_provider(&c, &types[i]);
        assert(clay_commands_provider_generation(&c, types[i].id) == i + 1);
    }
    assert(c.providers.count == 9 && c.providers.cap > 8);
    uint64_t ninth = clay_commands_provider_generation(&c, "nine");
    ClayConnectedProvider *first = clay_commands_find_provider(&c, "one");
    assert(clay_config_save(first->config) == 0);
    assert(clay_commands_logout_provider(&c, "one") == 0);
    assert(clay_commands_provider_generation(&c, "nine") == ninth);
    clay_commands_load_provider(&c, &types[0]);
    assert(clay_commands_provider_generation(&c, "one") > ninth);
    ClayScopeRegistry *registry = clay_scope_registry_create();
    assert(registry);
    uint64_t before = clay_commands_provider_generation(&c, "two");
    ClayScope *old_scope = clay_scope_get(registry, before, "https://example.com", "model");
    assert(old_scope);
    clay_commands_load_provider(&c, &types[1]);
    assert(clay_commands_provider_generation(&c, "two") > before);
    assert(clay_scope_get(registry, clay_commands_provider_generation(&c, "two"),
                          "https://example.com", "model") != old_scope);
    assert(clay_scope_get(registry, before, "https://example.com", "model") == old_scope);
    clay_scope_registry_destroy(registry);
    assert(setenv("OPENAI_CODEX_ACCESS_TOKEN", "old", 1) == 0);
    assert(setenv("OPENAI_CODEX_REFRESH_TOKEN", "refresh", 1) == 0);
    assert(setenv("OPENAI_CODEX_ACCOUNT_ID", "account", 1) == 0);
    clay_commands_load_provider(&c, clay_commands_find_provider_type("openai-codex"));
    ClayConnectedProvider *codex = clay_commands_find_provider(&c, "openai-codex");
    assert(codex);
    uint64_t generation = codex->generation;
    ClayCodexCredentials rotated = {"new", "rotated", NULL, "account", 123};
    ClayOpenAICodex *client = clay_openai_codex_create(&rotated, NULL);
    assert(client && clay_commands_save_codex_credentials(codex, client, codex->codex_version) == 0);
    assert(codex->generation == generation && !strcmp(codex->config->refresh_token, "rotated"));
    clay_openai_codex_destroy(client);
    uint64_t current = clay_commands_provider_generation(&c, "two");
    c.provider_generation = UINT64_MAX;
    clay_commands_load_provider(&c, &types[1]);
    assert(clay_commands_provider_generation(&c, "two") == current);
    while (c.providers.count) {
        ClayConnectedProvider *p = clay_array_get(&c.providers, 0);
        assert(clay_config_save(p->config) == 0);
        assert(clay_commands_logout_provider(&c, p->type->id) == 0);
    }
    clay_array_free(&c.providers);
}


typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t main_thread;
    ClayScope *worker_scope;
    ClayScope *main_scope;
    int entered;
    int release;
    int calls;
} CompletionProbe;

static int completion_probe(ClayScope *scope, const ClayOpenAICallbacks *callbacks, void *userdata) {
    CompletionProbe *p = userdata;
    pthread_mutex_lock(&p->lock);
    p->calls++;
    if (pthread_equal(pthread_self(), p->main_thread)) {
        p->main_scope = scope;
    } else {
        p->worker_scope = scope;
        p->entered = 1;
        pthread_cond_broadcast(&p->cond);
        while (!p->release) pthread_cond_wait(&p->cond, &p->lock);
    }
    pthread_mutex_unlock(&p->lock);
    if (callbacks && callbacks->on_token) callbacks->on_token("offline summary", callbacks->userdata);
    return 0;
}

static void *run_batch(void *arg) {
    ClayJson *result = call(arg, "{\"tasks\":[{\"description\":\"branch\",\"prompt\":\"test\"}]}");
    assert(ok(result));
    clay_json_free(result);
    return NULL;
}

typedef struct {
    pthread_barrier_t barrier;
    int results[2];
    pthread_mutex_t lock;
    int next;
} CancelProbe;

static int cancel_probe(ClayScope *scope, const ClayOpenAICallbacks *callbacks, void *userdata) {
    (void)scope;
    CancelProbe *p = userdata;
    pthread_mutex_lock(&p->lock);
    int index = p->next++;
    pthread_mutex_unlock(&p->lock);
    pthread_barrier_wait(&p->barrier);
    pthread_barrier_wait(&p->barrier);
    p->results[index] = callbacks->should_abort(callbacks->userdata);
    return 1;
}

static void *run_cancel_batch(void *arg) {
    ClayJson *result = call(arg, "{\"tasks\":[{\"description\":\"a\",\"prompt\":\"a\"},{\"description\":\"b\",\"prompt\":\"b\"}]}");
    assert(!ok(result));
    clay_json_free(result);
    return NULL;
}

static void test_completion_scopes(void) {
    ClayCommands c = {0};
    clay_array_init(&c.providers, sizeof(ClayConnectedProvider));
    clay_array_init(&c.tasks, sizeof(ClayBackgroundTask *));
    clay_array_init(&c.plan.todos, sizeof(ClayTodoItem));
    clay_array_init(&c.mcp_servers, sizeof(ClayMcpServer *));
    c.mcp_connect_attempted = 1;
    c.mode = CLAY_MODE_ACT;
    c.scopes = clay_scope_registry_create();
    c.chat = clay_chat_create("system");
    assert(c.scopes && c.chat);
    assert(setenv("OPENAI_API_KEY", "offline-key", 1) == 0);
    const ClayProviderType *type = clay_commands_find_provider_type("openai");
    assert(type);
    clay_commands_load_provider(&c, type);
    assert(clay_commands_find_provider(&c, "openai"));
    c.selected_provider = "openai";
    c.selected_model = "model-a";
    CompletionProbe p = {.main_thread = pthread_self()};
    assert(!pthread_mutex_init(&p.lock, NULL));
    assert(!pthread_cond_init(&p.cond, NULL));
    c.completion_test_hook = completion_probe;
    c.completion_test_userdata = &p;
    pthread_t batch;
    assert(!pthread_create(&batch, NULL, run_batch, &c));
    pthread_mutex_lock(&p.lock);
    while (!p.entered) pthread_cond_wait(&p.cond, &p.lock);
    pthread_mutex_unlock(&p.lock);
    assert(clay_commands_run_completion(&c, NULL, NULL, 1, NULL, NULL) == 0);
    assert(p.main_scope && p.main_scope == p.worker_scope);
    ClayScope *old = p.main_scope;
    assert(clay_scope_get(c.scopes, clay_commands_provider_generation(&c, "openai"),
                          clay_commands_find_provider(&c, "openai")->config->base_url, "model-a") == old);
    pthread_mutex_lock(&p.lock);
    p.release = 1;
    pthread_cond_broadcast(&p.cond);
    pthread_mutex_unlock(&p.lock);
    assert(!pthread_join(batch, NULL));
    CancelProbe cancel = {0};
    assert(!pthread_barrier_init(&cancel.barrier, NULL, 3));
    assert(!pthread_mutex_init(&cancel.lock, NULL));
    c.completion_test_hook = cancel_probe;
    c.completion_test_userdata = &cancel;
    clay_term_init();
    assert(!pthread_create(&batch, NULL, run_cancel_batch, &c));
    pthread_barrier_wait(&cancel.barrier);
    assert(!raise(SIGINT));
    pthread_barrier_wait(&cancel.barrier);
    assert(!pthread_join(batch, NULL));
    assert(cancel.results[0] && cancel.results[1]);
    assert(!clay_term_take_interrupt());
    cancel.next = 0;
    assert(!pthread_create(&batch, NULL, run_cancel_batch, &c));
    pthread_barrier_wait(&cancel.barrier);
    pthread_barrier_wait(&cancel.barrier);
    assert(!pthread_join(batch, NULL));
    assert(!cancel.results[0] && !cancel.results[1]);
    pthread_barrier_destroy(&cancel.barrier);
    pthread_mutex_destroy(&cancel.lock);
    c.completion_test_hook = completion_probe;
    c.completion_test_userdata = &p;
    c.selected_model = "model-b";
    assert(clay_commands_run_completion(&c, NULL, NULL, 1, NULL, NULL) == 0);
    assert(p.main_scope != old);
    c.selected_model = "model-a";
    clay_commands_load_provider(&c, type);
    assert(clay_commands_run_completion(&c, NULL, NULL, 1, NULL, NULL) == 0);
    assert(p.main_scope != old);
    for (int i = 1; i <= 3; i++) {
        int calls = p.calls;
        c.selected_model = "failure-model";
        clay_scope_test_fail_allocation(c.scopes, i);
        assert(clay_commands_run_completion(&c, NULL, NULL, 1, NULL, NULL) == -1);
        assert(p.calls == calls);
    }
    clay_scope_test_fail_allocation(c.scopes, 0);
    assert(clay_commands_run_completion(&c, NULL, NULL, 1, NULL, NULL) == 0);

    /* Exercise both subscription clients and Grok's API-key path offline. */
    assert(setenv("GROK_ACCESS_TOKEN", "offline-access", 1) == 0);
    assert(setenv("GROK_REFRESH_TOKEN", "offline-refresh", 1) == 0);
    const char *ids[] = {"openai-codex", "grok"};
    for (size_t i = 0; i < 2; i++) {
        const ClayProviderType *t = clay_commands_find_provider_type(ids[i]);
        clay_commands_load_provider(&c, t);
        assert(clay_commands_find_provider(&c, ids[i]));
        c.selected_provider = (char *)ids[i];
        assert(clay_commands_run_completion(&c, NULL, NULL, 1, NULL, NULL) == 0);
        assert(p.main_scope == clay_scope_get(c.scopes,
            clay_commands_provider_generation(&c, ids[i]),
            clay_commands_find_provider(&c, ids[i])->config->base_url, c.selected_model));
        assert(p.main_scope != old);
    }
    assert(unsetenv("GROK_ACCESS_TOKEN") == 0);
    assert(unsetenv("GROK_REFRESH_TOKEN") == 0);
    assert(setenv("XAI_API_KEY", "offline-key", 1) == 0);
    clay_commands_load_provider(&c, clay_commands_find_provider_type("grok"));
    assert(!clay_commands_find_provider(&c, "grok")->grok_client);
    assert(clay_commands_run_completion(&c, NULL, NULL, 1, NULL, NULL) == 0);
    assert(p.main_scope == clay_scope_get(c.scopes,
        clay_commands_provider_generation(&c, "grok"),
        clay_commands_find_provider(&c, "grok")->config->base_url, c.selected_model));
    c.selected_provider = "openai";
    ClayConnectedProvider *provider = clay_commands_find_provider(&c, "openai");
    assert(clay_config_save(provider->config) == 0);
    c.selected_provider = NULL;
    c.selected_model = NULL;
    assert(clay_commands_logout_provider(&c, "openai") == 0);
    while (c.providers.count) {
        ClayConnectedProvider *p = clay_array_get(&c.providers, 0);
        assert(clay_config_save(p->config) == 0);
        assert(clay_commands_logout_provider(&c, p->type->id) == 0);
    }
    clay_array_free(&c.providers);
    clay_scope_registry_destroy(c.scopes);
    clay_chat_destroy(c.chat);
    clay_array_free(&c.tasks);
    clay_array_free(&c.plan.todos);
    clay_array_free(&c.mcp_servers);
    pthread_cond_destroy(&p.cond);
    pthread_mutex_destroy(&p.lock);
}

int main(void) {
    test_codex_network_singleflight();
    test_grok_network_singleflight();
    test_codex_stale_http();
    test_codex_publication_waits_for_end();
    test_codex_concurrent_snapshot();
    test_grok_concurrent_snapshot();
    test_unpersisted_retry();
    test_stale_completion_results();
    test_interrupt_consumed_once();
    char template[] = "/tmp/clay_test_subagent_XXXXXX";
    char *home = mkdtemp(template);
    assert(home);
    assert(setenv("HOME", home, 1) == 0);
    test_codex_flight_foundation();
    test_codex_refresh_ownership();
    test_grok_catalog_refresh_failure();
    test_grok_invalid_grant_preserves_config();
    test_codex_invalid_grant_removes_config();

    test_scope_wait();
    test_scope_wakeup();
    test_scope_sharing_under_workers();
    test_scopes();
    test_concurrent_scope_failure();
    test_provider_generations();
    test_provider_replacement_isolation();
    test_logout_waits_for_workers();
    test_completion_scopes();
    test_scope_admission_limits_http();
    test_scope_cooldown_and_pacing();
    test_scope_rate_limit_http();
    test_scope_tpm_budget();
    test_provider_tpm_window();
    test_provider_tpm_config();
    test_subagent_parent_usage();
    test_subagent_budget_and_summary();
    test_subagent_stress_http();
    test_subagent_stress_mixed();

    ClayCommands commands;
    memset(&commands, 0, sizeof(commands));
    clay_array_init(&commands.tasks, sizeof(ClayBackgroundTask *));
    clay_array_init(&commands.plan.todos, sizeof(ClayTodoItem));
    clay_array_init(&commands.mcp_servers, sizeof(ClayMcpServer *));
    commands.mcp_connect_attempted = 1; /* no servers to dial in a test */
    commands.mode = CLAY_MODE_ACT;

    ClayJson *result = call(&commands, "{\"tasks\":[]}");
    assert(!ok(result));
    assert(strstr(error_of(result), "non-empty"));
    clay_json_free(result);

    /* One call fans out, but not without limit. */
    result = call(&commands,
                  "{\"tasks\":[{\"description\":\"a\",\"prompt\":\"p\"},"
                  "{\"description\":\"b\",\"prompt\":\"p\"},"
                  "{\"description\":\"c\",\"prompt\":\"p\"},"
                  "{\"description\":\"d\",\"prompt\":\"p\"},"
                  "{\"description\":\"e\",\"prompt\":\"p\"}]}");
    assert(!ok(result));
    assert(strstr(error_of(result), "at most 4"));
    clay_json_free(result);

    result = call(&commands, "{\"tasks\":[{\"prompt\":\"do the thing\"}]}");
    assert(!ok(result));
    assert(strstr(error_of(result), "description"));
    clay_json_free(result);

    result = call(&commands, "{\"tasks\":[{\"description\":\"step one\"}]}");
    assert(!ok(result));
    assert(strstr(error_of(result), "prompt"));
    clay_json_free(result);

    /* Plan mode refuses before anything is spawned. */
    commands.mode = CLAY_MODE_PLAN;
    result = call(&commands, "{\"tasks\":[{\"description\":\"step one\",\"prompt\":\"do it\"}]}");
    assert(!ok(result));
    assert(strstr(error_of(result), "Plan mode"));
    clay_json_free(result);
    commands.mode = CLAY_MODE_ACT;

    /* Without a chat there is nowhere to record the run. */
    result = call(&commands, "{\"tasks\":[{\"description\":\"step one\",\"prompt\":\"do it\"}]}");
    assert(!ok(result));
    assert(strstr(error_of(result), "chat"));
    clay_json_free(result);

    /* A subagent gets the working tools but neither the user's ear nor the
       ability to spawn more subagents. */
    ClayToolSet delegated;
    clay_commands_tools_build(&commands, &commands.plan, &delegated, 0);
    assert(has_tool(&delegated, "read"));
    assert(has_tool(&delegated, "shell_exec"));
    assert(has_tool(&delegated, "todowrite"));
    assert(!has_tool(&delegated, "subagent"));
    assert(!has_tool(&delegated, "ask_user"));
    size_t delegated_count = delegated.tools.count;
    clay_commands_tools_free(&delegated);

    ClayToolSet top;
    clay_commands_tools_build(&commands, &commands.plan, &top, 1);
    assert(has_tool(&top, "subagent"));
    assert(has_tool(&top, "ask_user"));
    assert(top.tools.count == delegated_count + 2);
    clay_commands_tools_free(&top);

    /* Each run gets its own file under the chat. */
    ClayChat *chat = clay_chat_create("system");
    assert(chat);
    char *first = clay_chat_subagent_path(chat, "aaaa");
    char *second = clay_chat_subagent_path(chat, "bbbb");
    assert(first && second && strcmp(first, second) != 0);
    assert(strstr(first, "/subagents/aaaa.json"));
    assert(clay_term_write_file_atomic(first, "{}", 2) == 0);
    free(first);
    free(second);
    clay_chat_destroy(chat);

    ClayJson *schema = subagent_schema();
    ClayJson *properties = clay_json_object_get(schema, "properties");
    ClayJson *tasks = clay_json_object_get(properties, "tasks");
    assert(tasks);
    ClayJson *task = clay_json_object_get(tasks, "items");
    assert(clay_json_object_get(clay_json_object_get(task, "properties"), "description"));
    assert(clay_json_object_get(clay_json_object_get(task, "properties"), "prompt"));
    assert(clay_json_array_count(clay_json_object_get(schema, "required")) == 1);
    clay_json_free(schema);

    clay_array_free(&commands.tasks);
    clay_array_free(&commands.plan.todos);
    clay_array_free(&commands.mcp_servers);
    printf("subagent tests passed\n");
    return 0;
}
