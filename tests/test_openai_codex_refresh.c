#include "clay/http.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static int fail_dup = -1;
static char *test_strdup(const char *s) {
  if (fail_dup == 0) return NULL;
  if (fail_dup > 0) fail_dup--;
  return strdup(s);
}
static long status;
static int transport_failure;
static const char *reply;
static int expect_model_failure;
static int observed;
static int test_http(const ClayHttpRequest *request, ClayHttpResponse *response) {
  memset(response, 0, sizeof(*response));
  if (expect_model_failure && !strcmp(request->method, "GET")) {
    assert(observed == 1);
    response->status = 503;
    return 0;
  }
  response->status = status;
  if (reply) response->body = strdup(reply);
  return transport_failure ? -1 : 0;
}
#define strdup test_strdup
#define clay_http_request test_http
#include "../src/providers/openai_codex.c"
#undef strdup
#undef clay_http_request

static int observe_refresh(void *userdata, ClayOpenAICodex *client) {
  ClayCodexCredentials *saved = userdata;
  assert(clay_openai_codex_copy_credentials(client, saved) == 0);
  observed++;
  return 0;
}

static int reject_refresh(void *userdata, ClayOpenAICodex *client) {
  (void)userdata;
  assert(!strcmp(client->credentials.refresh_token, "new-refresh"));
  return -1;
}

typedef struct {
  int begins;
  int ends;
  int end_result;
} RefreshCoordination;

static int coordinated_begin(void *userdata, ClayOpenAICodex *client,
                             int (*should_abort)(void *), void *abort_userdata) {
  RefreshCoordination *coordination = userdata;
  (void)client; (void)should_abort; (void)abort_userdata;
  coordination->begins++;
  return 0;
}

static void coordinated_end(void *userdata, int result) {
  RefreshCoordination *coordination = userdata;
  coordination->ends++;
  coordination->end_result = result;
}

int main(void) {
  ClayJson *string = clay_json_string("owned");
  char *owned = clay_json_take_string(string);
  assert(owned && !strcmp(owned, "owned"));
  assert(clay_json_type(string) == CLAY_JSON_NULL);
  assert(!clay_json_take_string(string));
  free(owned);
  clay_json_free(string);
  for (int scenario = 0; scenario < 6; scenario++) {
    ClayCodexCredentials initial = {"old-access", "old-refresh", "old-id", "account", 1};
    ClayOpenAICodex *client = clay_openai_codex_create(&initial, "model");
    assert(client);
    ClayCodexCredentials failed_save = {0};
    clay_openai_codex_set_refresh_observer(client, observe_refresh, &failed_save);
    status = scenario == 1 || scenario == 2 || scenario == 3 ? 400 :
             scenario == 4 ? 503 : 200;
    transport_failure = scenario == 0;
    reply = scenario == 1 ? "{\"error\":\"invalid_grant\"}" :
            scenario == 2 ? "{\"error\":\"invalid_client\"}" :
            scenario == 3 || scenario == 5 ? "{" :
            "{\"access_token\":\"new-access\",\"refresh_token\":\"new-refresh\",\"id_token\":\"new-id\",\"expires_in\":3600}";
    fail_dup = scenario >= 6 && scenario <= 8 ? scenario - 6 : -1;
    int rc = refresh(client);
    fail_dup = -1;
    assert(rc != 0);
    assert(observed == 0 && !failed_save.access_token);
    assert(client->authentication_invalid == (scenario == 1));
    assert(strcmp(client->credentials.access_token, "old-access") == 0);
    assert(strcmp(client->credentials.refresh_token, "old-refresh") == 0);
    assert(strcmp(client->credentials.id_token, "old-id") == 0);
    assert(client->credentials.expires_at == 1);
    clay_openai_codex_destroy(client);
  }
  ClayCodexCredentials initial = {"old-access", "old-refresh", "old-id", "account", 1};
  ClayOpenAICodex *client = clay_openai_codex_create(&initial, "model");
  assert(client);
  status = 200; transport_failure = 0;
  reply = "{\"access_token\":\"new-access\",\"refresh_token\":\"new-refresh\",\"id_token\":\"new-id\",\"expires_in\":3600}";
  fail_dup = 0;
  assert(refresh(client) == 0);
  fail_dup = -1;
  assert(!client->authentication_invalid);
  assert(strcmp(client->credentials.access_token, "new-access") == 0);
  assert(strcmp(client->credentials.refresh_token, "new-refresh") == 0);
  assert(strcmp(client->credentials.id_token, "new-id") == 0);
  assert(client->credentials.expires_at > 1);
  clay_openai_codex_destroy(client);
  client = clay_openai_codex_create(&initial, "another-model");
  assert(client);
  ClayCodexCredentials saved = {0};
  clay_openai_codex_set_refresh_observer(client, observe_refresh, &saved);
  expect_model_failure = 1;
  ClayArray models;
  assert(clay_openai_codex_list_models(client, &models) != 0);
  assert(observed == 1);
  assert(!strcmp(saved.refresh_token, "new-refresh"));
  clay_openai_codex_credentials_free(&saved);
  clay_openai_codex_destroy(client);
  client = clay_openai_codex_create(&initial, "model");
  assert(client);
  clay_openai_codex_set_refresh_observer(client, reject_refresh, NULL);
  assert(refresh(client) != 0);
  assert(client->refresh_failed && !client->authentication_invalid);
  assert(!strcmp(client->credentials.refresh_token, "new-refresh"));
  clay_openai_codex_destroy(client);

  initial.access_token = "old-access";
  initial.refresh_token = "old-refresh";
  initial.expires_at = 1;
  client = clay_openai_codex_create(&initial, "model");
  assert(client);
  status = 400; transport_failure = 0;
  reply = "{\"error\":\"invalid_grant\"}";
  RefreshCoordination coordination = {0};
  clay_openai_codex_set_refresh_coordination(client, coordinated_begin,
                                            coordinated_end, &coordination);
  assert(refresh(client) != 0);
  assert(coordination.begins == 1 && coordination.ends == 1);
  assert(coordination.end_result != 0);
  assert(client->authentication_invalid);
  assert(!strcmp(client->credentials.refresh_token, "old-refresh"));
  clay_openai_codex_destroy(client);

  puts("openai_codex refresh tests passed");
  return 0;
}
