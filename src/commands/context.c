#include "context.h"
#include "../providers/scope.h"

#include "clay/config.h"
#include "clay/storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <time.h>

#define CLAY_SYSTEM_PROMPT_BASE                                                \
  "You are Clay, a coding agent working in the user's terminal. You " \
  "have a real shell, direct file tools, and the user's actual project " \
  "in front of you. Work like a software architect: understand the " \
  "system before changing it, keep its design coherent, and leave the " \
  "code better structured than you found it." \
  "\n\n" \
  "# Working principles" \
  "\n\n" \
  "Do what the user asked, at the scope they asked. Their instructions " \
  "outrank every convention below. If an instruction looks wrong, say " \
  "so in one sentence, then follow it." \
  "\n\n" \
  "Stand on evidence, never on speculation. Read the file, run the " \
  "command, print the value. Never describe code you have not opened, " \
  "and never call a fix working when you have not run it. If you cannot " \
  "verify something, say what you do not know and what would settle it." \
  "\n\n" \
  "Diagnose before editing. When something fails, read the error, " \
  "follow the call path, and confirm the real cause. Changing lines " \
  "until the symptom disappears is not a fix, and neither is silencing " \
  "an error, widening a catch, or skipping a test." \
  "\n\n" \
  "Do not reinvent what already exists. Look for a helper in this " \
  "project, then the standard library, then the tools already " \
  "installed. Duplicated logic drifts apart and turns into a bug. A new " \
  "dependency is a last resort." \
  "\n\n" \
  "Prefer the modern, efficient path: current APIs over deprecated " \
  "ones, the right data structure over a repeated scan, one pass over " \
  "three. Efficient means fewer moving parts, not cleverer code." \
  "\n\n" \
  "Keep the design honest. Fix causes, not symptoms. No speculative " \
  "abstraction, no compatibility shim nobody asked for, no config flag " \
  "standing in for a decision. Three plain lines beat a premature " \
  "helper." \
  "\n\n" \
  "Blend in. Match the project's language level, layout, naming, " \
  "formatting, error handling, and test style, and read the surrounding " \
  "code before adding to it." \
  "\n\n" \
  "# Verify your work" \
  "\n\n" \
  "Run what you changed. Build it, run the tests, execute the command, " \
  "exercise the path. A change you have not run is a proposal, not a " \
  "result. If it cannot be verified here (no credentials, no device, a " \
  "service you must not touch), say so and give the user the exact " \
  "command to run and what a correct result looks like. When something " \
  "fails, show the output, name any step you skipped, and never present " \
  "written code as working code." \
  "\n\n" \
  "# Ask when it matters" \
  "\n\n" \
  "Use the ask_user tool when an unknown would change what you build " \
  "and the code cannot settle it. It draws a picker in the user's " \
  "terminal and returns their answer in the same turn, so you never " \
  "have to end a turn just to ask. Its schema carries the exact shape: " \
  "a question, its options, and allow_custom." \
  "\n\n" \
  "Ask one question per call, in the user's terms, with two to four " \
  "concrete options ordered most likely first; the user can always type " \
  "an answer of their own. If their answer stays vague, ask a narrowing " \
  "follow-up. Do not use it for permission to continue, for anything " \
  "the code already answers, or for routine judgment calls that are " \
  "yours to make: state the assumption and move on." \
  "\n\n" \
  "# Planning and delegation" \
  "\n\n" \
  "Most work needs no ceremony: read what you need, make the change, " \
  "verify it, say what you did. Reach for a plan only when the job has " \
  "several real parts, and for a subagent only when one of those parts " \
  "is big enough to be worth briefing someone else on." \
  "\n\n" \
  "When a job is that big, write the plan first with todowrite, in " \
  "steps a person could tick off, then work through them one at a time, " \
  "keeping exactly one step in_progress and marking it completed before " \
  "starting the next. That checklist is the plan the user reads: do not " \
  "spell it out in prose as well, do not print a tree of files you are " \
  "about to create, and do not announce each step before taking it. " \
  "Write the plan, do the work, then say what came of it. Skip it for a " \
  "single quick action." \
  "\n\n" \
  "Branches of the work that do not depend on each other go out " \
  "together, in one subagent call: it takes a list of tasks, starts " \
  "them all at once, and returns when the last one is done. Each runs " \
  "on a fresh agent with your tools but none of this conversation, so " \
  "its prompt has to carry everything it needs: what to build, any " \
  "contract it must honour, and what its summary should answer. Each " \
  "plans its own work; you do not plan for it, and \"launch the " \
  "subagent\" is not a step in your plan - delegating is how you do a " \
  "step, not a step of its own. Give every branch its own files, since " \
  "two of them editing the same file will fight. Settle whatever they " \
  "all depend on (a shared contract, an interface, a schema) before you " \
  "fan them out, and feed the summaries that come back into whatever " \
  "follows." \
  "\n\n" \
  "Delegate the self-contained parts (a module, a test suite, a survey " \
  "of unfamiliar code) and keep the rest: anything that turns on the " \
  "user's own words, or on judgment about the job as a whole. If a " \
  "subagent comes back empty or wrong, do that step yourself instead of " \
  "retrying it blindly." \
  "\n\n" \
  "# Comments and writing" \
  "\n\n" \
  "Comments state what the code does or the constraint it obeys, never " \
  "how you reasoned, what you tried first, or which request produced " \
  "the change. Default to no comment; add one only where the code alone " \
  "would mislead, such as a protocol quirk, an ordering requirement, a " \
  "unit, or a lock held across a call. One line, not three. Delete " \
  "stale comments you pass by, since a wrong comment costs more than no " \
  "comment." \
  "\n\n" \
  "Write English like an engineer explaining something to another " \
  "engineer: short sentences, concrete nouns, active voice, no " \
  "marketing and no filler. That holds for documentation, commit " \
  "messages, and your replies alike. Accurate beats impressive, so say " \
  "what it does and what it does not." \
  "\n\n" \
  "# Git" \
  "\n\n" \
  "Only change git state (commit, push, branch, reset) when the user " \
  "asks for it. Commit messages follow Conventional Commits: \"type: " \
  "description\", where type is one of feat, fix, chore, docs, refactor, " \
  "test, perf, build, ci, with an optional scope such as \"fix(http): " \
  "...\". Keep the description lowercase, imperative, under about 72 " \
  "characters, with no trailing period, and describe the effect rather " \
  "than the files. Add a body only when it carries something the " \
  "description cannot. Check git log first and follow the repository's " \
  "own convention where it differs." \
  "\n\n" \
  "# Tools" \
  "\n\n" \
  "read, write, edit, glob and grep are scoped to the workspace: use " \
  "them instead of shell_exec's cat, sed, find and grep. edit needs an " \
  "exact, unique match. Reach for shell_exec for everything else: " \
  "builds, tests, git, other programs." \
  "\n\n" \
  "shell_exec runs in /workspace, the sandbox alias of the project " \
  "directory; do not try to discover its host path or inspect mounts. " \
  "/scratch is this conversation's private directory under /tmp, for " \
  "temporary files. Sandboxed commands cannot reach host paths outside " \
  "the configured read-only mounts, and their network reaches only each " \
  "other: a server one command starts is reachable from your next " \
  "command over localhost, and from nothing on the user's machine. " \
  "Normal POSIX syntax works (variables, command substitution, globs, " \
  "redirections) but may need an execution confirmation. Keep commands " \
  "focused and summarize what came back." \
  "\n\n" \
  "shell_exec kills a command after 120 seconds and hands you what it " \
  "printed by then, so a stuck command never stalls the session; raise " \
  "timeout_seconds for a slow build or test run." \
  "\n\n" \
  "A command that only ends when someone stops it belongs in task_run, " \
  "not shell_exec: it starts in the background and returns as soon as " \
  "it has had a moment to fail. task_output shows what a task has " \
  "printed and whether it is still alive, task_stop ends it, and " \
  "task_list shows everything running. The usual shape is task_run the " \
  "server, shell_exec a curl against it, read task_output if the curl " \
  "looks wrong, then task_stop. Stop what you started before your turn " \
  "ends; nothing else will." \
  "\n\n" \
  "repo_map ranks the workspace's top-level definitions; use it to " \
  "orient in unfamiliar code before opening files one by one." \
  "\n\n" \
  "The mcp tool reaches the servers the user configured with /mcp: list " \
  "their tools and arguments, then call one." \
  "\n\n" \
  "The user can put you in Plan mode with /plan to discuss an approach " \
  "before anything changes: write and edit are refused there and " \
  "mutating shell commands are blocked. A blocked tool result means you " \
  "explain the plan in words instead of retrying the call." \
  "\n\n" \
  "If the user set up /autotest, a write or edit result carrying " \
  "auto_test_failed means the configured command failed after that " \
  "change. Read auto_test_output and fix it before moving on." \
  "\n\n" \
  "# Memory" \
  "\n\n" \
  "Long-term memory (memory_save, memory_read) persists across every " \
  "future chat: save a decision, a fix, or a preference the user stated " \
  "once significant work is done. The index of existing entries is " \
  "below, and memory_read loads one by its slug. Short-term memory " \
  "(remember) is this chat's scratchpad, replayed every turn even after " \
  "older messages are dropped: pin details you will still need many " \
  "turns from now." \
  "\n\n" \
  "# Replies" \
  "\n\n" \
  "Be brief and concrete. Say what you did, what you verified, and what " \
  "is left. Skip preambles, restated instructions, and narration of " \
  "your own process, and show a command with its real output instead of " \
  "describing it. Never claim more confidence than your evidence " \
  "supports."


/* Reused as-is by a chat-less session started within this long of the
   cached text being built, so the provider's prefix cache stays warm;
   rebuilt with current memory/skill data once it expires. The window runs
   from when it was built, never extended by use, so a stale prompt cannot
   live forever. Never applies to an existing chat - see
   clay_chat_system_prompt. */
#define CLAY_SYSTEM_PROMPT_TTL_SECONDS 600

static const ClayProviderType PROVIDER_TYPES[] = {
    {"openai", "OpenAI", "https://api.openai.com/v1"},
    {"openai-codex", "OpenAI Codex", NULL},
    {"grok", "Grok", CLAY_GROK_API_URL},
    {"openrouter", "OpenRouter", "https://openrouter.ai/api/v1"},
    {"groq", "Groq", "https://api.groq.com/openai/v1"},
    {"deepseek", "DeepSeek", "https://api.deepseek.com"},
    {"mistral", "Mistral", "https://api.mistral.ai/v1"},
    {"together", "Together AI", "https://api.together.xyz/v1"},
    {"custom", "OpenAI Custom", NULL},
};

static const ClayReasoningEffort REASONING_EFFORTS[] = {
    {NULL, "Default",
     "Let the selected model use its default reasoning level."},
    {"minimal", "Minimal", "Use the fewest reasoning tokens when supported."},
    {"low", "Low", "Favor lower latency and token use when supported."},
    {"medium", "Medium", "Balance reasoning depth, latency, and token use."},
    {"high", "High", "Favor deeper reasoning when supported."},
    {"xhigh", "XHigh", "Use very deep reasoning when supported."},
    {"max", "Max", "Request maximum reasoning when supported."},
};

static const char *env_value(const char *first, const char *second) {
  const char *value = first ? getenv(first) : NULL;
  if (value && *value) return value;
  value = second ? getenv(second) : NULL;
  return value && *value ? value : NULL;
}

static void replace_string(char **field, const char *value) {
  if (!value) return;
  free(*field);
  *field = strdup(value);
}

static void uppercase_provider_id(const char *id, char *out, size_t out_size) {
  size_t i = 0;
  for (; id[i] && i + 1 < out_size; i++) {
    char ch = id[i];
    out[i] = (char)(ch == '-' ? '_' : toupper((unsigned char)ch));
  }
  out[i] = '\0';
}

/* Environment credentials are deliberately an in-memory override: they make
   CI and ephemeral shells usable without copying secrets into ~/.clay. */
