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
static int test_http(const ClayHttpRequest *request, ClayHttpResponse *response) {
  (void)request;
  memset(response, 0, sizeof(*response));
  response->status = status;
  if (reply) response->body = strdup(reply);
  return transport_failure ? -1 : 0;
}
#define strdup test_strdup
#define clay_http_request test_http
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
  puts("grok refresh tests passed");
  return 0;
}
