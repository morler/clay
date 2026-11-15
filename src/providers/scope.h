#ifndef CLAY_PROVIDER_SCOPE_H
#define CLAY_PROVIDER_SCOPE_H
#include "clay/providers/openai.h"
#include "clay/providers/openai_codex.h"
#include "clay/providers/grok.h"
typedef struct ClayScope ClayScope;
void clay_openai_set_scope(ClayOpenAI *, ClayScope *);
ClayScope *clay_openai_scope(const ClayOpenAI *);
void clay_openai_codex_set_scope(ClayOpenAICodex *, ClayScope *);
ClayScope *clay_openai_codex_scope(const ClayOpenAICodex *);
void clay_grok_set_scope(ClayGrok *, ClayScope *);
ClayScope *clay_grok_scope(const ClayGrok *);
#endif