static int apply_environment_credentials(const ClayProviderType *type,
                                         ClayProviderConfig **config_ptr) {
  const char *api_key = NULL;
  const char *base_url = NULL;
  const char *access_token = NULL;
  const char *refresh_token = NULL;
  const char *id_token = NULL;
  const char *account_id = NULL;
  const char *expires_at = NULL;
  char provider_name[64];
  char api_name[80];
  char base_name[80];

  uppercase_provider_id(type->id, provider_name, sizeof(provider_name));
  snprintf(api_name, sizeof(api_name), "CLAY_%s_API_KEY", provider_name);
  snprintf(base_name, sizeof(base_name), "CLAY_%s_BASE_URL", provider_name);

  if (strcmp(type->id, "openai") == 0) {
    api_key = env_value("OPENAI_API_KEY", api_name);
    base_url = env_value("OPENAI_BASE_URL", base_name);
  } else if (strcmp(type->id, "openrouter") == 0) {
    api_key = env_value("OPENROUTER_API_KEY", api_name);
    base_url = env_value("OPENROUTER_BASE_URL", base_name);
  } else if (strcmp(type->id, "groq") == 0) {
    api_key = env_value("GROQ_API_KEY", api_name);
    base_url = env_value("GROQ_BASE_URL", base_name);
  } else if (strcmp(type->id, "deepseek") == 0) {
    api_key = env_value("DEEPSEEK_API_KEY", api_name);
    base_url = env_value("DEEPSEEK_BASE_URL", base_name);
  } else if (strcmp(type->id, "mistral") == 0) {
    api_key = env_value("MISTRAL_API_KEY", api_name);
    base_url = env_value("MISTRAL_BASE_URL", base_name);
  } else if (strcmp(type->id, "together") == 0) {
    api_key = env_value("TOGETHER_API_KEY", api_name);
    base_url = env_value("TOGETHER_BASE_URL", base_name);
  } else if (strcmp(type->id, "grok") == 0) {
    api_key = env_value("XAI_API_KEY", "GROK_API_KEY");
    if (!api_key) api_key = getenv(api_name);
    base_url = env_value("GROK_BASE_URL", base_name);
    access_token = env_value("GROK_ACCESS_TOKEN", "CLAY_GROK_ACCESS_TOKEN");
    refresh_token = env_value("GROK_REFRESH_TOKEN", "CLAY_GROK_REFRESH_TOKEN");
    id_token = env_value("GROK_ID_TOKEN", "CLAY_GROK_ID_TOKEN");
    account_id = env_value("GROK_ACCOUNT_ID", "CLAY_GROK_ACCOUNT_ID");
    expires_at = env_value("GROK_EXPIRES_AT", "CLAY_GROK_EXPIRES_AT");
  } else if (strcmp(type->id, "openai-codex") == 0) {
    access_token = env_value("OPENAI_CODEX_ACCESS_TOKEN",
                             "CLAY_CODEX_ACCESS_TOKEN");
    refresh_token = env_value("OPENAI_CODEX_REFRESH_TOKEN",
                              "CLAY_CODEX_REFRESH_TOKEN");
    id_token = env_value("OPENAI_CODEX_ID_TOKEN", "CLAY_CODEX_ID_TOKEN");
    account_id = env_value("OPENAI_CODEX_ACCOUNT_ID",
                           "CLAY_CODEX_ACCOUNT_ID");
    expires_at = env_value("OPENAI_CODEX_EXPIRES_AT",
                           "CLAY_CODEX_EXPIRES_AT");
  } else if (strcmp(type->id, "custom") == 0) {
    api_key = env_value("CLAY_API_KEY", "CUSTOM_API_KEY");
    base_url = env_value("CLAY_BASE_URL", "CUSTOM_BASE_URL");
  } else {
    api_key = getenv(api_name);
    base_url = getenv(base_name);
  }

  int has_override = api_key || base_url || access_token || refresh_token ||
                     id_token || account_id || expires_at;
  if (!has_override) return 0;

  ClayProviderConfig *config = *config_ptr;
  if (!config) {
    config = calloc(1, sizeof(*config));
    if (!config) return -1;
    config->id = strdup(type->id);
    if (type->default_base_url) config->base_url = strdup(type->default_base_url);
    *config_ptr = config;
  }

  if (api_key) replace_string(&config->apikey, api_key);
  if (base_url) replace_string(&config->base_url, base_url);
  if (strcmp(type->id, "grok") == 0 && api_key) {
    replace_string(&config->base_url, base_url ? base_url : CLAY_GROK_API_URL);
    replace_string(&config->auth_mode, "api_key");
  }
  if (access_token) replace_string(&config->access_token, access_token);
  if (refresh_token) replace_string(&config->refresh_token, refresh_token);
  if (id_token) replace_string(&config->id_token, id_token);
  if (account_id) replace_string(&config->account_id, account_id);
  if (access_token || refresh_token) {
    if (strcmp(type->id, "grok") == 0) {
      replace_string(&config->auth_mode, "subscription");
      replace_string(&config->base_url, CLAY_GROK_SUBSCRIPTION_URL);
    }
  }
  if (expires_at) {
    char *end = NULL;
    long long value = strtoll(expires_at, &end, 10);
    if (end && *end == '\0' && value > 0) config->expires_at = value;
  }
  if (strcmp(type->id, "openai-codex") == 0 && !config->base_url)
    config->base_url = strdup("");
  return 1;
}

static const char *environment_provider(void) {
  const char *provider = env_value("CLAY_PROVIDER", NULL);
  if (provider) return provider;
  if (env_value("OPENAI_CODEX_ACCESS_TOKEN", "CLAY_CODEX_ACCESS_TOKEN"))
    return "openai-codex";
  if (env_value("OPENROUTER_API_KEY", "CLAY_OPENROUTER_API_KEY"))
    return "openrouter";
  if (env_value("GROQ_API_KEY", "CLAY_GROQ_API_KEY")) return "groq";
  if (env_value("DEEPSEEK_API_KEY", "CLAY_DEEPSEEK_API_KEY"))
    return "deepseek";
  if (env_value("MISTRAL_API_KEY", "CLAY_MISTRAL_API_KEY"))
    return "mistral";
  if (env_value("TOGETHER_API_KEY", "CLAY_TOGETHER_API_KEY"))
    return "together";
  if (env_value("XAI_API_KEY", "GROK_API_KEY")) return "grok";
  if (env_value("OPENAI_API_KEY", "CLAY_OPENAI_API_KEY")) return "openai";
  if (env_value("CLAY_API_KEY", "CUSTOM_API_KEY")) return "custom";
  return NULL;
}

static const char *environment_model(const char *provider) {
  const char *model = env_value("CLAY_MODEL", NULL);
  if (model) return model;
  if (strcmp(provider, "openai") == 0) return getenv("OPENAI_MODEL");
  if (strcmp(provider, "openrouter") == 0) return getenv("OPENROUTER_MODEL");
  if (strcmp(provider, "groq") == 0) return getenv("GROQ_MODEL");
  if (strcmp(provider, "deepseek") == 0) return getenv("DEEPSEEK_MODEL");
  if (strcmp(provider, "mistral") == 0) return getenv("MISTRAL_MODEL");
  if (strcmp(provider, "together") == 0) return getenv("TOGETHER_MODEL");
  if (strcmp(provider, "grok") == 0) return env_value("GROK_MODEL", "XAI_MODEL");
  if (strcmp(provider, "openai-codex") == 0)
    return env_value("OPENAI_CODEX_MODEL", "CODEX_MODEL");
  if (strcmp(provider, "custom") == 0) return getenv("CUSTOM_MODEL");
  return NULL;
}

static void provider_models_free(ClayConnectedProvider *provider) {
  for (size_t i = 0; i < provider->models.count; i++)
    free(*(char **)clay_array_get(&provider->models, i));
  clay_array_free(&provider->models);
}

static void provider_free(ClayConnectedProvider *provider) {
  clay_openai_destroy(provider->client);
  clay_openai_codex_destroy(provider->codex_client);
  clay_grok_destroy(provider->grok_client);
  clay_config_free(provider->config);
  provider_models_free(provider);
}

ClayConnectedProvider *clay_commands_find_provider(ClayCommands *commands,
                                                   const char *id) {
  for (size_t i = 0; i < commands->providers.count; i++) {
    ClayConnectedProvider *provider = clay_array_get(&commands->providers, i);
    if (strcmp(provider->type->id, id) == 0)
      return provider;
  }
  return NULL;
}

uint64_t clay_commands_provider_generation(ClayCommands *commands, const char *id) {
  ClayConnectedProvider *provider = id ? clay_commands_find_provider(commands, id) : NULL;
  return provider ? provider->generation : 0;
}

const ClayProviderType *clay_commands_find_provider_type(const char *id) {
  for (size_t i = 0; i < sizeof(PROVIDER_TYPES) / sizeof(PROVIDER_TYPES[0]);
       i++) {
    if (strcmp(PROVIDER_TYPES[i].id, id) == 0)
      return &PROVIDER_TYPES[i];
  }
  return NULL;
}

const ClayProviderType *clay_commands_provider_types(size_t *count) {
  *count = sizeof(PROVIDER_TYPES) / sizeof(PROVIDER_TYPES[0]);
  return PROVIDER_TYPES;
}

/* Provider state uses a short lock; the gate serializes replacement with active completions. */
static pthread_mutex_t codex_credentials_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t credential_save_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t codex_refresh_cond = PTHREAD_COND_INITIALIZER;
static pthread_cond_t grok_refresh_cond = PTHREAD_COND_INITIALIZER;

#define UNPERSISTED_RETRY_SECONDS 5

static ClayProviderConfig *config_clone(const ClayProviderConfig *config) {
  if (!config) return NULL;
  ClayProviderConfig *copy = calloc(1, sizeof(*copy));
  if (!copy) return NULL;
  copy->id = config->id ? strdup(config->id) : NULL;
  copy->apikey = config->apikey ? strdup(config->apikey) : NULL;
  copy->base_url = config->base_url ? strdup(config->base_url) : NULL;
  copy->auth_mode = config->auth_mode ? strdup(config->auth_mode) : NULL;
  copy->access_token = config->access_token ? strdup(config->access_token) : NULL;
  copy->refresh_token = config->refresh_token ? strdup(config->refresh_token) : NULL;
  copy->id_token = config->id_token ? strdup(config->id_token) : NULL;
  copy->account_id = config->account_id ? strdup(config->account_id) : NULL;
  copy->expires_at = config->expires_at;
  copy->tpm = config->tpm;
  if ((config->id && !copy->id) || (config->apikey && !copy->apikey) ||
      (config->base_url && !copy->base_url) ||
      (config->auth_mode && !copy->auth_mode) ||
      (config->access_token && !copy->access_token) ||
      (config->refresh_token && !copy->refresh_token) ||
      (config->id_token && !copy->id_token) ||
      (config->account_id && !copy->account_id)) {
    clay_config_free(copy);
    return NULL;
  }
  return copy;
}

/* Caller holds codex_credentials_lock. Releases it for the disk write and
   returns with it held again. The save lock keeps older snapshots from
   landing after newer ones. */
static int persist_config(ClayConnectedProvider *provider,
                          ClayProviderConfig *snapshot, int codex,
                          uint64_t version, int *flag, time_t *next_try) {
  pthread_mutex_unlock(&codex_credentials_lock);
  pthread_mutex_lock(&credential_save_lock);
  pthread_mutex_lock(&codex_credentials_lock);
  uint64_t current = codex ? provider->codex_version : provider->grok_version;
  int write = current == version;
  int rc = 0;
  if (write) {
    pthread_mutex_unlock(&codex_credentials_lock);
    rc = clay_config_save(snapshot);
    pthread_mutex_lock(&codex_credentials_lock);
  }
  pthread_mutex_unlock(&credential_save_lock);
  current = codex ? provider->codex_version : provider->grok_version;
  if (current != version) {
    *flag = 0;
    *next_try = 0;
  } else if (rc != 0) {
    *flag = 1;
    *next_try = time(NULL) + UNPERSISTED_RETRY_SECONDS;
  } else {
    *flag = 0;
    *next_try = 0;
  }
  return rc;
}

static int codex_flight_usable(const ClayConnectedProvider *provider) {
  return provider->codex_version != UINT64_MAX &&
         provider->codex_refresh_epoch != UINT64_MAX;
}

static int grok_flight_usable(const ClayConnectedProvider *provider) {
  return provider->grok_version != UINT64_MAX &&
         provider->grok_refresh_epoch != UINT64_MAX;
}

static void provider_use_begin(ClayCommands *commands) {
  pthread_mutex_lock(&commands->provider_gate_lock);
  while (commands->provider_gate_writer)
    pthread_cond_wait(&commands->provider_gate_cond,
                      &commands->provider_gate_lock);
  commands->provider_gate_readers++;
  pthread_mutex_unlock(&commands->provider_gate_lock);
}

static void provider_use_end(ClayCommands *commands) {
  pthread_mutex_lock(&commands->provider_gate_lock);
  if (--commands->provider_gate_readers == 0)
    pthread_cond_broadcast(&commands->provider_gate_cond);
  pthread_mutex_unlock(&commands->provider_gate_lock);
}

static void provider_replace_begin(ClayCommands *commands) {
  pthread_mutex_lock(&commands->provider_gate_lock);
  while (commands->provider_gate_writer || commands->provider_gate_readers)
    pthread_cond_wait(&commands->provider_gate_cond,
                      &commands->provider_gate_lock);
  commands->provider_gate_writer = 1;
  pthread_mutex_unlock(&commands->provider_gate_lock);
}

static void provider_replace_end(ClayCommands *commands) {
  pthread_mutex_lock(&commands->provider_gate_lock);
  commands->provider_gate_writer = 0;
  pthread_cond_broadcast(&commands->provider_gate_cond);
  pthread_mutex_unlock(&commands->provider_gate_lock);
}

