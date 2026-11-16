#include "clay/array.h"
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
static int seq_mode;
static int seq_n;
static long seq_status[3];
static char seq_auth[3][256]; /* Authorization header seen by each sequenced request */
static const char *seq_body[3];
static void free_models(ClayArray *models) {
  for (size_t i = 0; i < models->count; i++) free(*(char **)clay_array_get(models, i));
  clay_array_free(models);
}
static int test_http(const ClayHttpRequest *request, ClayHttpResponse *response) {
  memset(response, 0, sizeof(*response));
  if (seq_mode) {
    if (seq_n >= (int)(sizeof(seq_status) / sizeof(seq_status[0]))) {
      response->status = 500; /* unexpected extra request: fail the assertion, not UB */
      response->body = strdup("{}");
      return 0;
    }
    seq_auth[seq_n][0] = 0;
    for (size_t i = 0; i < request->header_count; i++)
      if (!strcmp(request->headers[i].name, "Authorization"))
        snprintf(seq_auth[seq_n], sizeof(seq_auth[0]), "%s", request->headers[i].value);
    response->status = seq_status[seq_n];
    response->body = strdup(seq_body[seq_n++]);
    return 0;
  }
  response->status = status;
  if (reply) response->body = strdup(reply);
  return transport_failure ? -1 : 0;
}
#define clay_http_request test_http
#include "../src/providers/openai.c"
#define strdup test_strdup
#include "../src/providers/grok.c"
#undef strdup
#undef clay_http_request

