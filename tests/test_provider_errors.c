#include "clay/providers/openai.h"
#include "clay/providers/openai_codex.h"
#include "clay/providers/grok.h"
#include "clay/providers/retry.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t fake_time;
static int cancel_wait, cancel_request, succeed_after, too_long, no_header, elapsed_per_request;
int64_t __wrap_clay_provider_retry_now_ms(void *u) { (void)u; return fake_time; }
void __wrap_clay_provider_retry_sleep_ms(unsigned ms, void *u) { (void)u; fake_time += ms; }
unsigned __wrap_clay_provider_retry_jitter(void) { return 500; }
static int abort_wait(void *u) { (void)u; return cancel_wait && fake_time >= 200; }
static long status;
static const char *body;
static int calls, legacy, detailed, refresh_first, sse_mode, tokens, tool_calls;
static const char *event;
static int transport_after_sse, executed, two_rounds, codex_rounds, order, refresh_fails;
static ClayJson *execute_tool(const ClayJson *args, void *userdata) {
  (void)args; (void)userdata; assert(order == 1); order = 2; executed++; return clay_json_object();
}
static ClayOpenAIErrorKind expected;
int __wrap_clay_http_request(const ClayHttpRequest *req, ClayHttpResponse *r) {
  memset(r, 0, sizeof(*r)); calls++; fake_time += elapsed_per_request;
  if (cancel_request) { assert(req->should_abort(req->abort_userdata)); return -1; }
  if (refresh_first && calls == 3 && strstr(req->url, "cli-chat-proxy.grok.com")) {
    assert(req->header_count >= 2 && strstr(req->headers[0].value, "new-secret"));
  }
  if (succeed_after && calls == succeed_after) {
    r->status = 200;
    const char *wire = "data: {\"choices\":[{\"delta\":{\"content\":\"before\"}}]}\n\n";
    req->on_chunk(wire, strlen(wire), req->userdata);
    return 0;
  }
  if (two_rounds) {
    if (calls == 1) {
      const char *wire = codex_rounds
          ? "data: {\"type\":\"response.output_item.added\",\"item\":{\"type\":\"function_call\",\"id\":\"item\",\"call_id\":\"x\",\"name\":\"test\",\"arguments\":\"{}\"}}\n\n"
          : "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"x\",\"function\":{\"name\":\"test\",\"arguments\":\"{}\"}}]}}]}\n\n";
      r->status = 200;
      req->on_chunk(wire, strlen(wire), req->userdata);
    } else {
      assert(strstr(req->body, codex_rounds ? "function_call_output" : "tool_call_id"));
      r->status = status;
      r->error_body = strdup(body); r->error_body_len = strlen(body);
      r->rate_header_count = 1; r->rate_headers[0].name = "retry-after";
      strcpy(r->rate_headers[0].value, too_long ? "31" : "7");
      req->on_chunk(body, strlen(body), req->userdata);
    }
    return 0;
  }
  if (refresh_first && calls == 2) {
    assert(strstr(req->url, "oauth/token") || strstr(req->url, "oauth2/token")); r->status = 200;
    if (refresh_fails) { r->status = 500; r->body = strdup("oauth failed"); r->body_len = strlen(r->body); return 0; }
    r->body = strdup("{\"access_token\":\"new-secret\",\"expires_in\":3600}");
    r->body_len = strlen(r->body); return 0;
  }
  if (sse_mode) {
    r->status = 200;
    for (size_t i = 0; i < strlen(body); i++)
      if (req->on_chunk(body + i, 1, req->userdata)) return -1;
    return transport_after_sse ? -1 : 0;
  }
  r->status = refresh_first && calls == 1 ? 401 : status;
  r->error_body = strdup(body); r->error_body_len = strlen(body);
  r->rate_header_count = no_header ? 0 : 1; r->rate_headers[0].name = "retry-after";
  strcpy(r->rate_headers[0].value, too_long ? "31" : "7");
  if (req->on_chunk) req->on_chunk(body, strlen(body), req->userdata);
  return 0;
}
static void on_old(long s, const char *b, void *u) {
  (void)u; assert(s == status); assert(!strcmp(b, sse_mode ? event : body)); legacy++;
}
static void on_detail(const ClayOpenAIError *e, void *u) {
  (void)u; assert(e->status == status && e->kind == expected);
  assert(e->from_sse == sse_mode);
  if (sse_mode) {
    assert(e->body_len == strlen(event) && !memcmp(e->body, event, e->body_len));
    assert(!e->retry_after_valid && !e->rate_header_count);
    detailed++; return;
  }
  assert(e->body_len == strlen(body) && !memcmp(e->body, body, e->body_len));
  assert(!e->body_truncated && e->retry_after_valid && e->retry_after_seconds == 7);
  assert(e->rate_header_count == 1 && !strcmp(e->rate_headers[0].value, "7"));
  detailed++;
}
static void on_partial(const ClayOpenAIError *e, void *u) {
  (void)u;
  assert(e->kind == CLAY_OPENAI_ERROR_RATE_LIMIT && e->status == 429);
  assert(e->body_truncated && !e->retry_after_valid && e->rate_headers_dropped);
  assert(e->rate_header_count == 1 && !strcmp(e->rate_headers[0].value, "invalid"));
  detailed++;
}
static void on_reasoning(const char *text, void *u) { (void)text; (void)u; tokens++; }
static void on_token(const char *text, void *u) { (void)u; assert(!strcmp(text, "before")); tokens++; }
static void on_tool(const char *name, const char *args, void *u) { (void)name; (void)args; (void)u; assert(order == 0); order = 1; tool_calls++; }
static void on_result(const char *name, const ClayJson *result, void *u) { (void)name; (void)result; (void)u; assert(order == 2); order = 3; }
static void reset(long s, const char *b, ClayOpenAIErrorKind k) {
  fake_time=0; order=0; refresh_fails=0; cancel_request=cancel_wait=succeed_after=too_long=no_header=elapsed_per_request=0;
  status=s; body=b; expected=k; calls=legacy=detailed=refresh_first=sse_mode=tokens=tool_calls=transport_after_sse=executed=two_rounds=codex_rounds=0;
}
static void checked(void) { assert(calls == 1 && legacy == 1 && detailed == 1); }
int main(void) {
  ClayOpenAIRetryState boundary = {0};
  assert(clay_openai_retry_safe(&boundary));
  boundary.visible = 1; assert(!clay_openai_retry_safe(&boundary));
  boundary.visible = 0; boundary.tool_executed = 1;
  assert(!clay_openai_retry_safe(&boundary));
  boundary.tool_executed = 0; boundary.callback_delivered = 1;
  assert(!clay_openai_retry_safe(&boundary));
  ClayOpenAICallbacks cb = {0}; cb.on_error=on_old; cb.on_http_error=on_detail;
  ClayHttpResponse partial = {0};
  partial.status = 429;
  partial.error_body = "{\"error\":{\"code\":\"insufficient_quota\"}}";
  partial.error_body_len = strlen(partial.error_body);
  partial.error_body_truncated = 1;
  partial.rate_headers_dropped = 1;
  partial.rate_header_count = 1;
  partial.rate_headers[0].name = "retry-after";
  strcpy(partial.rate_headers[0].value, "invalid");
  cb.on_http_error = on_partial;
  clay_openai_report_http_error(&partial, &cb);
  assert(detailed == 1);
  cb.on_http_error = on_detail;
  ClayJson *messages=clay_json_array();
  ClayOpenAI *client=clay_openai_create("http://localhost/v1", "secret", "model");
  assert(client);
  struct {long status; const char *body; ClayOpenAIErrorKind kind;} cases[] = {
    {429, "{\"error\":{\"code\":\"rate_limit_exceeded\"}}", CLAY_OPENAI_ERROR_RATE_LIMIT},
    {429, "{\"error\":{\"type\":\"insufficient_quota\"}}", CLAY_OPENAI_ERROR_QUOTA},
    {403, "{\"error\":{\"code\":\"billing_hard_limit_reached\"}}", CLAY_OPENAI_ERROR_QUOTA},
    {500, "{\"error\":{\"code\":\"insufficient_quota\"}}", CLAY_OPENAI_ERROR_OTHER},
    {401, "unauthorized", CLAY_OPENAI_ERROR_OTHER},
    {429, "not json", CLAY_OPENAI_ERROR_RATE_LIMIT},
    {429, "{\"error\":{\"code\":\"insufficient_quota\"}} trailing", CLAY_OPENAI_ERROR_RATE_LIMIT}
  };
  for (size_t i=0; i<sizeof(cases)/sizeof(*cases); i++) {
    reset(cases[i].status,cases[i].body,cases[i].kind);
    assert(clay_openai_run(client,messages,NULL,0,1,&cb)==-1);
    assert(calls == (status == 429 && expected != CLAY_OPENAI_ERROR_QUOTA ? 3 : 1));
    assert(legacy == 1 && detailed == 1);
    assert(clay_openai_last_status(client)==status);
  }
  cb.on_token = on_token; cb.on_tool_call = on_tool; cb.on_tool_result = on_result;
  ClayTool tool = {.name="test", .fn=execute_tool};
  const char *wire = "data: {\"choices\":[{\"delta\":{\"content\":\"before\",\"tool_calls\":[{\"index\":0,\"id\":\"x\",\"function\":{\"name\":\"test\",\"arguments\":\"{}\"}}]}}]}\n\n"
                     "data: {\"error\":\n"
                     "data: {\"code\":429,\"message\":\"slow down\"}}\n\n";
  reset(200, wire, CLAY_OPENAI_ERROR_RATE_LIMIT); sse_mode=1;
  event="{\"error\":\n{\"code\":429,\"message\":\"slow down\"}}";
  assert(clay_openai_run(client,messages,&tool,1,1,&cb)==-1); checked();
  assert(!executed);
  assert(tokens==1 && tool_calls==0 && clay_openai_last_status(client)==200);
  ClayJson *partial_reply=clay_json_array_get(messages,clay_json_array_count(messages)-1);
  assert(!strcmp(clay_json_string_value(clay_json_object_get(partial_reply,"content")),"before"));
  assert(clay_json_array_count(clay_json_object_get(partial_reply,"tool_calls"))==0);
  reset(200, wire, CLAY_OPENAI_ERROR_RATE_LIMIT); sse_mode=1; transport_after_sse=1;
  event="{\"error\":\n{\"code\":429,\"message\":\"slow down\"}}";
  assert(clay_openai_run(client,messages,&tool,1,1,&cb)==-1); checked();
  assert(!executed && !tool_calls);
  ClayJson *j=clay_json_parse("{\"error\":{\"message\":\"429\"}}",NULL);
  assert(!clay_openai_sse_error_is_429(j)); clay_json_free(j);
  j=clay_json_parse("{\"choices\":[{\"delta\":{\"content\":\"429\"}}]}",NULL);
  assert(!clay_openai_sse_error_is_429(j)); clay_json_free(j);
  reset(200, "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"x\",\"function\":{\"name\":\"test\",\"arguments\":\"{}\"}}]}}]}\n\ndata: {\"error\":{\"message\":\"bad\"}}\n\ndata: {\"error\":{\"code\":429}}\n\n", CLAY_OPENAI_ERROR_OTHER);
  sse_mode=1; event="{\"error\":{\"message\":\"bad\"}}"; cb.on_http_error=NULL;
  assert(clay_openai_run(client,messages,&tool,1,1,&cb)==-1);
  assert(calls==1 && legacy==1 && !executed && !tool_calls);
  cb.on_http_error=on_detail;
  size_t before_openai_fragments = clay_json_array_count(messages);
  reset(200, "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\"{\"}}]}}]}\n\ndata: {\"error\":{\"code\":429}}\n\n", CLAY_OPENAI_ERROR_RATE_LIMIT);
  sse_mode=1; event="{\"error\":{\"code\":429}}";
  assert(clay_openai_run(client,messages,&tool,1,1,&cb)==-1);
  assert(calls==3 && legacy==1 && detailed==1);
  assert(!executed && !tool_calls && clay_openai_last_run_replay_safe(client));
  assert(clay_json_array_count(messages)==before_openai_fragments);
  reset(200, "data: {\"choices\":[{\"delta\":{\"reasoning\":\"hidden\"}}]}\n\ndata: {\"error\":{\"code\":429}}\n\n", CLAY_OPENAI_ERROR_RATE_LIMIT);
  sse_mode=1; event="{\"error\":{\"code\":429}}";
  assert(clay_openai_run(client,messages,&tool,1,1,&cb)==-1); checked();
  assert(!clay_openai_last_run_replay_safe(client));
  reset(429, "limited", CLAY_OPENAI_ERROR_RATE_LIMIT); two_rounds=1;
  size_t old_count = clay_json_array_count(messages);
  assert(clay_openai_run(client,messages,&tool,1,2,&cb)==-1);
  assert(calls==4 && executed==1 && tool_calls==1 && order==3 && legacy==1 && detailed==1);
  assert(clay_json_array_count(messages)==old_count+2);
  assert(!clay_openai_last_run_replay_safe(client));
  reset(429, "limited", CLAY_OPENAI_ERROR_RATE_LIMIT); succeed_after=2;
  assert(clay_openai_run(client,messages,NULL,0,1,&cb)==0);
  assert(calls==2 && fake_time==7000 && !legacy && !detailed);
  reset(429, "limited", CLAY_OPENAI_ERROR_RATE_LIMIT); cancel_wait=cancel_request=1; elapsed_per_request=200; cb.should_abort=abort_wait;
  assert(clay_openai_run(client,messages,NULL,0,1,&cb)==1);
  assert(calls==1 && !legacy && !detailed); cb.should_abort=NULL;
  reset(429, "limited", CLAY_OPENAI_ERROR_RATE_LIMIT); cancel_wait=1; cb.should_abort=abort_wait;
  assert(clay_openai_run(client,messages,NULL,0,1,&cb)==1);
  assert(calls==1 && fake_time==200 && !legacy && !detailed); cb.should_abort=NULL;
  reset(429, "limited", CLAY_OPENAI_ERROR_RATE_LIMIT); too_long=1; cb.on_http_error=NULL; cb.on_error=NULL;
  assert(clay_openai_run(client,messages,NULL,0,1,&cb)==-1);
  assert(calls==1 && fake_time==0 && clay_openai_last_status(client)==429);
  reset(429, "{\"error\":{\"code\":\"insufficient_quota\"}}", CLAY_OPENAI_ERROR_QUOTA);
  assert(clay_openai_run(client,messages,NULL,0,1,&cb)==-1);
  assert(calls==1 && fake_time==0);
  cb.on_http_error=on_detail; cb.on_error=on_old;
  reset(200, "data: {\"error\":{\"status\":429,\"type\":\"insufficient_quota\"}}\n\n", CLAY_OPENAI_ERROR_QUOTA);
  sse_mode=1; event="{\"error\":{\"status\":429,\"type\":\"insufficient_quota\"}}";
  assert(clay_openai_run(client,messages,NULL,0,1,&cb)==-1); checked();
  reset(429, "limited", CLAY_OPENAI_ERROR_RATE_LIMIT); no_header=1; succeed_after=3;
  assert(clay_openai_run(client,messages,NULL,0,1,&cb)==0);
  assert(calls==3 && fake_time==4000 && !legacy && !detailed);
  reset(429, "limited", CLAY_OPENAI_ERROR_RATE_LIMIT); elapsed_per_request=54000;
  assert(clay_openai_run(client,messages,NULL,0,1,&cb)==-1);
  assert(calls==1 && fake_time==54000 && legacy==1 && detailed==1);
  reset(200, "data: {\"error\":{\"code\":429}}\n\n", CLAY_OPENAI_ERROR_RATE_LIMIT);
  sse_mode=1; event="{\"error\":{\"code\":429}}"; succeed_after=2;
  assert(clay_openai_run(client,messages,NULL,0,1,&cb)==0);
  assert(calls==2 && fake_time==1500 && !legacy && !detailed);
  reset(200, "data: {\"choices\":[{\"delta\":{\"content\":\"before\"}}]}\n\n", CLAY_OPENAI_ERROR_OTHER); sse_mode=1;
  assert(clay_openai_run(client,messages,NULL,0,1,&cb)==0);
  assert(calls==1 && tokens==1 && !legacy && !detailed);
  clay_openai_destroy(client);
  ClayCodexCredentials cc = {.access_token="secret", .refresh_token="refresh-secret", .account_id="account"};
  ClayOpenAICodex *codex=clay_openai_codex_create(&cc,"model"); assert(codex);
  reset(429,"{\"error\":{\"code\":\"insufficient_quota\"}}",CLAY_OPENAI_ERROR_QUOTA);
  assert(clay_openai_codex_run(codex,messages,NULL,0,1,&cb)==-1); checked();
  assert(clay_openai_codex_last_status(codex)==429);
  reset(429,"limited",CLAY_OPENAI_ERROR_RATE_LIMIT); refresh_first=1;
  assert(clay_openai_codex_run(codex,messages,NULL,0,1,&cb)==-1);
  assert(calls==5 && detailed==1 && legacy==1 && clay_openai_codex_last_status(codex)==429);
  reset(200, "data: {\"type\":\"response.output_text.delta\",\"delta\":\"before\"}\n\ndata: {\"type\":\"response.failed\",\n"
             "data: \"response\":{\"error\":{\"status\":429,\"message\":\"slow\"}}}\n\n", CLAY_OPENAI_ERROR_RATE_LIMIT);
  sse_mode=1; event="{\"type\":\"response.failed\",\n\"response\":{\"error\":{\"status\":429,\"message\":\"slow\"}}}";
  assert(clay_openai_codex_run(codex,messages,NULL,0,1,&cb)==-1); checked();
  assert(tokens==1 && !tool_calls && clay_openai_codex_last_status(codex)==200);
  partial_reply=clay_json_array_get(messages,clay_json_array_count(messages)-1);
  assert(!strcmp(clay_json_string_value(clay_json_object_get(partial_reply,"content")),"before"));
  reset(200, "data: {\"type\":\"response.failed\",\"response\":{\"error\":{\"message\":\"429\"}}}\n\n", CLAY_OPENAI_ERROR_OTHER);
  sse_mode=1; event=body;
  /* Ordinary Codex failures retain the old raw-stream error path. */
  cb.on_http_error=NULL;
  assert(clay_openai_codex_run(codex,messages,NULL,0,1,&cb)==-1);
  assert(calls==1 && legacy==1 && detailed==0);
  cb.on_http_error=on_detail;
  reset(200, "data: {\"type\":\"error\",\"error\":{\"message\":\"bad\"}}\n\ndata: {\"type\":\"error\",\"error\":{\"code\":429}}\n\n", CLAY_OPENAI_ERROR_OTHER);
  sse_mode=1; event=body; cb.on_http_error=NULL;
  assert(clay_openai_codex_run(codex,messages,&tool,1,1,&cb)==-1);
  assert(calls==1 && legacy==1 && !executed && !tool_calls);
  cb.on_http_error=on_detail;
  reset(200, "data: {\"type\":\"response.reasoning_summary_text.delta\",\"delta\":\"thinking\"}\n\ndata: {\"type\":\"error\",\"error\":{\"code\":429}}\n\n", CLAY_OPENAI_ERROR_RATE_LIMIT);
  sse_mode=1; event="{\"type\":\"error\",\"error\":{\"code\":429}}";
  assert(clay_openai_codex_run(codex,messages,&tool,1,1,&cb)==-1); checked();
  assert(!tokens && !tool_calls && !executed);
  reset(200, "data: {\"type\":\"response.reasoning_summary_text.delta\",\"delta\":\"\"}\n\ndata: {\"type\":\"error\",\"error\":{\"code\":429}}\n\n", CLAY_OPENAI_ERROR_RATE_LIMIT);
  sse_mode=1; event="{\"type\":\"error\",\"error\":{\"code\":429}}"; cb.on_reasoning=on_reasoning;
  assert(clay_openai_codex_run(codex,messages,&tool,1,1,&cb)==-1); checked();
  assert(tokens==1); cb.on_reasoning=NULL;
  size_t before_fragments = clay_json_array_count(messages);
  reset(200, "data: {\"type\":\"response.output_item.added\",\"item\":{\"type\":\"function_call\",\"id\":\"item-x\",\"call_id\":\"x\",\"name\":\"test\",\"arguments\":\"{}\"}}\n\ndata: {\"type\":\"error\",\"error\":{\"code\":429}}\n\n", CLAY_OPENAI_ERROR_RATE_LIMIT);
  sse_mode=1; event="{\"type\":\"error\",\"error\":{\"code\":429}}";
  assert(clay_openai_codex_run(codex,messages,&tool,1,1,&cb)==-1);
  assert(calls==3 && legacy==1 && detailed==1);
  assert(!executed && !tool_calls);
  assert(clay_json_array_count(messages)==before_fragments);
  reset(429, "limited", CLAY_OPENAI_ERROR_RATE_LIMIT); two_rounds=codex_rounds=1;
  old_count = clay_json_array_count(messages);
  assert(clay_openai_codex_run(codex,messages,&tool,1,2,&cb)==-1);
  assert(calls==4 && executed==1 && tool_calls==1 && order==3 && legacy==1 && detailed==1);
  assert(clay_json_array_count(messages)==old_count+2);
  reset(429, "limited", CLAY_OPENAI_ERROR_RATE_LIMIT); no_header=1; succeed_after=3;
  assert(clay_openai_codex_run(codex,messages,NULL,0,1,&cb)==0);
  assert(calls==3 && fake_time==4000 && !legacy && !detailed);
  reset(429, "limited", CLAY_OPENAI_ERROR_RATE_LIMIT); succeed_after=2;
  assert(clay_openai_codex_run(codex,messages,NULL,0,1,&cb)==0);
  assert(calls==2 && fake_time==7000 && !legacy && !detailed);
  reset(429, "limited", CLAY_OPENAI_ERROR_RATE_LIMIT); cancel_wait=cancel_request=1; elapsed_per_request=200; cb.should_abort=abort_wait;
  assert(clay_openai_codex_run(codex,messages,NULL,0,1,&cb)==1);
  assert(calls==1 && !legacy && !detailed); cb.should_abort=NULL;
  reset(429, "limited", CLAY_OPENAI_ERROR_RATE_LIMIT); cancel_wait=1; cb.should_abort=abort_wait;
  assert(clay_openai_codex_run(codex,messages,NULL,0,1,&cb)==1);
  assert(calls==1 && fake_time==200 && !legacy && !detailed); cb.should_abort=NULL;
  reset(429, "limited", CLAY_OPENAI_ERROR_RATE_LIMIT); elapsed_per_request=54000;
  assert(clay_openai_codex_run(codex,messages,NULL,0,1,&cb)==-1);
  assert(calls==1 && fake_time==54000 && legacy==1 && detailed==1);
  reset(429, "limited", CLAY_OPENAI_ERROR_RATE_LIMIT); too_long=1; cb.on_http_error=NULL; cb.on_error=NULL;
  assert(clay_openai_codex_run(codex,messages,NULL,0,1,&cb)==-1);
  assert(calls==1 && fake_time==0 && clay_openai_codex_last_status(codex)==429);
  cb.on_http_error=on_detail; cb.on_error=on_old;
  reset(200, "data: {\"type\":\"error\",\"error\":{\"status\":429,\"type\":\"insufficient_quota\"}}\n\n", CLAY_OPENAI_ERROR_QUOTA);
  sse_mode=1; event="{\"type\":\"error\",\"error\":{\"status\":429,\"type\":\"insufficient_quota\"}}";
  assert(clay_openai_codex_run(codex,messages,NULL,0,1,&cb)==-1); checked();
  reset(200, "data: {\"type\":\"error\",\"error\":{\"code\":429}}\n\n", CLAY_OPENAI_ERROR_RATE_LIMIT);
  sse_mode=1; event="{\"type\":\"error\",\"error\":{\"code\":429}}"; succeed_after=2;
  assert(clay_openai_codex_run(codex,messages,NULL,0,1,&cb)==0);
  assert(calls==2 && fake_time==1500 && !legacy && !detailed);
  reset(200, "data: {\"type\":\"response.output_text.delta\",\"delta\":\"before\"}\n\n", CLAY_OPENAI_ERROR_OTHER); sse_mode=1;
  assert(clay_openai_codex_run(codex,messages,NULL,0,1,&cb)==0);
  assert(calls==1 && tokens==1 && !legacy && !detailed);
  reset(500, "server failed", CLAY_OPENAI_ERROR_OTHER);
  assert(clay_openai_codex_run(codex,messages,NULL,0,1,&cb)==-1); checked();
  clay_openai_codex_destroy(codex);
  codex=clay_openai_codex_create(&cc,"model"); assert(codex);
  reset(401, "unauthorized", CLAY_OPENAI_ERROR_OTHER); refresh_first=refresh_fails=1;
  assert(clay_openai_codex_run(codex,messages,NULL,0,1,&cb)==-1);
  assert(calls==2 && legacy==1 && detailed==1);
  clay_openai_codex_destroy(codex);
  ClayGrokCredentials gc={.access_token="secret",.refresh_token="refresh-secret",.expires_at=(long long)time(NULL)+3600};
  ClayGrok *grok=clay_grok_create(&gc,"model"); assert(grok);
  reset(429,"limited",CLAY_OPENAI_ERROR_RATE_LIMIT);
  assert(clay_grok_run(grok,messages,NULL,0,1,&cb)==-1);
  assert(calls==3 && legacy==1 && detailed==1);
  reset(429, "limited", CLAY_OPENAI_ERROR_RATE_LIMIT); two_rounds=1;
  old_count=clay_json_array_count(messages);
  assert(clay_grok_run(grok,messages,&tool,1,2,&cb)==-1);
  assert(calls==4 && executed==1 && tool_calls==1 && order==3 && legacy==1 && detailed==1);
  assert(clay_json_array_count(messages)==old_count+2);
  reset(200, wire, CLAY_OPENAI_ERROR_RATE_LIMIT); sse_mode=1;
  event="{\"error\":\n{\"code\":429,\"message\":\"slow down\"}}";
  assert(clay_grok_run(grok,messages,&tool,1,1,&cb)==-1); checked();
  assert(tokens==1 && !tool_calls && !executed);
  reset(200, "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"thinking\"}}]}\n\ndata: {\"error\":{\"code\":429}}\n\n", CLAY_OPENAI_ERROR_RATE_LIMIT);
  sse_mode=1; event="{\"error\":{\"code\":429}}"; cb.on_reasoning=on_reasoning;
  assert(clay_grok_run(grok,messages,NULL,0,1,&cb)==-1); checked();
  assert(tokens==1); cb.on_reasoning=NULL;
  reset(200, "data: {\"choices\":[{\"delta\":{\"content\":\"before\"}}]}\n\n", CLAY_OPENAI_ERROR_OTHER); sse_mode=1;
  assert(clay_grok_run(grok,messages,NULL,0,1,&cb)==0);
  assert(calls==1 && tokens==1 && !legacy && !detailed);
  reset(500, "server failed", CLAY_OPENAI_ERROR_OTHER);
  assert(clay_grok_run(grok,messages,NULL,0,1,&cb)==-1); checked();
  reset(401, "unauthorized", CLAY_OPENAI_ERROR_OTHER); two_rounds=1;
  assert(clay_grok_run(grok,messages,&tool,1,2,&cb)==-1);
  assert(calls==2 && executed==1 && legacy==1 && detailed==1);
  reset(401, "unauthorized", CLAY_OPENAI_ERROR_OTHER); refresh_first=1;
  assert(clay_grok_run(grok,messages,NULL,0,1,&cb)==-1);
  assert(calls==3 && legacy==1 && detailed==1);
  reset(401, "unauthorized", CLAY_OPENAI_ERROR_OTHER); refresh_first=1; succeed_after=3;
  assert(clay_grok_run(grok,messages,NULL,0,1,&cb)==0);
  assert(calls==3 && tokens==1 && !legacy && !detailed);
  reset(500, "server failed", CLAY_OPENAI_ERROR_OTHER); refresh_first=1;
  assert(clay_grok_run(grok,messages,NULL,0,1,&cb)==-1);
  assert(calls==3 && legacy==1 && detailed==1);
  reset(401, "unauthorized", CLAY_OPENAI_ERROR_OTHER); refresh_first=refresh_fails=1;
  assert(clay_grok_run(grok,messages,NULL,0,1,&cb)==-1);
  assert(calls==2 && legacy==1 && detailed==1);
  clay_grok_destroy(grok); clay_json_free(messages);
  puts("provider error tests passed"); return 0;
}