void clay_commands_load_provider(ClayCommands *commands,
                                 const ClayProviderType *type) {
  provider_replace_begin(commands);
  ClayProviderConfig *config = clay_config_load(type->id);
  int environment_override = apply_environment_credentials(type, &config);
  if (environment_override < 0) {
    clay_config_free(config);
    provider_replace_end(commands);
    return;
  }
  if (!config) {
    provider_replace_end(commands);
    return;
  }
  int is_codex = strcmp(type->id, "openai-codex") == 0;
  int is_grok_subscription = strcmp(type->id, "grok") == 0 &&
                             config->auth_mode &&
                             strcmp(config->auth_mode, "subscription") == 0;
  int is_grok_api_key = strcmp(type->id, "grok") == 0 &&
                        !is_grok_subscription;
  if ((!is_codex && !is_grok_subscription &&
       (!clay_openai_url_is_supported(config->base_url) || !config->apikey ||
        !*config->apikey)) ||
      (is_grok_api_key && strcmp(config->base_url, CLAY_GROK_API_URL) != 0) ||
      (is_codex && (!config->access_token || !config->refresh_token ||
                    !config->account_id)) ||
      (is_grok_subscription &&
       (!config->access_token || !config->refresh_token))) {
    clay_config_free(config);
    provider_replace_end(commands);
    return;
  }

  ClayOpenAI *client = (is_codex || is_grok_subscription)
                           ? NULL
                           : clay_openai_create(config->base_url, config->apikey,
                                                NULL);
  ClayCodexCredentials credentials = {config->access_token,
                                      config->refresh_token, config->id_token,
                                      config->account_id, config->expires_at};
  ClayOpenAICodex *codex_client =
      is_codex ? clay_openai_codex_create(&credentials, NULL) : NULL;
  ClayGrokCredentials grok_credentials = {config->access_token,
                                          config->refresh_token,
                                          config->id_token,
                                          config->expires_at};
  ClayGrok *grok_client = is_grok_subscription
                              ? clay_grok_create(&grok_credentials, NULL)
                              : NULL;

  pthread_mutex_lock(&codex_credentials_lock);
  ClayConnectedProvider *existing =
      clay_commands_find_provider(commands, type->id);
  if (commands->provider_generation == UINT64_MAX ||
      (existing && (!codex_flight_usable(existing) ||
                    !grok_flight_usable(existing)))) {
    pthread_mutex_unlock(&codex_credentials_lock);
    clay_openai_destroy(client);
    clay_openai_codex_destroy(codex_client);
    clay_grok_destroy(grok_client);
    clay_config_free(config);
    provider_replace_end(commands);
    return;
  }
  uint64_t generation = ++commands->provider_generation;
  if (existing) {
    ClayOpenAI *old_client = existing->client;
    ClayOpenAICodex *old_codex_client = existing->codex_client;
    ClayGrok *old_grok_client = existing->grok_client;
    ClayProviderConfig *old_config = existing->config;
    provider_models_free(existing);
    existing->config = config;
    existing->commands = commands;
    existing->generation = generation;
    existing->codex_version++;
    existing->grok_version++;
    existing->codex_refresh_epoch++;
    existing->grok_refresh_epoch++;
    existing->codex_refresh_failure_epoch = 0;
    existing->grok_refresh_failure_epoch = 0;
    existing->codex_refresh_active = 0;
    existing->grok_refresh_active = 0;
    existing->codex_refresh_start_version = existing->codex_version;
    existing->grok_refresh_start_version = existing->grok_version;
    existing->codex_refresh_unpersisted = 0;
    existing->codex_auth_invalid = 0;
    existing->grok_refresh_unpersisted = 0;
    existing->codex_retry_at = existing->grok_retry_at = 0;
    existing->environment_override = environment_override;
    existing->client = client;
    existing->codex_client = codex_client;
    existing->grok_client = grok_client;
    clay_array_init(&existing->models, sizeof(char *));
    existing->models_fetched = 0;
    existing->models_rc = 0;
    pthread_cond_broadcast(&codex_refresh_cond);
    pthread_cond_broadcast(&grok_refresh_cond);
    pthread_mutex_unlock(&codex_credentials_lock);
    clay_openai_destroy(old_client);
    clay_openai_codex_destroy(old_codex_client);
    clay_grok_destroy(old_grok_client);
    clay_config_free(old_config);
    provider_replace_end(commands);
    return;
  }

  ClayConnectedProvider provider = {0};
  provider.type = type;
  provider.commands = commands;
  provider.generation = generation;
  provider.config = config;
  provider.environment_override = environment_override;
  provider.client = client;
  provider.codex_client = codex_client;
  provider.grok_client = grok_client;
  clay_array_init(&provider.models, sizeof(char *));
  ClayConnectedProvider *slot = clay_array_push(&commands->providers);
  if (slot)
    *slot = provider;
  else
    provider_free(&provider);
  pthread_mutex_unlock(&codex_credentials_lock);
  provider_replace_end(commands);
}

/* Caller holds codex_credentials_lock. Environment credentials are never persisted, so the flag is simply dropped. */
static int retry_unpersisted(ClayConnectedProvider *provider, int *flag,
                             time_t *next_try, int force, int codex) {
  if (!*flag) return 0;
  if (provider->environment_override) {
    *flag = 0;
    return 0;
  }
  time_t now = time(NULL);
  if (!force && now < *next_try) return -1;
  ClayProviderConfig *snapshot = config_clone(provider->config);
  if (!snapshot) {
    *next_try = now + UNPERSISTED_RETRY_SECONDS;
    return -1;
  }
  uint64_t version = codex ? provider->codex_version : provider->grok_version;
  int rc = persist_config(provider, snapshot, codex, version, flag, next_try);
  clay_config_free(snapshot);
  return rc == 0 ? 0 : -1;
}

static int retry_codex_unpersisted(ClayConnectedProvider *provider, int force) {
  return retry_unpersisted(provider, &provider->codex_refresh_unpersisted,
                           &provider->codex_retry_at, force, 1);
}

static int retry_grok_unpersisted(ClayConnectedProvider *provider, int force) {
  return retry_unpersisted(provider, &provider->grok_refresh_unpersisted,
                           &provider->grok_retry_at, force, 0);
}

static int save_config_tracked(ClayConnectedProvider *provider, int *flag,
                               time_t *next_try, int codex) {
  if (provider->environment_override) return 0;
  ClayProviderConfig *snapshot = config_clone(provider->config);
  if (!snapshot) {
    *flag = 1;
    *next_try = time(NULL) + UNPERSISTED_RETRY_SECONDS;
    return -1;
  }
  uint64_t version = codex ? provider->codex_version : provider->grok_version;
  int rc = persist_config(provider, snapshot, codex, version, flag, next_try);
  clay_config_free(snapshot);
  return rc;
}

static int remove_config_tracked(ClayConnectedProvider *provider,
                                 const char *id, int codex,
                                 uint64_t version) {
  if (provider->environment_override) return 0;
  char *copy = id ? strdup(id) : NULL;
  if (!copy) return -1;
  pthread_mutex_unlock(&codex_credentials_lock);
  pthread_mutex_lock(&credential_save_lock);
  pthread_mutex_lock(&codex_credentials_lock);
  uint64_t current = codex ? provider->codex_version : provider->grok_version;
  int rc = 0;
  if (current != version) {
    if (codex) provider->codex_auth_invalid = 0;
  } else {
    pthread_mutex_unlock(&codex_credentials_lock);
    if (clay_config_exists(copy) && clay_config_remove(copy) != 0) rc = -1;
    pthread_mutex_lock(&codex_credentials_lock);
    current = codex ? provider->codex_version : provider->grok_version;
    if (codex && current != version) provider->codex_auth_invalid = 0;
  }
  pthread_mutex_unlock(&credential_save_lock);
  free(copy);
  return rc;
}

int clay_commands_codex_refresh_begin(ClayConnectedProvider *provider,
    uint64_t version, int (*should_abort)(void *), void *userdata) {
  if (!provider) return -1;
  pthread_mutex_lock(&codex_credentials_lock);
  if (!codex_flight_usable(provider)) {
    pthread_mutex_unlock(&codex_credentials_lock);
    return -1;
  }
  uint64_t epoch = provider->codex_refresh_epoch;
  for (;;) {
    if (provider->codex_auth_invalid) {
      pthread_mutex_unlock(&codex_credentials_lock);
      return -1;
    }
    retry_codex_unpersisted(provider, 0);
    if (provider->codex_refresh_failure_epoch > epoch ||
        provider->codex_refresh_unpersisted) {
      pthread_mutex_unlock(&codex_credentials_lock);
      return -1;
    }
    if (!provider->codex_refresh_active) {
      if (provider->codex_version != version) {
        pthread_mutex_unlock(&codex_credentials_lock);
        return 1;
      }
      if (provider->codex_refresh_epoch != epoch) {
        pthread_mutex_unlock(&codex_credentials_lock);
        return -1;
      }
      provider->codex_refresh_active = 1;
      provider->codex_refresh_start_version = version;
      pthread_mutex_unlock(&codex_credentials_lock);
      return 0;
    }
    pthread_mutex_unlock(&codex_credentials_lock);
    int cancelled = should_abort && should_abort(userdata);
    pthread_mutex_lock(&codex_credentials_lock);
    if (cancelled) {
      pthread_mutex_unlock(&codex_credentials_lock);
      return 2;
    }
    if (provider->codex_refresh_epoch != epoch || !provider->codex_refresh_active)
      continue;
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
      pthread_mutex_unlock(&codex_credentials_lock);
      return -1;
    }
    deadline.tv_nsec += 50000000;
    if (deadline.tv_nsec >= 1000000000) {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(&codex_refresh_cond, &codex_credentials_lock, &deadline);
  }
}

void clay_commands_codex_refresh_end(ClayConnectedProvider *provider, int result) {
  pthread_mutex_lock(&codex_credentials_lock);
  provider->codex_refresh_active = 0;
  if (provider->codex_refresh_epoch != UINT64_MAX)
    provider->codex_refresh_epoch++;
  if (result != 0) {
    provider->codex_refresh_failure_epoch = provider->codex_refresh_epoch;
    if (provider->codex_version != provider->codex_refresh_start_version)
      provider->codex_refresh_unpersisted = 1;
  }
  pthread_cond_broadcast(&codex_refresh_cond);
  pthread_mutex_unlock(&codex_credentials_lock);
}

static int grok_refresh_begin(ClayConnectedProvider *provider,
    uint64_t version, int (*should_abort)(void *), void *userdata) {
  if (!provider) return -1;
  pthread_mutex_lock(&codex_credentials_lock);
  if (!grok_flight_usable(provider)) {
    pthread_mutex_unlock(&codex_credentials_lock);
    return -1;
  }
  uint64_t epoch = provider->grok_refresh_epoch;
  for (;;) {
    retry_grok_unpersisted(provider, 0);
    if (provider->grok_refresh_failure_epoch > epoch ||
        provider->grok_refresh_unpersisted) {
      pthread_mutex_unlock(&codex_credentials_lock);
      return -1;
    }
    if (!provider->grok_refresh_active) {
      if (provider->grok_version != version) {
        pthread_mutex_unlock(&codex_credentials_lock);
        return 1;
      }
      if (provider->grok_refresh_epoch != epoch) {
        pthread_mutex_unlock(&codex_credentials_lock);
        return -1;
      }
      provider->grok_refresh_active = 1;
      provider->grok_refresh_start_version = version;
      pthread_mutex_unlock(&codex_credentials_lock);
      return 0;
    }
    pthread_mutex_unlock(&codex_credentials_lock);
    int cancelled = should_abort && should_abort(userdata);
    pthread_mutex_lock(&codex_credentials_lock);
    if (cancelled) {
      pthread_mutex_unlock(&codex_credentials_lock);
      return 2;
    }
    if (provider->grok_refresh_epoch != epoch || !provider->grok_refresh_active)
      continue;
    struct timespec deadline;
    if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
      pthread_mutex_unlock(&codex_credentials_lock);
      return -1;
    }
    deadline.tv_nsec += 50000000;
    if (deadline.tv_nsec >= 1000000000) {
      deadline.tv_sec++;
      deadline.tv_nsec -= 1000000000;
    }
    pthread_cond_timedwait(&grok_refresh_cond, &codex_credentials_lock, &deadline);
  }
}

static void grok_refresh_end(ClayConnectedProvider *provider, int result) {
  pthread_mutex_lock(&codex_credentials_lock);
  provider->grok_refresh_active = 0;
  if (provider->grok_refresh_epoch != UINT64_MAX)
    provider->grok_refresh_epoch++;
  if (result != 0) {
    provider->grok_refresh_failure_epoch = provider->grok_refresh_epoch;
    if (provider->grok_version != provider->grok_refresh_start_version)
      provider->grok_refresh_unpersisted = 1;
  }
  pthread_cond_broadcast(&grok_refresh_cond);
  pthread_mutex_unlock(&codex_credentials_lock);
}

ClayOpenAICodex *clay_commands_codex_snapshot(ClayConnectedProvider *provider,
                                               const char *model, uint64_t *version) {
  if (!provider || !provider->config || !version) return NULL;
  pthread_mutex_lock(&codex_credentials_lock);
  retry_codex_unpersisted(provider, 0);
  ClayProviderConfig *config = provider->config;
  ClayCodexCredentials credentials = {config->access_token, config->refresh_token,
                                       config->id_token, config->account_id,
                                       config->expires_at};
  ClayOpenAICodex *client = clay_openai_codex_create(&credentials, model);
  *version = provider->codex_version;
  pthread_mutex_unlock(&codex_credentials_lock);
  return client;
}

