#ifndef CLAY_PROVIDERS_OPENAI_CODEX_H
#define CLAY_PROVIDERS_OPENAI_CODEX_H

#include "clay/array.h"
#include "clay/json.h"
#include "clay/providers/openai.h"
#include "clay/str.h"

typedef struct ClayOpenAICodex ClayOpenAICodex;

/* ChatGPT/Codex subscription OAuth credentials. They are intentionally
   separate from an OpenAI Platform API key. */
typedef struct {
  char *access_token;
  char *refresh_token;
  char *id_token;
  char *account_id;
  long long expires_at;
} ClayCodexCredentials;

char *clay_openai_codex_authorization_url(const char *challenge,
                                          const char *state);
int clay_openai_codex_extract_account_id(const char *id_token,
                                         char **account_id_out);

/* Starts browser OAuth and returns a fully populated credential set. The
   error text is safe to show (it never includes secrets). */
int clay_openai_codex_authenticate(ClayCodexCredentials *out, ClayStr *error);
void clay_openai_codex_credentials_free(ClayCodexCredentials *credentials);

ClayOpenAICodex *
clay_openai_codex_create(const ClayCodexCredentials *credentials,
                         const char *model);
void clay_openai_codex_destroy(ClayOpenAICodex *client);
/* Called synchronously after a successful token rotation. */
void clay_openai_codex_set_refresh_observer(ClayOpenAICodex *client,
    int (*observer)(void *, ClayOpenAICodex *), void *userdata);
/* begin returns 0 for ownership, 1 for adopted credentials, 2 for cancellation. */
void clay_openai_codex_set_refresh_coordination(ClayOpenAICodex *client,
    int (*begin)(void *, ClayOpenAICodex *, int (*)(void *), void *),
    void (*end)(void *, int), void *userdata);
/* Exchange owned credential strings without allocating. */
void clay_openai_codex_swap_credentials(ClayOpenAICodex *client,
                                         ClayCodexCredentials *credentials);
/* NULL omits reasoning from Responses requests and uses the model default. */
void clay_openai_codex_set_reasoning_effort(ClayOpenAICodex *client,
                                            const char *effort);
/* Stable per-chat key used by the Responses prompt cache. */
void clay_openai_codex_set_prompt_cache_key(ClayOpenAICodex *client,
                                            const char *key);
int clay_openai_codex_list_models(ClayOpenAICodex *client, ClayArray *models);
long clay_openai_codex_last_status(const ClayOpenAICodex *client);
/* Caller saves a copied result to its existing credential store after this
   call, which persists refresh-token rotation. */
int clay_openai_codex_copy_credentials(const ClayOpenAICodex *client,
                                        ClayCodexCredentials *out);
int clay_openai_codex_authentication_invalid(const ClayOpenAICodex *client);
int clay_openai_codex_run(ClayOpenAICodex *client, ClayJson *messages,
                          const ClayTool *tools, size_t tool_count,
                          int max_rounds, const ClayOpenAICallbacks *callbacks);

#endif
