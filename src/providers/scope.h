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
/* Admission for one HTTP attempt: 1 held, 0 cancelled, -1 not admitted (see commands/scope.c). */
int clay_scope_try_acquire(ClayScope *);
void clay_scope_note_response(ClayScope *, const ClayHttpResponse *, int);
void clay_scope_set_tpm(ClayScope *, uint64_t);
int clay_scope_admit(ClayScope *, size_t, int (*)(void *), void *, uint64_t *);
void clay_scope_finish(ClayScope *, uint64_t, int, uint64_t);
int clay_scope_acquire(ClayScope *, int (*)(void *), void *);
void clay_scope_release(ClayScope *);
#endif