static int same_optional_string(const char *a, const char *b) {
  return a && b ? strcmp(a, b) == 0 : a == b;
}

ClayGrok *clay_commands_grok_snapshot(ClayConnectedProvider *provider,
                                      const char *model, uint64_t *version) {
  if (!provider || !provider->config || !version) return NULL;
  pthread_mutex_lock(&codex_credentials_lock);
  retry_grok_unpersisted(provider, 0);
  ClayProviderConfig *config = provider->config;
  ClayGrokCredentials credentials = {config->access_token, config->refresh_token,
                                     config->id_token, config->expires_at};
  ClayGrok *client = clay_grok_create(&credentials, model);
  *version = provider->grok_version;
  pthread_mutex_unlock(&codex_credentials_lock);
  return client;
}

int clay_commands_save_grok_credentials(ClayConnectedProvider *provider,
                                        const ClayGrok *client, uint64_t version) {
  if (!provider || !provider->config || !client) return -1;
  ClayGrokCredentials credentials = {0};
  int invalid = clay_grok_authentication_invalid(client);
  pthread_mutex_lock(&codex_credentials_lock);
  int rc = 0;
  ClayProviderConfig *config = provider->config;
  if (provider->grok_version != version) {
    rc = 1;
  } else if (invalid) {
    rc = -1;
  } else if (clay_grok_copy_credentials(client, &credentials) != 0) {
    rc = -1;
  } else if (!same_optional_string(config->access_token, credentials.access_token) ||
             !same_optional_string(config->refresh_token, credentials.refresh_token) ||
             !same_optional_string(config->id_token, credentials.id_token) ||
             config->expires_at != credentials.expires_at) {
    if (provider->grok_version == UINT64_MAX) {
      rc = -1;
    } else {
      free(config->access_token); free(config->refresh_token); free(config->id_token);
      config->access_token = credentials.access_token;
      config->refresh_token = credentials.refresh_token;
      config->id_token = credentials.id_token;
      config->expires_at = credentials.expires_at;
      memset(&credentials, 0, sizeof(credentials));
      provider->grok_version++;
      rc = save_config_tracked(provider, &provider->grok_refresh_unpersisted,
                               &provider->grok_retry_at, 0);
    }
  } else {
    rc = save_config_tracked(provider, &provider->grok_refresh_unpersisted,
                             &provider->grok_retry_at, 0);
  }
  pthread_mutex_unlock(&codex_credentials_lock);
  clay_grok_credentials_free(&credentials);
  return rc;
}
static int save_codex_credentials(ClayConnectedProvider *provider,
                                  const ClayOpenAICodex *client, uint64_t *version) {
  if (!provider || !provider->config || !client) return -1;
  ClayCodexCredentials credentials = {0};
  int invalid = clay_openai_codex_authentication_invalid(client);
  pthread_mutex_lock(&codex_credentials_lock);
  int rc = 0;
  ClayProviderConfig *config = provider->config;
  if (provider->codex_version != *version) {
    rc = 1;
  } else if (invalid || provider->codex_auth_invalid) {
    provider->codex_auth_invalid = 1;
    uint64_t version = provider->codex_version;
    rc = config->id ? remove_config_tracked(provider, config->id, 1, version) : 0;
    if (rc == 0) provider->codex_refresh_unpersisted = 0;
  } else if (clay_openai_codex_copy_credentials(client, &credentials) != 0) {
    rc = -1;
  } else if (!same_optional_string(config->access_token, credentials.access_token) ||
             !same_optional_string(config->refresh_token, credentials.refresh_token) ||
             !same_optional_string(config->id_token, credentials.id_token) ||
             !same_optional_string(config->account_id, credentials.account_id) ||
             config->expires_at != credentials.expires_at) {
    if (provider->codex_version == UINT64_MAX) {
      rc = -1;
    } else {
      free(config->access_token); free(config->refresh_token);
      free(config->id_token); free(config->account_id);
      config->access_token = credentials.access_token;
      config->refresh_token = credentials.refresh_token;
      config->id_token = credentials.id_token;
      config->account_id = credentials.account_id;
      config->expires_at = credentials.expires_at;
      memset(&credentials, 0, sizeof(credentials));
      provider->codex_version++;
      provider->codex_auth_invalid = 0;
      *version = provider->codex_version;
      rc = save_config_tracked(provider, &provider->codex_refresh_unpersisted,
                               &provider->codex_retry_at, 1);
    }
  } else {
    rc = save_config_tracked(provider, &provider->codex_refresh_unpersisted,
                             &provider->codex_retry_at, 1);
  }
  pthread_mutex_unlock(&codex_credentials_lock);
  clay_openai_codex_credentials_free(&credentials);
  return rc;
}

int clay_commands_save_codex_credentials(ClayConnectedProvider *provider,
                                         const ClayOpenAICodex *client, uint64_t version) {
  return save_codex_credentials(provider, client, &version);
}

typedef struct {
  ClayConnectedProvider *provider;
  uint64_t generation;
  uint64_t version;
} CodexRefreshSave;

static int begin_codex_refresh(void *userdata, ClayOpenAICodex *client,
    int (*should_abort)(void *), void *abort_userdata) {
  CodexRefreshSave *save = userdata;
  if (save->generation != save->provider->generation) return -1;
  int rc = clay_commands_codex_refresh_begin(save->provider, save->version,
                                              should_abort, abort_userdata);
  if (rc != 1) return rc;
  uint64_t version;
  ClayOpenAICodex *copy = clay_commands_codex_snapshot(save->provider, NULL, &version);
  if (!copy) return -1;
  ClayCodexCredentials credentials = {0};
  clay_openai_codex_swap_credentials(copy, &credentials);
  clay_openai_codex_swap_credentials(client, &credentials);
  clay_openai_codex_credentials_free(&credentials);
  clay_openai_codex_destroy(copy);
  save->version = version;
  return 1;
}

static void end_codex_refresh(void *userdata, int result) {
  CodexRefreshSave *save = userdata;
  if (save->generation != save->provider->generation) return;
  clay_commands_codex_refresh_end(save->provider, result);
}

static int save_codex_refresh(void *userdata, ClayOpenAICodex *client) {
  CodexRefreshSave *save = userdata;
  ClayConnectedProvider *provider = save->provider;
  pthread_mutex_lock(&codex_credentials_lock);
  ClayProviderConfig *config = provider->config;
  if (save->generation != provider->generation ||
      provider->codex_version != save->version ||
      provider->codex_version == UINT64_MAX) {
    pthread_mutex_unlock(&codex_credentials_lock);
    return -1;
  }
  ClayCodexCredentials credentials = {config->access_token, config->refresh_token,
                                      config->id_token, config->account_id,
                                      config->expires_at};
  clay_openai_codex_swap_credentials(client, &credentials);
  config->access_token = credentials.access_token;
  config->refresh_token = credentials.refresh_token;
  config->id_token = credentials.id_token;
  config->account_id = credentials.account_id;
  config->expires_at = credentials.expires_at;
  provider->codex_version++;
  int rc = save_config_tracked(provider, &provider->codex_refresh_unpersisted,
                               &provider->codex_retry_at, 1);
  ClayOpenAICodex *copy = NULL;
  if (!rc) {
    ClayCodexCredentials published = {config->access_token, config->refresh_token,
                                       config->id_token, config->account_id,
                                       config->expires_at};
    copy = clay_openai_codex_create(&published, NULL);
    if (!copy) rc = -1;
  }
  if (!rc) {
    ClayCodexCredentials old = {0};
    clay_openai_codex_swap_credentials(copy, &old);
    clay_openai_codex_swap_credentials(client, &old);
    clay_openai_codex_credentials_free(&old);
    save->version = provider->codex_version;
    provider->codex_auth_invalid = 0;
  }
  clay_openai_codex_destroy(copy);
  pthread_mutex_unlock(&codex_credentials_lock);
  return rc;
}

typedef struct {
  ClayConnectedProvider *provider;
  uint64_t generation;
  uint64_t version;
} GrokRefreshSave;

static int begin_grok_refresh(void *userdata, ClayGrok *client,
    int (*should_abort)(void *), void *abort_userdata) {
  GrokRefreshSave *save = userdata;
  if (save->generation != save->provider->generation) return -1;
  int rc = grok_refresh_begin(save->provider, save->version,
                                              should_abort, abort_userdata);
  if (rc != 1) return rc;
  uint64_t version;
  ClayGrok *copy = clay_commands_grok_snapshot(save->provider, NULL, &version);
  if (!copy) return -1;
  ClayGrokCredentials credentials = {0};
  clay_grok_swap_credentials(copy, &credentials);
  clay_grok_swap_credentials(client, &credentials);
  clay_grok_credentials_free(&credentials);
  clay_grok_destroy(copy);
  save->version = version;
  return 1;
}

static void end_grok_refresh(void *userdata, int result) {
  GrokRefreshSave *save = userdata;
  if (save->generation != save->provider->generation) return;
  grok_refresh_end(save->provider, result);
}

static int save_grok_refresh(void *userdata, ClayGrok *client) {
  GrokRefreshSave *save = userdata;
  ClayConnectedProvider *provider = save->provider;
  pthread_mutex_lock(&codex_credentials_lock);
  ClayProviderConfig *config = provider->config;
  if (save->generation != provider->generation ||
      provider->grok_version != save->version ||
      provider->grok_version == UINT64_MAX) {
    pthread_mutex_unlock(&codex_credentials_lock);
    return -1;
  }
  ClayGrokCredentials credentials = {config->access_token, config->refresh_token,
                                      config->id_token, config->expires_at};
  clay_grok_swap_credentials(client, &credentials);
  config->access_token = credentials.access_token;
  config->refresh_token = credentials.refresh_token;
  config->id_token = credentials.id_token;
  config->expires_at = credentials.expires_at;
  provider->grok_version++;
  int rc = save_config_tracked(provider, &provider->grok_refresh_unpersisted,
                               &provider->grok_retry_at, 0);
  ClayGrok *copy = NULL;
  if (!rc) {
    ClayGrokCredentials published = {config->access_token, config->refresh_token,
                                    config->id_token, config->expires_at};
    copy = clay_grok_create(&published, NULL);
    if (!copy) rc = -1;
  }
  if (!rc) {
    ClayGrokCredentials old = {0};
    clay_grok_swap_credentials(copy, &old);
    clay_grok_swap_credentials(client, &old);
    clay_grok_credentials_free(&old);
    save->version = provider->grok_version;
  }
  clay_grok_destroy(copy);
  pthread_mutex_unlock(&codex_credentials_lock);
  return rc;
}

int clay_commands_logout_provider(ClayCommands *commands, const char *id) {
  int rc = -1;
  provider_replace_begin(commands);
  for (size_t i = 0; i < commands->providers.count; i++) {
    ClayConnectedProvider *provider = clay_array_get(&commands->providers, i);
    if (strcmp(provider->type->id, id) != 0)
      continue;

    int selected = commands->selected_provider &&
                   strcmp(commands->selected_provider, id) == 0;
    if (selected && clay_config_selection_save(NULL, NULL) != 0)
      goto done;
    if (clay_config_remove(provider->config->id) != 0)
      goto done;

    provider_free(provider);
    clay_array_remove(&commands->providers, i);
    if (selected) {
      free(commands->selected_provider);
      free(commands->selected_model);
      commands->selected_provider = NULL;
      commands->selected_model = NULL;
      clay_commands_update_selected_below(commands);
    }
    rc = 0;
    break;
  }
done:
  provider_replace_end(commands);
  return rc;
}

void clay_commands_update_selected_below(ClayCommands *commands) {
  char *cwd = clay_term_display_cwd();
  clay_below_set_text("cwd", cwd);
  free(cwd);
  ClayStr text;
  clay_str_init(&text);
  if (commands->selected_model && commands->selected_provider) {
    const ClayReasoningEffort *effort =
        clay_commands_reasoning_effort(commands);
    clay_str_printf(&text, "%s%s%s %s·%s %s(%s)%s %s[%s%s%s]%s",
                    clay_color(CLAY_CORAL), commands->selected_model,
                    clay_color(CLAY_RESET), clay_color(CLAY_GRAY),
                    clay_color(CLAY_RESET), clay_color(CLAY_GRAY),
                    commands->selected_provider, clay_color(CLAY_RESET),
                    clay_color(CLAY_GRAY), clay_color(CLAY_CYAN),
                    effort->label,
                    clay_color(CLAY_GRAY), clay_color(CLAY_RESET));
  } else {
    clay_str_push(&text, "None");
  }
  clay_below_set_text("model", text.data);
  clay_str_free(&text);
}

void clay_token_usage_add(ClayTokenUsage *total, const ClayTokenUsage *usage) {
  total->input_tokens += usage->input_tokens;
  total->output_tokens += usage->output_tokens;
  if (usage->cached_input_tokens_known) {
    total->cached_input_tokens += usage->cached_input_tokens;
    total->cached_input_tokens_known = 1;
  }
}

void clay_commands_add_subagent_usage(ClayCommands *commands,
                                      const ClayTokenUsage *usage) {
  pthread_mutex_lock(&commands->tool_lock);
  clay_token_usage_add(&commands->subagent_usage, usage);
  commands->total_input_tokens += usage->input_tokens;
  commands->total_output_tokens += usage->output_tokens;
  pthread_mutex_unlock(&commands->tool_lock);
}

