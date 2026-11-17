#include "../src/commands/context.h"

#include "clay/json.h"
#include "clay/str.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void push_message(ClayJson *conversation, const char *role, const char *content) {
    ClayJson *message = clay_json_object();
    clay_json_object_set(message, "role", clay_json_string(role));
    if (content) clay_json_object_set(message, "content", clay_json_string(content));
    clay_json_array_push(conversation, message);
}

/* The requirement survives a compaction: the summarizer's prose is context,
   the user's own words are the spec, and a long paste is cut with a note. */
static void test_user_requests_kept(void) {
    ClayJson *conversation = clay_json_array();
    push_message(conversation, "system", "you are clay");
    push_message(conversation, "user", "Keep every public API stable.");
    push_message(conversation, "assistant", "Understood.");
    ClayStr huge;
    clay_str_init(&huge);
    for (int i = 0; i < 6000; i++) clay_str_push_char(&huge, 'p');
    ClayJson *pasted = clay_json_object();
    clay_json_object_set(pasted, "role", clay_json_string("user"));
    clay_json_object_set(pasted, "content", clay_json_string(huge.data));
    clay_json_array_push(conversation, pasted);

    char *requests = clay_commands_collect_user_requests(conversation);
    assert(requests);
    assert(strstr(requests, "Keep every public API stable."));
    /* 4,000 of the 6,000 pasted characters survive, and the cut is named. */
    assert(strstr(requests, "2000 characters cut"));
    assert(strlen(requests) < 6000);
    free(requests);
    clay_str_free(&huge);
    clay_json_free(conversation);
}

int main(void) {
    ClayJson *conversation = clay_json_array();
    push_message(conversation, "system", "you are clay"); /* skipped by the transcript builder */
    push_message(conversation, "user", "read main.c and summarize it");

    ClayJson *assistant_call = clay_json_object();
    clay_json_object_set(assistant_call, "role", clay_json_string("assistant"));
    ClayJson *tool_calls = clay_json_array();
    ClayJson *call = clay_json_object();
    clay_json_object_set(call, "id", clay_json_string("call_1"));
    ClayJson *function = clay_json_object();
    clay_json_object_set(function, "name", clay_json_string("read"));
    clay_json_object_set(function, "arguments", clay_json_string("{\"path\":\"main.c\"}"));
    clay_json_object_set(call, "function", function);
    clay_json_array_push(tool_calls, call);
    clay_json_object_set(assistant_call, "tool_calls", tool_calls);
    clay_json_array_push(conversation, assistant_call);

    ClayStr long_output;
    clay_str_init(&long_output);
    for (int i = 0; i < 500; i++) clay_str_push_char(&long_output, 'x');
    ClayJson *tool_result = clay_json_object();
    clay_json_object_set(tool_result, "role", clay_json_string("tool"));
    clay_json_object_set(tool_result, "content", clay_json_string(long_output.data));
    clay_json_array_push(conversation, tool_result);

    push_message(conversation, "assistant", "main.c defines the process entry point.");

    char *transcript = clay_commands_build_compact_transcript(conversation);
    assert(transcript);

    /* The system prompt never leaks into the transcript. */
    assert(!strstr(transcript, "you are clay"));

    /* User/assistant text survive verbatim. */
    assert(strstr(transcript, "read main.c and summarize it"));
    assert(strstr(transcript, "main.c defines the process entry point."));

    /* The tool call is recorded compactly. */
    assert(strstr(transcript, "called read("));
    assert(strstr(transcript, "main.c"));

    /* The tool result is collapsed, not included in full. */
    assert(!strstr(transcript, long_output.data));
    assert(strstr(transcript, "truncated"));
    assert(strstr(transcript, "500 bytes total"));

    free(transcript);
    clay_str_free(&long_output);
    clay_json_free(conversation);

    /* A write's arguments are a whole file: the transcript names the target
       and its size instead of replaying it. */
    {
        ClayJson *writing = clay_json_array();
        push_message(writing, "system", "you are clay");
        push_message(writing, "user", "write it");
        ClayJson *call_json = clay_json_object();
        clay_json_object_set(call_json, "id", clay_json_string("call_w"));
        ClayJson *fn = clay_json_object();
        clay_json_object_set(fn, "name", clay_json_string("write"));
        clay_json_object_set(
            fn, "arguments",
            clay_json_string("{\"path\":\"src/big.c\",\"content\":"
                             "\"int a; int b; int c;\"}"));
        clay_json_object_set(call_json, "function", fn);
        ClayJson *calls = clay_json_array();
        clay_json_array_push(calls, call_json);
        ClayJson *assistant_call = clay_json_object();
        clay_json_object_set(assistant_call, "role", clay_json_string("assistant"));
        clay_json_object_set(assistant_call, "tool_calls", calls);
        clay_json_array_push(writing, assistant_call);

        char *wrote = clay_commands_build_compact_transcript(writing);
        assert(wrote);
        assert(strstr(wrote, "called write(src/big.c"));
        assert(strstr(wrote, "bytes written"));
        assert(!strstr(wrote, "int a; int b; int c;"));
        free(wrote);
        clay_json_free(writing);
    }

    /* A conversation with nothing but the system prompt yields an empty
       transcript - the caller uses this to skip compacting. */
    ClayJson *empty = clay_json_array();
    push_message(empty, "system", "you are clay");
    char *empty_transcript = clay_commands_build_compact_transcript(empty);
    assert(empty_transcript);
    assert(*empty_transcript == '\0');
    free(empty_transcript);
    clay_json_free(empty);

    test_user_requests_kept();
    printf("compact tests passed\n");
    return 0;
}