int main(void) {
  for (int scenario = 0; scenario < 6; scenario++) {
    ClayGrokCredentials initial = {"old-access", "old-refresh", "old-id", 1};
    ClayGrok *client = clay_grok_create(&initial, "model");
    assert(client);
    status = scenario == 1 || scenario == 2 || scenario == 3 ? 400 :
             scenario == 4 ? 503 : 200;
    transport_failure = scenario == 0;
    reply = scenario == 1 ? "{\"error\":\"invalid_grant\"}" :
            scenario == 2 ? "{\"error\":\"invalid_client\"}" :
            scenario == 3 || scenario == 5 ? "{" :
            "{\"access_token\":\"new-access\",\"refresh_token\":\"new-refresh\",\"id_token\":\"new-id\",\"expires_in\":3600}";
    fail_dup = -1;
    int rc = refresh(client);
    fail_dup = -1;
    assert(rc != 0);
    assert(client->authentication_invalid == (scenario == 1));
    assert(strcmp(client->credentials.access_token, "old-access") == 0);
    assert(strcmp(client->credentials.refresh_token, "old-refresh") == 0);
    assert(strcmp(client->credentials.id_token, "old-id") == 0);
    assert(client->credentials.expires_at == 1);
    clay_grok_destroy(client);
  }
  ClayGrokCredentials initial = {"old-access", "old-refresh", "old-id", 1};
  ClayGrok *client = clay_grok_create(&initial, "model");
  assert(client);
  status = 200; transport_failure = 0;
  reply = "{\"access_token\":\"new-access\",\"refresh_token\":\"new-refresh\",\"expires_in\":3600}";
  fail_dup = 0;
  assert(refresh(client) == 0);
  fail_dup = -1;
  assert(!client->authentication_invalid);
  assert(strcmp(client->credentials.access_token, "new-access") == 0);
  assert(strcmp(client->credentials.refresh_token, "new-refresh") == 0);
  assert(strcmp(client->credentials.id_token, "old-id") == 0);
  assert(client->credentials.expires_at > 1);
  clay_grok_destroy(client);
  /* Models 401 -> one refresh -> retry with the refreshed token. */
  ClayGrokCredentials fresh = {"old-access", "old-refresh", "old-id", (long long)time(NULL) + 100000};
  client = clay_grok_create(&fresh, "model");
  assert(client);
  seq_mode = 1; seq_n = 0;
  seq_status[0] = 401; seq_body[0] = "{}";
  seq_status[1] = 200; seq_body[1] = "{\"access_token\":\"new-access\",\"refresh_token\":\"new-refresh\",\"expires_in\":3600}";
  seq_status[2] = 200; seq_body[2] = "{\"data\":[{\"id\":\"grok-test\"}]}";
  ClayArray models;
  clay_array_init(&models, sizeof(char *));
  assert(clay_grok_list_models(client, &models) == 0);
  assert(seq_n == 3);
  assert(models.count == 1);
  assert(strcmp(client->credentials.access_token, "new-access") == 0);
  free_models(&models);
  clay_array_init(&models, sizeof(char *));
  clay_grok_destroy(client);
  /* Models 200 on the first try: no refresh, one request. */
  ClayGrokCredentials ok_creds = {"old-access", "old-refresh", "old-id", (long long)time(NULL) + 100000};
  ClayGrok *ok_client = clay_grok_create(&ok_creds, "model");
  assert(ok_client);
  seq_mode = 1; seq_n = 0;
  seq_status[0] = 200; seq_body[0] = "{\"data\":[{\"id\":\"grok-test\"}]}";
  assert(clay_grok_list_models(ok_client, &models) == 0);
  assert(seq_n == 1 && models.count == 1);
  assert(strcmp(ok_client->credentials.access_token, "old-access") == 0);
  free_models(&models);
  clay_grok_destroy(ok_client);
  /* Models 403 -> refresh -> retry succeeds. */
  client = clay_grok_create(&fresh, "model");
  assert(client);
  clay_array_init(&models, sizeof(char *));
  seq_n = 0;
  seq_status[0] = 403; seq_body[0] = "{}";
  seq_status[1] = 200; seq_body[1] = "{\"access_token\":\"new-access\",\"refresh_token\":\"new-refresh\",\"expires_in\":3600}";
  seq_status[2] = 200; seq_body[2] = "{\"data\":[{\"id\":\"grok-test\"}]}";
  assert(clay_grok_list_models(client, &models) == 0);
  assert(seq_n == 3 && models.count == 1);
  assert(!strcmp(seq_auth[0], "Bearer old-access") && !strcmp(seq_auth[2], "Bearer new-access"));
  free_models(&models);
  clay_grok_destroy(client);
  /* Models 401 -> refresh fails (invalid_grant) -> no retry, error returned. */
  client = clay_grok_create(&fresh, "model");
  assert(client);
  clay_array_init(&models, sizeof(char *));
  seq_n = 0;
  seq_status[0] = 401; seq_body[0] = "{}";
  seq_status[1] = 400; seq_body[1] = "{\"error\":\"invalid_grant\"}";
  assert(clay_grok_list_models(client, &models) != 0);
  assert(seq_n == 2 && models.count == 0);
  free_models(&models);
  clay_grok_destroy(client);
  /* Models 401 -> refresh ok -> models 401 again: exactly one refresh, no third retry. */
  client = clay_grok_create(&fresh, "model");
  assert(client);
  clay_array_init(&models, sizeof(char *));
  seq_n = 0;
  seq_status[0] = 401; seq_body[0] = "{}";
  seq_status[1] = 200; seq_body[1] = "{\"access_token\":\"new-access\",\"refresh_token\":\"new-refresh\",\"expires_in\":3600}";
  seq_status[2] = 401; seq_body[2] = "{}";
  assert(clay_grok_list_models(client, &models) != 0);
  assert(seq_n == 3 && models.count == 0);
  free_models(&models);
  clay_grok_destroy(client);
  /* Stale OpenAI key forces a refresh even when the token is not near expiry. */
  client = clay_grok_create(&fresh, "model");
  assert(client);
  client->openai_key_stale = 1;
  seq_n = 0;
  seq_status[0] = 200; seq_body[0] = "{\"access_token\":\"new-access\",\"refresh_token\":\"new-refresh\",\"expires_in\":3600}";
  assert(ensure_access_token(client) == 0);
  assert(seq_n == 1 && !client->openai_key_stale);
  clay_grok_destroy(client);
  seq_mode = 0;
  puts("grok refresh tests passed");
  return 0;
}