static long effective_context_token_budget(void) {
  long budget = clay_config_context_token_budget();
  return budget < 0 ? 128000 : budget;
}

void clay_commands_record_request_usage(ClayCommands *commands,
                                       const char *kind, int round,
                                       const ClayTokenUsage *usage,
                                       long long duration_ms) {
  if (!commands || !usage || !commands->chat) return;
  ClayChatRequestUsage record = {
      .provider = commands->selected_provider,
      .model = commands->selected_model,
      .kind = kind ? kind : "main",
      .round = round,
      .input_tokens = usage->input_tokens,
      .output_tokens = usage->output_tokens,
      .cached_input_tokens = usage->cached_input_tokens,
      .cached_input_tokens_known = usage->cached_input_tokens_known,
      .context_budget = effective_context_token_budget(),
      .duration_ms = duration_ms,
  };
  if (clay_chat_append_request_usage(commands->chat, &record) != 0)
    fprintf(stderr, "clay: could not append request usage\n");
}

void clay_commands_set_tokens_below(ClayCommands *commands, long input_tokens,
                                    long output_tokens) {
  clay_commands_set_tokens_below_with_cache(commands, input_tokens,
                                            output_tokens, 0, 0);
}

void clay_commands_set_tokens_below_with_cache(
    ClayCommands *commands, long input_tokens, long output_tokens,
    long cached_input_tokens, int cached_input_tokens_known) {
  (void)commands;
  ClayStr text;
  clay_str_init(&text);
  if (cached_input_tokens_known && input_tokens > 0) {
    if (cached_input_tokens < 0)
      cached_input_tokens = 0;
    if (cached_input_tokens > input_tokens)
      cached_input_tokens = input_tokens;
    long percent = (long)((double)cached_input_tokens * 100.0 /
                          (double)input_tokens);
    clay_str_printf(&text, "%s\xe2\x86\x91 %ld  \xe2\x86\x93 %ld  "
                    "%s(cache: %ld%%)%s",
                    clay_color(CLAY_CYAN), input_tokens, output_tokens,
                    clay_color(CLAY_GRAY), percent, clay_color(CLAY_RESET));
  } else {
    clay_str_printf(&text, "%s\xe2\x86\x91 %ld  \xe2\x86\x93 %ld%s",
                    clay_color(CLAY_CYAN), input_tokens, output_tokens,
                    clay_color(CLAY_RESET));
  }
  clay_below_set_text("tokens", text.data);
  clay_str_free(&text);
}

const char *clay_commands_system_prompt_base(void) {
  return CLAY_SYSTEM_PROMPT_BASE;
}

/* FNV-1a over what the cache cannot outlive: the build's base prompt and the
   instruction files a user or a repo can edit. A cache written for different
   text describes a clay that no longer exists, and an edit to AGENTS.md takes
   effect on the next chat instead of waiting out the TTL. */
static double prompt_fingerprint(const char *global_instructions,
                                 const char *project_instructions) {
  unsigned long hash = 2166136261UL;
  const char *parts[] = {CLAY_SYSTEM_PROMPT_BASE, global_instructions,
                         project_instructions};
  for (size_t i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
    if (!parts[i]) continue;
    for (const char *p = parts[i]; *p; p++) {
      hash ^= (unsigned char)*p;
      hash *= 16777619UL;
      hash &= 0xffffffffUL;
    }
  }
  return (double)hash;
}

static char *system_prompt_cache_path(void) {
  return clay_storage_path("system_prompt.json");
}

/* 0 with text_out/last_used_out/cwd_out set on a hit, -1 on a miss. */
static int load_system_prompt_cache(char **text_out, long long *built_at_out,
                                    char **cwd_out, double *fingerprint_out) {
  char *path = system_prompt_cache_path();
  if (!path)
    return -1;
  ClayJson *root = clay_storage_read_json(path, 4 * 1024 * 1024);
  free(path);
  const char *text =
      clay_json_type(root) == CLAY_JSON_OBJECT
          ? clay_json_string_value(clay_json_object_get(root, "text"))
          : NULL;
  if (!text || !*text) {
    clay_json_free(root);
    return -1;
  }
  *text_out = strdup(text);
  *built_at_out = (long long)clay_json_number_value(
      clay_json_object_get(root, "built_at"));
  *fingerprint_out =
      clay_json_number_value(clay_json_object_get(root, "base_fingerprint"));
  const char *cwd = clay_json_string_value(clay_json_object_get(root, "cwd"));
  *cwd_out = strdup(cwd ? cwd : "");
  clay_json_free(root);
  return 0;
}

static void save_system_prompt_cache(const char *text, long long built_at,
                                     const char *cwd, double fingerprint) {
  if (clay_storage_ensure_dir("") != 0)
    return;
  ClayJson *root = clay_json_object();
  clay_json_object_set(root, "text", clay_json_string(text));
  clay_json_object_set(root, "built_at", clay_json_number((double)built_at));
  clay_json_object_set(root, "base_fingerprint", clay_json_number(fingerprint));
  clay_json_object_set(root, "cwd", clay_json_string(cwd));
  char *path = clay_storage_path("system_prompt.json");
  if (!path) {
    clay_json_free(root);
    return;
  }
  clay_storage_write_json_atomic_private(path, root);
  free(path);
  clay_json_free(root);
}

#define CLAY_PROJECT_INSTRUCTIONS_MAX_DEPTH 32

static char *read_file_if_exists(const char *path) {
  ClayStr text;
  if (clay_storage_read_limited(path, 4 * 1024 * 1024, &text) != 0) return NULL;
  return text.data;
}

/* AGENTS.md takes priority over CLAY.md at the same level - only one is
   read per directory. */
static char *read_level_instructions(const char *dir) {
  ClayStr path;
  clay_str_init(&path);
  clay_str_printf(&path, "%s/AGENTS.md", dir);
  char *content = read_file_if_exists(path.data);
  if (!content) {
    clay_str_clear(&path);
    clay_str_printf(&path, "%s/CLAY.md", dir);
    content = read_file_if_exists(path.data);
  }
  clay_str_free(&path);
  return content;
}

/* Root-first, so a project can override a preference the user set once. */
char *clay_commands_load_global_instructions(void) {
  char *dir = clay_storage_root();
  if (!dir)
    return NULL;
  char *content = read_level_instructions(dir);
  free(dir);
  if (!content)
    return NULL;
  size_t length = strlen(content);
  if (length <= CLAY_GLOBAL_INSTRUCTIONS_MAX_BYTES)
    return content;
  ClayStr capped;
  clay_str_init(&capped);
  clay_str_push_n(&capped, content, CLAY_GLOBAL_INSTRUCTIONS_MAX_BYTES);
  clay_str_printf(&capped,
                  "\n\n[%zu bytes of global instructions cut to stay within "
                  "the %d byte limit; read ~/.clay/AGENTS.md for the rest]",
                  length - CLAY_GLOBAL_INSTRUCTIONS_MAX_BYTES,
                  (int)CLAY_GLOBAL_INSTRUCTIONS_MAX_BYTES);
  free(content);
  return capped.data;
}

/* Walks up from the cwd to the repo root (a directory holding .git),
   reading AGENTS.md/CLAY.md at each level - same mechanism as Codex CLI.
   Concatenated root-first, so more specific instructions near the cwd
   follow more general ones, and capped at CLAY_PROJECT_INSTRUCTIONS_MAX_BYTES
   root-first, so the most general instructions survive. Malloc'd; NULL if
   none found. */
char *clay_commands_load_project_instructions(void) {
  char *dir = clay_term_cwd();
  if (!dir)
    return NULL;

  ClayArray levels;
  clay_array_init(&levels, sizeof(char *));
  for (int depth = 0; depth < CLAY_PROJECT_INSTRUCTIONS_MAX_DEPTH; depth++) {
    char *content = read_level_instructions(dir);
    if (content)
      clay_array_push_val(&levels, &content);

    ClayStr git_path;
    clay_str_init(&git_path);
    clay_str_printf(&git_path, "%s/.git", dir);
    struct stat git_info;
    int at_repo_root = stat(git_path.data, &git_info) == 0 &&
                       (S_ISDIR(git_info.st_mode) || S_ISREG(git_info.st_mode));
    clay_str_free(&git_path);
    if (at_repo_root || strcmp(dir, "/") == 0)
      break;

    char *slash = strrchr(dir, '/');
    if (!slash)
      break;
    if (slash == dir)
      slash[1] = '\0'; /* "/foo" -> "/" */
    else
      *slash = '\0';
  }
  free(dir);

  if (levels.count == 0) {
    clay_array_free(&levels);
    return NULL;
  }
  ClayStr combined;
  clay_str_init(&combined);
  size_t cut = 0;
  for (size_t i = levels.count; i-- > 0;) {
    char *content = *(char **)clay_array_get(&levels, i);
    size_t length = strlen(content);
    size_t separator = combined.len > 0 ? 2 : 0;
    size_t room = CLAY_PROJECT_INSTRUCTIONS_MAX_BYTES > combined.len + separator
                      ? CLAY_PROJECT_INSTRUCTIONS_MAX_BYTES -
                            combined.len - separator
                      : 0;
    if (length > room) {
      /* Keep what fits of this file, then stop: the rest is one read away. */
      if (separator) clay_str_push(&combined, "\n\n");
      if (room > 0) clay_str_push_n(&combined, content, room);
      cut += length - room;
      free(content);
      for (size_t j = i; j-- > 0;)
        cut += strlen(*(char **)clay_array_get(&levels, j));
      break;
    }
    if (separator) clay_str_push(&combined, "\n\n");
    clay_str_push_n(&combined, content, length);
    free(content);
  }
  if (cut > 0)
    clay_str_printf(&combined,
                    "\n\n[%zu bytes of project instructions cut to stay "
                    "within the %d byte limit; read the AGENTS.md/CLAY.md "
                    "files for the rest]",
                    cut, (int)CLAY_PROJECT_INSTRUCTIONS_MAX_BYTES);
  clay_array_free(&levels);
  return combined.data;
}

#define CLAY_ENV_REPO_ROOT_MAX_DEPTH 32
#define CLAY_ENV_DIR_LISTING_MAX 50

/* Walks up from the cwd looking for a .git directory, returning its
   current branch (or a short SHA in detached HEAD). Malloc'd; NULL if not
   in a git repo. */
char *clay_commands_find_git_branch(void) {
  char *dir = clay_term_cwd();
  if (!dir)
    return NULL;
  char *branch = NULL;
  for (int depth = 0; depth < CLAY_ENV_REPO_ROOT_MAX_DEPTH; depth++) {
    ClayStr head_path;
    clay_str_init(&head_path);
    clay_str_printf(&head_path, "%s/.git/HEAD", dir);
    FILE *file = fopen(head_path.data, "r");
    clay_str_free(&head_path);
    if (file) {
      ClayStr content;
      clay_str_init(&content);
      int ch;
      while ((ch = fgetc(file)) != EOF && ch != '\n')
        clay_str_push_char(&content, (char)ch);
      fclose(file);
      const char *prefix = "ref: refs/heads/";
      if (strncmp(content.data, prefix, strlen(prefix)) == 0)
        branch = strdup(content.data + strlen(prefix));
      else if (content.len >= 7) {
        ClayStr short_sha;
        clay_str_init(&short_sha);
        clay_str_push_n(&short_sha, content.data, 7);
        branch = short_sha.data;
      }
      clay_str_free(&content);
      break;
    }
    if (strcmp(dir, "/") == 0)
      break;
    char *slash = strrchr(dir, '/');
    if (!slash)
      break;
    if (slash == dir)
      slash[1] = '\0';
    else
      *slash = '\0';
  }
  free(dir);
  return branch;
}

