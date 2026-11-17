#include "context.h"

#include "clay/list.h"

#include <stdlib.h>
#include <string.h>

/* How many consecutive billed requests may report no cached input before the
   log reads as "the prefix cache stopped hitting" rather than as one cold
   start. A real session's first request is always a miss. */
#define CLAY_USAGE_MISS_STREAK 3

void clay_cmd_usage(const char *args, void *user_data) {
    (void)args;
    ClayCommands *commands = user_data;
    if (!commands->chat) {
        clay_sayc(CLAY_GRAY, "No active chat. Send a message first.");
        return;
    }
    ClayChatRequestTotals totals = {0};
    clay_chat_request_totals(commands->chat, &totals);
    if (!totals.any_usage) {
        clay_sayc(CLAY_GRAY,
                  "No requests logged for this chat yet. Numbers appear after "
                  "the first message.");
        return;
    }
    long percent = 0;
    if (totals.any_cached_known && totals.input_tokens > 0) {
        long cached = totals.cached_tokens;
        if (cached > totals.input_tokens) cached = totals.input_tokens;
        percent = (long)((double)cached * 100.0 / (double)totals.input_tokens);
    }
    clay_sayc(CLAY_CYAN, "Requests %s%ld%s  Input %s%ld%s  Output %s%ld%s",
              CLAY_BOLD, totals.requests, CLAY_RESET, CLAY_BOLD,
              totals.input_tokens, CLAY_RESET, CLAY_BOLD,
              totals.output_tokens, CLAY_RESET);
    if (totals.any_cached_known)
        clay_sayc(CLAY_GRAY, "Cache %s%ld%%%s of input (%ld cached tokens)",
                  CLAY_BOLD, percent, CLAY_RESET, totals.cached_tokens);
    else
        clay_sayc(CLAY_GRAY, "Cache unknown: the provider reported no usage.");
    if (totals.longest_miss_streak >= CLAY_USAGE_MISS_STREAK)
        clay_sayc(CLAY_YELLOW,
                  "%ld consecutive requests had no cached input. Check that "
                  "the tool list, system prompt and history prefix stay "
                  "unchanged between turns.",
                  totals.longest_miss_streak);
}
