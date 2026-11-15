#ifndef CLAY_COMMANDS_H
#define CLAY_COMMANDS_H

typedef struct ClayApp ClayApp;
typedef struct ClayCommands ClayCommands;

/* The owner must serialize public calls and wait for each to return before
   destroying the session once; internal subagent workers finish before return.
   Neither concurrent reconfiguration nor concurrent destruction is supported. */
ClayCommands *clay_commands_create(ClayApp *app);
void clay_commands_destroy(ClayCommands *commands);
void clay_commands_register(ClayCommands *commands);
int clay_commands_running(const ClayCommands *commands);
int clay_commands_has_provider(const ClayCommands *commands);
int clay_commands_connect(ClayCommands *commands);
int clay_commands_run_message(ClayCommands *commands, const char *input);
void clay_commands_print_session_summary(const ClayCommands *commands);

#endif /* CLAY_COMMANDS_H */