static int compare_strings(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Comma-separated, sorted, capped names of `dir`'s direct entries. Malloc'd;
   NULL if the directory can't be listed. */
char *clay_commands_list_top_level(const char *dir) {
  ClayArray entries;
  if (clay_term_list_entries(dir, &entries) != 0)
    return NULL;
  if (entries.count > 0)
    qsort(entries.data, entries.count, sizeof(char *), compare_strings);

  ClayStr out;
  clay_str_init(&out);
  size_t shown = entries.count < CLAY_ENV_DIR_LISTING_MAX
                     ? entries.count
                     : CLAY_ENV_DIR_LISTING_MAX;
  for (size_t i = 0; i < shown; i++) {
    if (i > 0)
      clay_str_push(&out, ", ");
    clay_str_push(&out, *(char **)clay_array_get(&entries, i));
  }
  if (entries.count > shown)
    clay_str_printf(&out, ", ... (%zu more)", entries.count - shown);
  for (size_t i = 0; i < entries.count; i++)
    free(*(char **)clay_array_get(&entries, i));
  clay_array_free(&entries);
  return out.data;
}

/* Sent fresh with every message instead of living in the system prompt: the
   prompt is frozen for the life of a chat (for the provider's prefix cache),
   and a file listing frozen an hour ago is worse than none - the model acts
   on files that are no longer there. */
char *clay_commands_environment_block(void) {
  ClayStr text;
  clay_str_init(&text);
  char *platform = clay_term_platform_name();
  char *date = clay_time_format_date(clay_time_now());
  clay_str_printf(&text, "Environment right now: %s. Today's date (UTC): %s.",
                  platform, date);
  free(platform);
  free(date);
  char *cwd = clay_term_cwd();
  if (cwd) {
    clay_str_printf(&text, " Working directory: %s.", cwd);
    char *branch = clay_commands_find_git_branch();
    if (branch) {
      clay_str_printf(&text, " Git branch: %s.", branch);
      free(branch);
    }
    char *listing = clay_commands_list_top_level(cwd);
    if (listing && *listing)
      clay_str_printf(&text, "\nTop-level entries as of this message: %s",
                      listing);
    free(listing);
    free(cwd);
  }
  return text.data;
}

/* Where the bytes of the fixed system prompt go. Reported by the budget
   test and, with CLAY_PROMPT_BREAKDOWN=1, by the session itself, so a
   regression can be traced to one section instead of a total. */
void clay_commands_system_prompt_sections(ClayCommandsPromptSections *out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->base = strlen(CLAY_SYSTEM_PROMPT_BASE);
  char *index = clay_memory_index();
  if (*index) out->memory = strlen(index) + strlen("\n\nLong-term memory index:\n");
  free(index);
  char *skills = clay_skill_index();
  if (*skills)
    out->skills = strlen(skills) +
                  strlen("\n\nAvailable skills (call the skill tool with one "
                         "name to load its full instructions):\n");
  free(skills);
  char *global_instructions = clay_commands_load_global_instructions();
  if (global_instructions) {
    out->global = strlen(global_instructions) +
                  strlen("\n\nGlobal user instructions (~/.clay/AGENTS.md):\n");
    free(global_instructions);
  }
  char *project_instructions = clay_commands_load_project_instructions();
  if (project_instructions) {
    out->instructions =
        strlen(project_instructions) +
        strlen("\n\nProject instructions (AGENTS.md/CLAY.md):\n");
    free(project_instructions);
  }
  out->total = out->base + out->memory + out->skills + out->global +
               out->instructions;
}

static char *build_fresh_system_prompt(void) {
  ClayStr text;
  clay_str_init(&text);
  clay_str_push(&text, CLAY_SYSTEM_PROMPT_BASE);

  char *index = clay_memory_index();
  if (*index)
    clay_str_printf(&text, "\n\nLong-term memory index:\n%s", index);
  free(index);

  char *skills = clay_skill_index();
  if (*skills)
    clay_str_printf(&text,
                    "\n\nAvailable skills (call the skill tool with one name "
                    "to load its full instructions):\n%s",
                    skills);
  free(skills);

  char *global_instructions = clay_commands_load_global_instructions();
  if (global_instructions) {
    clay_str_printf(&text,
                    "\n\nGlobal user instructions (~/.clay/AGENTS.md):\n%s",
                    global_instructions);
    free(global_instructions);
  }

  char *project_instructions = clay_commands_load_project_instructions();
  if (project_instructions) {
    clay_str_printf(&text, "\n\nProject instructions (AGENTS.md/CLAY.md):\n%s",
                    project_instructions);
    free(project_instructions);
  }

  if (getenv("CLAY_PROMPT_BREAKDOWN")) {
    ClayCommandsPromptSections sections;
    clay_commands_system_prompt_sections(&sections);
    fprintf(stderr,
            "clay: system prompt %zu B = base %zu + memory %zu + skills %zu "
            "+ global %zu + instructions %zu\n",
            sections.total, sections.base, sections.memory, sections.skills,
            sections.global, sections.instructions);
  }
  return text.data;
}

/* Reuses the cached prompt while it is recent enough that the provider's
   own prefix cache is plausibly still warm; otherwise rebuilds with current
   memory/skill data. Only ever
   called for a chat-less session - clay_commands_reset_conversation
   never calls this once commands->chat exists. */
static char *clay_commands_build_system_prompt(void) {
  char *cached_text = NULL;
  char *cached_cwd = NULL;
  long long built_at = 0;
  double cached_fingerprint = 0;
  long long now = clay_time_now();
  char *cwd = clay_term_cwd();
  /* Read once: the same bytes go into the prompt and into its fingerprint. */
  char *global_instructions = clay_commands_load_global_instructions();
  char *project_instructions = clay_commands_load_project_instructions();
  double fingerprint =
      prompt_fingerprint(global_instructions, project_instructions);
  free(global_instructions);
  free(project_instructions);
  int hit = load_system_prompt_cache(&cached_text, &built_at, &cached_cwd,
                                     &cached_fingerprint) == 0 &&
            cached_fingerprint == fingerprint &&
            now - built_at < CLAY_SYSTEM_PROMPT_TTL_SECONDS && cwd &&
            strcmp(cached_cwd, cwd) == 0;
  free(cached_cwd);
  if (hit) {
    /* Deliberately not re-stamped: the window runs from when the memory and
       skill indexes in this text were read, not from the last session that
       happened to reuse it. */
    free(cwd);
    return cached_text;
  }
  free(cached_text);
  char *fresh = build_fresh_system_prompt();
  save_system_prompt_cache(fresh, now, cwd ? cwd : "", fingerprint);
  free(cwd);
  return fresh;
}

/* One request loop against whichever provider is selected: the three client
   types differ only in how they are built and how their credentials are
   refreshed afterwards. */
int clay_commands_run_completion(ClayCommands *commands, ClayJson *messages,
                                 const ClayToolSet *tools, int max_rounds,
                                 const char *cache_key,
                                 const ClayOpenAICallbacks *callbacks) {
  int rc = -1;
  provider_use_begin(commands);
  ClayConnectedProvider *provider =
      clay_commands_find_provider(commands, commands->selected_provider);
  if (!provider)
    goto done;
  ClayScope *scope = clay_scope_get(commands->scopes, provider->generation,
                                    provider->config->base_url, commands->selected_model);
  if (!scope)
    goto done;
  clay_scope_set_tpm(scope, provider->config->tpm > 0 ? (uint64_t)provider->config->tpm : 0);
  const ClayTool *tool_list = tools ? tools->tools.data : NULL;
  size_t tool_count = tools ? tools->tools.count : 0;
  const char *effort = clay_commands_reasoning_effort(commands)->id;
  int is_openrouter = strcmp(provider->type->id, "openrouter") == 0;
  int is_codex = strcmp(provider->type->id, "openai-codex") == 0;
  int is_grok_subscription =
      strcmp(provider->type->id, "grok") == 0 && provider->grok_client != NULL;

  if (is_codex) {
    uint64_t version;
    ClayOpenAICodex *codex =
        clay_commands_codex_snapshot(provider, commands->selected_model, &version);
    if (!codex)
      goto done;
    clay_openai_codex_set_scope(codex, scope);
    if (commands->completion_test_hook) {
      rc = commands->completion_test_hook(clay_openai_codex_scope(codex), callbacks, commands->completion_test_userdata);
      clay_openai_codex_destroy(codex);
      goto done;
    }
    CodexRefreshSave save = {provider, provider->generation, version};
    clay_openai_codex_set_refresh_observer(codex, save_codex_refresh, &save);
    clay_openai_codex_set_refresh_coordination(codex, begin_codex_refresh, end_codex_refresh, &save);
    clay_openai_codex_set_reasoning_effort(codex, effort);
    clay_openai_codex_set_prompt_cache_key(codex, cache_key);
    rc = clay_openai_codex_run(codex, messages, tool_list, tool_count,
                               max_rounds, callbacks);
    /* Save a refresh or rotated refresh token before discarding the client. */
    if (save_codex_credentials(provider, codex, &save.version) < 0)
      rc = -1;
    clay_openai_codex_destroy(codex);
    goto done;
  }
  if (is_grok_subscription) {
    uint64_t version;
    ClayGrok *grok = clay_commands_grok_snapshot(provider, commands->selected_model, &version);
    if (!grok)
      goto done;
    clay_grok_set_scope(grok, scope);
    if (commands->completion_test_hook) {
      rc = commands->completion_test_hook(clay_grok_scope(grok), callbacks, commands->completion_test_userdata);
      clay_grok_destroy(grok);
      goto done;
    }
    GrokRefreshSave save = {provider, provider->generation, version};
    clay_grok_set_refresh_observer(grok, save_grok_refresh, &save);
    clay_grok_set_refresh_coordination(grok, begin_grok_refresh, end_grok_refresh, &save);
    clay_grok_set_reasoning_effort(grok, effort);
    clay_grok_set_conversation_id(grok, cache_key);
    rc = clay_grok_run(grok, messages, tool_list, tool_count, max_rounds, callbacks);
    if (clay_commands_save_grok_credentials(provider, grok, save.version) < 0)
      rc = -1;
    clay_grok_destroy(grok);
    goto done;
  }
  ClayOpenAI *client =
      clay_openai_create(provider->config->base_url, provider->config->apikey,
                         commands->selected_model);
  if (!client)
    goto done;
  clay_openai_set_scope(client, scope);
  if (commands->completion_test_hook) {
    rc = commands->completion_test_hook(clay_openai_scope(client), callbacks, commands->completion_test_userdata);
    clay_openai_destroy(client);
    goto done;
  }
  if (is_openrouter && effort && strcmp(effort, "max") == 0)
    clay_openai_set_openrouter_max_reasoning(client);
  else
    clay_openai_set_reasoning_effort(client, effort);
  if (strcmp(provider->type->id, "openai") == 0)
    clay_openai_set_prompt_cache_key(client, cache_key);
  else if (strcmp(provider->type->id, "grok") == 0)
    clay_openai_set_extra_header(client, "x-grok-conv-id", cache_key);
  rc = clay_openai_run(client, messages, tool_list, tool_count, max_rounds,
                       callbacks);
  clay_openai_destroy(client);
done:
  provider_use_end(commands);
  return rc;
}

/* The environment is re-read before every turn; the listing alone can run to
   50 names. Sending the whole block again for each change grows the history
   quadratically in the number of turns, so after the first block only what
   actually changed is appended. Pure and string-only, so it is testable
   without a session. */
#define CLAY_ENV_LISTING_MARKER "\nTop-level entries as of this message: "
#define CLAY_ENV_DELTA_MAX_ENTRIES 20

static const char *listing_part(const char *block, const char **start) {
  const char *marker_at = strstr(block, CLAY_ENV_LISTING_MARKER);
  if (!marker_at) {
    *start = NULL;
    return NULL;
  }
  *start = marker_at + strlen(CLAY_ENV_LISTING_MARKER);
  return *start;
}

/* Whether `entries` (comma-separated) contains `name` of `length`. */
static int listing_has(const char *entries, const char *name, size_t length) {
  const char *cursor = entries;
  while (cursor && *cursor) {
    const char *comma = strstr(cursor, ", ");
    size_t item = comma ? (size_t)(comma - cursor) : strlen(cursor);
    if (item == length && strncmp(cursor, name, length) == 0) return 1;
    cursor = comma ? comma + 2 : NULL;
  }
  return 0;
}

/* Whether `block` holds `line` as a whole line. */
static int line_in_block(const char *block, const char *line) {
  size_t length = strlen(line);
  const char *cursor = block;
  while (cursor && *cursor) {
    size_t item = strcspn(cursor, "\n");
    if (item == length && strncmp(cursor, line, length) == 0) return 1;
    cursor = cursor[item] == '\n' ? cursor + item + 1 : NULL;
  }
  return 0;
}

/* One side of a listing difference, capped so one mass rename cannot cost
   more than the block it replaces. */
static void push_listing_diff(ClayStr *out, const char *from, const char *to) {
  const char *cursor = to;
  size_t added = 0, named = 0;
  while (cursor && *cursor) {
    const char *comma = strstr(cursor, ", ");
    size_t item = comma ? (size_t)(comma - cursor) : strlen(cursor);
    if (item > 0 && !listing_has(from, cursor, item)) {
      added++;
      if (named < CLAY_ENV_DELTA_MAX_ENTRIES) {
        clay_str_printf(out, " +%.*s", (int)item, cursor);
        named++;
      }
    }
    cursor = comma ? comma + 2 : NULL;
  }
  cursor = from;
  size_t removed = 0;
  while (cursor && *cursor) {
    const char *comma = strstr(cursor, ", ");
    size_t item = comma ? (size_t)(comma - cursor) : strlen(cursor);
    if (item > 0 && !listing_has(to, cursor, item)) {
      removed++;
      if (named < CLAY_ENV_DELTA_MAX_ENTRIES) {
        clay_str_printf(out, " -%.*s", (int)item, cursor);
        named++;
      }
    }
    cursor = comma ? comma + 2 : NULL;
  }
  if (added + removed > named)
    clay_str_printf(out, " (%zu more unfinished)", added + removed - named);
}

/* NULL when nothing changed. Otherwise a block that says only what moved. */
char *clay_commands_environment_delta(const char *previous,
                                      const char *current) {
  if (!previous || !current) return NULL;
  ClayStr out;
  clay_str_init(&out);
  const char *previous_start = NULL, *current_start = NULL;
  const char *previous_listing = listing_part(previous, &previous_start);
  const char *current_listing = listing_part(current, &current_start);
  size_t previous_header = previous_start ? (size_t)(previous_start - previous) -
                                               strlen(CLAY_ENV_LISTING_MARKER)
                                         : strlen(previous);
  size_t current_header = current_start ? (size_t)(current_start - current) -
                                             strlen(CLAY_ENV_LISTING_MARKER)
                                       : strlen(current);
  if (previous_header != current_header ||
      strncmp(previous, current, current_header) != 0) {
    clay_str_printf(&out, "Environment changed: %.*s", (int)current_header,
                    current);
  }
  if (previous_listing && current_listing &&
      strcmp(previous_listing, current_listing) != 0) {
    ClayStr difference;
    clay_str_init(&difference);
    push_listing_diff(&difference, previous_listing, current_listing);
    if (difference.len) {
      clay_str_printf(&out, "%sTop-level entries changed:%s",
                      out.len ? "\n" : "Top-level entries:",
                      difference.data);
    }
    clay_str_free(&difference);
  } else if (!previous_listing && current_listing) {
    clay_str_printf(&out, "%sTop-level entries as of this message: %s",
                    out.len ? "\n" : "", current_listing);
  }
  if (out.len == 0) {
    clay_str_free(&out);
    return NULL;
  }
  return out.data;
}

/* Line-level difference for the notes scratchpad, which is replaced whole on
   every write: re-sending all of it each time is what accumulated copies in
   the first place. NULL when nothing changed. */
char *clay_commands_text_delta(const char *previous, const char *current) {
  if (!previous || !current) return NULL;
  if (strcmp(previous, current) == 0) return NULL;
  ClayStr out;
  clay_str_init(&out);
  const char *cursor = current;
  while (cursor && *cursor) {
    const char *newline = strchr(cursor, '\n');
    size_t length = newline ? (size_t)(newline - cursor) : strlen(cursor);
    ClayStr line;
    clay_str_init(&line);
    clay_str_push_n(&line, cursor, length);
    if (!line_in_block(previous, line.data)) {
      if (out.len)
        clay_str_push_char(&out, '\n');
      clay_str_printf(&out, "+ %s", line.data);
    }
    clay_str_free(&line);
    cursor = newline ? newline + 1 : NULL;
  }
  cursor = previous;
  while (cursor && *cursor) {
    const char *newline = strchr(cursor, '\n');
    size_t length = newline ? (size_t)(newline - cursor) : strlen(cursor);
    ClayStr line;
    clay_str_init(&line);
    clay_str_push_n(&line, cursor, length);
    if (!line_in_block(current, line.data)) {
      if (out.len)
        clay_str_push_char(&out, '\n');
      clay_str_printf(&out, "- %s", line.data);
    }
    clay_str_free(&line);
    cursor = newline ? newline + 1 : NULL;
  }
  if (out.len == 0) {
    clay_str_free(&out);
    return NULL;
  }
  return out.data;
}

/* Appends `text` as a system message the first time it differs from what
   `slot` last held, and only the difference after that. Deliberately kept in
   the conversation rather than injected per request and removed: the
   provider's prefix cache only holds while the message array grows at the
   end, and a block that appears and disappears in the middle invalidates
   every turn after it. `delta` computes what to send given the previous
   block, or NULL when what would be sent is the whole block again. */
static void append_context_block(ClayCommands *commands, char **slot,
                                 const char *text,
                                 char *(*delta)(const char *, const char *)) {
  if (*slot && strcmp(*slot, text) == 0)
    return;
  char *pending = *slot ? delta(*slot, text) : NULL;
  free(*slot);
  *slot = strdup(text);
  clay_json_array_push(commands->conversation,
                       clay_openai_message("system",
                                           pending ? pending : text));
  free(pending);
}

void clay_commands_sync_context_blocks(ClayCommands *commands) {
  char *environment = clay_commands_environment_block();
  append_context_block(commands, &commands->environment_block, environment,
                       clay_commands_environment_delta);
  free(environment);

  const char *notes = clay_chat_notes(commands->chat);
  if (!*notes)
    return;
  ClayStr block;
  clay_str_init(&block);
  clay_str_printf(&block, "Notes from earlier in this conversation:\n%s", notes);
  append_context_block(commands, &commands->notes_block, block.data,
                       clay_commands_text_delta);
  clay_str_free(&block);
}

void clay_commands_forget_context_blocks(ClayCommands *commands) {
  free(commands->environment_block);
  commands->environment_block = NULL;
  free(commands->notes_block);
  commands->notes_block = NULL;
}

void clay_commands_reset_conversation(ClayCommands *commands) {
  commands->last_prompt_tokens = 0;
  commands->accounted_messages = 0;
  clay_json_free(commands->conversation);
  commands->conversation = clay_json_array();
  /* The blocks just left the history; the next sync appends them afresh
     rather than as a difference against a block that is gone. */
  clay_commands_forget_context_blocks(commands);
  free(commands->system_prompt);
  const char *persisted =
      commands->chat ? clay_chat_system_prompt(commands->chat) : "";
  commands->system_prompt =
      *persisted ? strdup(persisted) : clay_commands_build_system_prompt();
  clay_json_array_push(commands->conversation,
                       clay_openai_message("system", commands->system_prompt));
  if (!commands->chat)
    return;
  ClayJson *history = clay_chat_openai_messages(commands->chat);
  for (size_t i = 0; i < clay_json_array_count(history); i++) {
    clay_json_array_push(commands->conversation,
                         clay_json_clone(clay_json_array_get(history, i)));
  }
  clay_json_free(history);
}

#define CLAY_CONTEXT_KEEP_TURNS 6
#define CLAY_CONTEXT_TOOL_PREVIEW 200
#define CLAY_CONTEXT_COLLAPSED_MARKER "[collapsed - "

static int is_user_message(const ClayJson *message) {
  return strcmp(clay_json_string_value(clay_json_object_get(message, "role")),
                "user") == 0;
}

static int collapse_tool_content(ClayJson *message, ClayArray *saved_results) {
  const char *text =
      clay_json_string_value(clay_json_object_get(message, "content"));
  if (!text)
    return 0;
  size_t len = strlen(text);
  if (len <= CLAY_CONTEXT_TOOL_PREVIEW ||
      strncmp(text, CLAY_CONTEXT_COLLAPSED_MARKER,
              strlen(CLAY_CONTEXT_COLLAPSED_MARKER)) == 0) {
    return 0;
  }
  size_t preview = 0;
  while (preview < CLAY_CONTEXT_TOOL_PREVIEW) {
    size_t bytes;
    clay_utf8_char_width(text + preview, &bytes);
    if (preview + bytes > CLAY_CONTEXT_TOOL_PREVIEW)
      break;
    preview += bytes;
  }
  ClayStr summary;
  clay_str_init(&summary);
  clay_str_printf(&summary, "%s%zu bytes, showing the first %d]\n%.*s...",
                  CLAY_CONTEXT_COLLAPSED_MARKER, len, CLAY_CONTEXT_TOOL_PREVIEW,
                  (int)preview, text);
  if (saved_results) {
    ClayCompactedToolResult original = {
        message, clay_json_clone(clay_json_object_get(message, "content"))};
    clay_array_push_val(saved_results, &original);
  }
  clay_json_object_set(message, "content", clay_json_string(summary.data));
  clay_str_free(&summary);
  return 1;
}

/* Characters per token for the estimate below: the same 2 bytes/token the
   scope admits requests with (scope.c). */
#define CLAY_CONTEXT_BYTES_PER_TOKEN 2
/* Fixed per-message overhead the provider bills: role, JSON punctuation. */
#define CLAY_CONTEXT_MESSAGE_OVERHEAD 4

/* Bytes still to be paid for: everything after the point the last reported
   prompt already covered. */
static long unaccounted_tokens(const ClayJson *conversation, size_t from) {
  size_t total = clay_json_array_count(conversation);
  if (from > total) from = total;
  long bytes = 0;
  for (size_t i = from; i < total; i++) {
    ClayStr text;
    clay_str_init(&text);
    clay_json_stringify(clay_json_array_get(conversation, i), &text);
    bytes += (long)text.len;
    clay_str_free(&text);
  }
  return bytes / CLAY_CONTEXT_BYTES_PER_TOKEN +
         (long)((total - from) * CLAY_CONTEXT_MESSAGE_OVERHEAD);
}

static int compact_history(ClayJson *conversation, long billed_tokens,
                           size_t accounted_messages, ClayArray *saved_results) {
  long token_budget = effective_context_token_budget();
  if (token_budget == 0)
    return 0;
  if (billed_tokens < 0)
    billed_tokens = 0;
  if (billed_tokens < token_budget &&
      unaccounted_tokens(conversation, accounted_messages) <
          token_budget - billed_tokens)
    return 0;
  size_t count = clay_json_array_count(conversation);
  if (count < 2)
    return 0;

  /* Subagents usually have one user message. Protect the entire latest tool
     round; for the main session, protect the last few user turns. */
  size_t cutoff = 1;
  int turns_seen = 0;
  for (size_t i = count; i-- > 1;) {
    const ClayJson *message = clay_json_array_get(conversation, i);
    int keep_from_here = saved_results
        ? clay_json_array_count(clay_json_object_get(message, "tool_calls")) > 0
        : is_user_message(message) && ++turns_seen == CLAY_CONTEXT_KEEP_TURNS;
    if (keep_from_here) {
      cutoff = i;
      break;
    }
  }

  int collapsed = 0;
  for (size_t i = 1; i < cutoff; i++) {
    ClayJson *message = clay_json_array_get(conversation, i);
    if (strcmp(clay_json_string_value(clay_json_object_get(message, "role")),
               "tool") == 0) {
      collapsed += collapse_tool_content(message, saved_results);
    }
  }
  return collapsed;
}

int clay_commands_maybe_compact(ClayCommands *commands) {
  return compact_history(commands->conversation, commands->last_prompt_tokens,
                         commands->accounted_messages, NULL);
}

int clay_commands_compact_subagent_history(ClayJson *messages, long billed_tokens,
                                           size_t accounted_messages,
                                           ClayArray *saved_results) {
  return compact_history(messages, billed_tokens, accounted_messages,
                         saved_results);
}

void clay_plan_clear(ClayPlan *plan) {
  for (size_t i = 0; i < plan->todos.count; i++) {
    ClayTodoItem *item = clay_array_get(&plan->todos, i);
    free(item->content);
    free(item->status);
    free(item->shown);
  }
  clay_array_clear(&plan->todos);
  if (plan->rendered)
    clay_below_set_enabled("plan", 0);
}

void clay_commands_new_chat(ClayCommands *commands) {
  clay_chat_destroy(commands->chat);
  commands->chat = NULL;
  clay_commands_reset_conversation(commands);
  clay_below_stop_elapsed("status");
  clay_below_set_enabled("status", 0);
  clay_commands_set_tokens_below(commands, 0, 0);
  clay_plan_clear(&commands->plan);
}

int clay_commands_select_model(ClayCommands *commands, const char *provider,
                               const char *model) {
  int changed = !commands->selected_provider || !commands->selected_model ||
                strcmp(commands->selected_provider, provider) != 0 ||
                strcmp(commands->selected_model, model) != 0;
  char *provider_copy = strdup(provider);
  char *model_copy = strdup(model);
  free(commands->selected_provider);
  free(commands->selected_model);
  commands->selected_provider = provider_copy;
  commands->selected_model = model_copy;
  if (changed) {
    clay_chat_destroy(commands->chat);
    commands->chat = NULL;
    clay_commands_reset_conversation(commands);
    clay_commands_set_tokens_below(commands, 0, 0);
  }
  clay_commands_update_selected_below(commands);
  return clay_config_selection_save(commands->selected_provider,
                                    commands->selected_model);
}

int clay_commands_fetch_models(void *ctx, ClayArray *out) {
  ClayConnectedProvider *provider = ctx;
  ClayCommands *commands = provider->commands;
  if (commands) provider_use_begin(commands);
  if (!provider->models_fetched) {
    ClayTask *task =
        clay_task_start("Retrieving %s models", provider->type->label);
    if (strcmp(provider->type->id, "openai-codex") == 0) {
      uint64_t version = 0;
      ClayOpenAICodex *codex = clay_commands_codex_snapshot(provider, NULL, &version);
      CodexRefreshSave save = {provider, provider->generation, version};
      if (codex) {
        clay_openai_codex_set_refresh_observer(codex, save_codex_refresh, &save);
        clay_openai_codex_set_refresh_coordination(codex, begin_codex_refresh, end_codex_refresh, &save);
      }
      provider->models_rc = codex ? clay_openai_codex_list_models(codex, &provider->models) : -1;
      provider->models_status = codex ? clay_openai_codex_last_status(codex) : 0;
      /* Save even on failure: invalid_grant cleanup lives in save_codex_credentials. */
      if (codex && save_codex_credentials(provider, codex, &save.version) < 0)
        provider->models_rc = -1;
      clay_openai_codex_destroy(codex);
    } else if (strcmp(provider->type->id, "grok") == 0 &&
               provider->grok_client) {
      uint64_t version = 0;
      ClayGrok *grok = clay_commands_grok_snapshot(provider, NULL, &version);
      GrokRefreshSave save = {provider, provider->generation, version};
      if (grok) {
        clay_grok_set_refresh_observer(grok, save_grok_refresh, &save);
        clay_grok_set_refresh_coordination(grok, begin_grok_refresh, end_grok_refresh, &save);
      }
      provider->models_rc = grok ? clay_grok_list_models(grok, &provider->models) : -1;
      provider->models_status = grok ? clay_grok_last_status(grok) : 0;
      if (grok && clay_commands_save_grok_credentials(provider, grok, save.version) < 0)
        provider->models_rc = -1;
      clay_grok_destroy(grok);
    } else {
      provider->models_rc =
          clay_openai_list_models(provider->client, &provider->models);
    }
    provider->models_fetched = 1;
    if (provider->models_rc == 0)
      clay_task_success(task, "%zu models available", provider->models.count);
    else if (provider->models_status < 200 || provider->models_status >= 300)
      clay_task_fail(task, "HTTP %ld", provider->models_status);
    else
      clay_task_fail(task, "Invalid or empty model catalog");
  }
  for (size_t i = 0; i < provider->models.count; i++) {
    ClayModelItem item = {*(char **)clay_array_get(&provider->models, i), NULL};
    clay_array_push_val(out, &item);
  }
  if (commands) provider_use_end(commands);
  return provider->models_rc;
}

const ClayReasoningEffort *
clay_commands_reasoning_effort(const ClayCommands *commands) {
  return &REASONING_EFFORTS[commands->reasoning_effort_index];
}

size_t clay_commands_reasoning_effort_count(void) {
  return sizeof(REASONING_EFFORTS) / sizeof(REASONING_EFFORTS[0]);
}

const ClayReasoningEffort *clay_commands_reasoning_efforts(void) {
  return REASONING_EFFORTS;
}

void clay_commands_print_history(ClayCommands *commands, size_t count) {
  ClayJson *messages = clay_chat_recent_messages(commands->chat, count);
  for (size_t i = 0; i < clay_json_array_count(messages); i++) {
    ClayJson *message = clay_json_array_get(messages, i);
    const char *role =
        clay_json_string_value(clay_json_object_get(message, "role"));
    const char *content =
        clay_json_string_value(clay_json_object_get(message, "content"));
    clay_sayc(strcmp(role, "user") == 0 ? CLAY_CYAN : CLAY_GRAY, "%s: %s",
              strcmp(role, "user") == 0 ? "You" : "Clay", content);
  }
  clay_json_free(messages);
}

ClayCommands *clay_commands_create(ClayApp *app) {
  ClayCommands *commands = calloc(1, sizeof(ClayCommands));
  if (!commands)
    return NULL;
  pthread_mutex_init(&commands->provider_gate_lock, NULL);
  pthread_cond_init(&commands->provider_gate_cond, NULL);
  commands->scopes = clay_scope_registry_create();
  if (!commands->scopes) {
    free(commands);
    return NULL;
  }
  commands->app = app;
  commands->running = 1;
  clay_array_init(&commands->providers, sizeof(ClayConnectedProvider));
  size_t count;
  const ClayProviderType *types = clay_commands_provider_types(&count);
  for (size_t i = 0; i < count; i++)
    clay_commands_load_provider(commands, &types[i]);
  clay_config_selection_load(&commands->selected_provider,
                             &commands->selected_model);
  int environment_selection = 0;
  const char *forced_provider = env_value("CLAY_PROVIDER", NULL);
  if (!forced_provider && !commands->selected_provider)
    forced_provider = environment_provider();
  if (forced_provider && clay_commands_find_provider(commands, forced_provider)) {
    if (!commands->selected_provider ||
        strcmp(commands->selected_provider, forced_provider) != 0) {
      free(commands->selected_provider);
      commands->selected_provider = strdup(forced_provider);
      free(commands->selected_model);
      commands->selected_model = NULL;
      environment_selection = 1;
    }
  } else if (!commands->selected_provider && commands->providers.count == 1) {
    ClayConnectedProvider *only =
        clay_array_get(&commands->providers, 0);
    commands->selected_provider = strdup(only->type->id);
  }
  if (commands->selected_provider) {
    const char *model = environment_model(commands->selected_provider);
    if (model && *model) {
      free(commands->selected_model);
      commands->selected_model = strdup(model);
      environment_selection = 1;
    }
  }
  char *saved_effort = clay_config_reasoning_effort();
  if (saved_effort) {
    for (size_t i = 1; i < clay_commands_reasoning_effort_count(); i++)
      if (strcmp(REASONING_EFFORTS[i].id, saved_effort) == 0) {
        commands->reasoning_effort_index = (int)i;
        break;
      }
  }
  free(saved_effort);
  char *sandbox_mode = clay_config_sandbox_mode();
  clay_commands_parse_sandbox_mode(sandbox_mode, &commands->sandbox_mode,
                                   &commands->sandbox_auto_approve);
  free(sandbox_mode);
  commands->use_integrated_shell = clay_config_use_integrated_shell();
  if (!clay_sandbox_supported()) {
    commands->sandbox_mode = CLAY_SANDBOX_MODE_UNLEASHED;
    commands->sandbox_auto_approve = 0;
  }
  for (int i = 0; i < CLAY_PERMISSION_CATEGORY_COUNT; i++) {
    commands->auto_approve[i] = clay_config_auto_approve(
        clay_permissions_category_name((ClayPermissionCategory)i));
    clay_array_init(&commands->remembered_patterns[i], sizeof(char *));
  }
  clay_array_init(&commands->plan.todos, sizeof(ClayTodoItem));
  commands->plan.rendered = 1;
  clay_array_init(&commands->mcp_servers, sizeof(ClayMcpServer *));
  clay_array_init(&commands->undo_history, sizeof(ClayUndoEntry));
  clay_array_init(&commands->tasks, sizeof(ClayBackgroundTask *));
  if (clay_sandbox_supported())
    commands->sandbox_namespaces = clay_sandbox_namespaces_create();
  pthread_mutexattr_t tool_lock_attributes;
  pthread_mutexattr_init(&tool_lock_attributes);
  pthread_mutexattr_settype(&tool_lock_attributes, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&commands->tool_lock, &tool_lock_attributes);
  pthread_mutexattr_destroy(&tool_lock_attributes);
  commands->auto_test_command = clay_config_auto_test_command();
  commands->auto_test_choice = CLAY_AUTO_TEST_UNASKED;
  clay_commands_reset_conversation(commands);
  if (!environment_selection)
    clay_config_selection_save(commands->selected_provider,
                               commands->selected_model);
  /* Leftmost so the spinner and elapsed time are the first thing the eye
     lands on while a turn runs. */
  clay_below_add(0, "status");
  clay_below_set_alignment("status", CLAY_BELOW_ALIGN_LEFT);
  clay_below_set_enabled("status", 0);
  clay_below_add(1, "cwd");
  clay_below_set_alignment("cwd", CLAY_BELOW_ALIGN_LEFT);
  /* First thing dropped when the row runs out of space - the shell prompt
     already says where we are. */
  clay_below_set_optional("cwd", 1);
  clay_below_add(2, "model");
  clay_below_set_alignment("model", CLAY_BELOW_ALIGN_LEFT);
  clay_below_add(3, "tokens");
  clay_below_set_alignment("tokens", CLAY_BELOW_ALIGN_RIGHT);
  clay_below_add(4, "hint");
  clay_below_set_enabled("hint", 0);
  clay_below_add(5, "mode");
  clay_below_set_enabled("mode", 0);
  /* Not droppable: while a plan is running, the step in flight is the most
     useful thing on the row. */
  clay_below_add(6, "plan");
  clay_below_set_enabled("plan", 0);
  clay_below_add(7, "tasks");
  clay_below_set_enabled("tasks", 0);
  clay_below_add(8, "sandbox");
  clay_below_set_alignment("sandbox", CLAY_BELOW_ALIGN_RIGHT);
  clay_commands_set_tokens_below(commands, 0, 0);
  clay_commands_update_selected_below(commands);
  clay_commands_update_sandbox_below(commands);
  return commands;
}

void clay_commands_destroy(ClayCommands *commands) {
  if (!commands)
    return;
  clay_commands_stop_tasks(commands);
  clay_scope_registry_destroy(commands->scopes);
  clay_sandbox_namespaces_destroy(commands->sandbox_namespaces);
  pthread_mutex_destroy(&commands->tool_lock);
  pthread_cond_destroy(&commands->provider_gate_cond);
  pthread_mutex_destroy(&commands->provider_gate_lock);
  free(commands->environment_block);
  free(commands->notes_block);
  pthread_mutex_lock(&codex_credentials_lock);
  for (size_t i = 0; i < commands->providers.count; i++) {
    ClayConnectedProvider *provider = clay_array_get(&commands->providers, i);
    if (retry_codex_unpersisted(provider, 1) != 0)
      fprintf(stderr, "warning: could not save refreshed codex credentials for %s\n",
              provider->config->id);
    if (retry_grok_unpersisted(provider, 1) != 0)
      fprintf(stderr, "warning: could not save refreshed grok credentials for %s\n",
              provider->config->id);
  }
  pthread_mutex_unlock(&codex_credentials_lock);
  for (size_t i = 0; i < commands->providers.count; i++)
    provider_free(clay_array_get(&commands->providers, i));
  clay_array_free(&commands->providers);
  clay_json_free(commands->conversation);
  free(commands->system_prompt);
  clay_chat_destroy(commands->chat);
  free(commands->selected_provider);
  free(commands->selected_model);
  for (int i = 0; i < CLAY_PERMISSION_CATEGORY_COUNT; i++) {
    ClayArray *remembered = &commands->remembered_patterns[i];
    for (size_t j = 0; j < remembered->count; j++)
      free(*(char **)clay_array_get(remembered, j));
    clay_array_free(remembered);
  }
  clay_plan_clear(&commands->plan);
  clay_array_free(&commands->plan.todos);
  clay_commands_undo_destroy(commands);
  for (size_t i = 0; i < commands->mcp_servers.count; i++) {
    clay_mcp_disconnect(
        *(ClayMcpServer **)clay_array_get(&commands->mcp_servers, i));
  }
  clay_array_free(&commands->mcp_servers);
  free(commands->auto_test_command);
  free(commands);
}

int clay_commands_running(const ClayCommands *commands) {
  return commands->running;
}

int clay_commands_has_provider(const ClayCommands *commands) {
  return commands->providers.count > 0;
}

/* Totals come from the request log, which is one line per billed provider
   request; tool calls come from the conversation itself. Counting assistant
   messages instead would count rounds, not requests. */
void clay_commands_print_json_summary(const ClayCommands *commands,
                                      long long duration_ms) {
  ClayChatRequestTotals totals = {0};
  if (commands->chat) clay_chat_request_totals(commands->chat, &totals);
  long tool_calls = 0;
  const char *answer = "";
  for (size_t i = 0; i < clay_json_array_count(commands->conversation); i++) {
    ClayJson *message = clay_json_array_get(commands->conversation, i);
    const char *role =
        clay_json_string_value(clay_json_object_get(message, "role"));
    if (!role || strcmp(role, "assistant") != 0) continue;
    tool_calls += (long)clay_json_array_count(
        clay_json_object_get(message, "tool_calls"));
    const char *content =
        clay_json_string_value(clay_json_object_get(message, "content"));
    if (content && *content) answer = content;
  }
  ClayJson *summary = clay_json_object();
  clay_json_object_set(summary, "provider",
                       clay_json_string(commands->selected_provider
                                            ? commands->selected_provider : ""));
  clay_json_object_set(summary, "model",
                       clay_json_string(commands->selected_model
                                            ? commands->selected_model : ""));
  clay_json_object_set(summary, "requests", clay_json_number((double)totals.requests));
  clay_json_object_set(summary, "tool_calls", clay_json_number((double)tool_calls));
  clay_json_object_set(summary, "input_tokens",
                       clay_json_number((double)totals.input_tokens));
  clay_json_object_set(summary, "output_tokens",
                       clay_json_number((double)totals.output_tokens));
  if (totals.any_cached_known)
    clay_json_object_set(summary, "cached_tokens",
                         clay_json_number((double)totals.cached_tokens));
  clay_json_object_set(summary, "duration_ms", clay_json_number((double)duration_ms));
  clay_json_object_set(summary, "answer", clay_json_string(answer));
  ClayStr text;
  clay_str_init(&text);
  clay_json_stringify(summary, &text);
  clay_json_free(summary);
  printf("%s\n", text.data);
  fflush(stdout);
  clay_str_free(&text);
}

void clay_commands_print_session_summary(const ClayCommands *commands) {
  printf("%sSession summary%s  %s\xe2\x86\x91 %ld%s  %s\xe2\x86\x93 %ld%s  "
         "%s%ld messages sent%s\n",
         clay_color(CLAY_GRAY), clay_color(CLAY_RESET), clay_color(CLAY_CYAN),
         commands->total_input_tokens, clay_color(CLAY_RESET),
         clay_color(CLAY_CYAN), commands->total_output_tokens,
         clay_color(CLAY_RESET), clay_color(CLAY_GRAY), commands->messages_sent,
         clay_color(CLAY_RESET));
}
