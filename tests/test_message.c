/* Run after make build:
   cc -std=gnu11 -Wall -Wextra -Iinclude -Isrc -c tests/test_message.c -o /tmp/clay-test-message.o
   cc $(find build -name '*.o' ! -path 'build/tests*' ! -path 'build/main.o' ! -path 'build/commands/message.o') /tmp/clay-test-message.o -o /tmp/clay-test-message -lcurl -lpthread
   /tmp/clay-test-message
*/
#define clay_commands_run_completion test_run_completion
#include "../src/commands/message.c"
#undef clay_commands_run_completion

#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>

static int completion_result;
static int fail_save;

int test_run_completion(ClayCommands *commands, ClayJson *messages,
                        const ClayToolSet *tools, int max_rounds,
                        const char *cache_key, const ClayOpenAICallbacks *callbacks) {
  (void)messages; (void)tools; (void)max_rounds; (void)cache_key; (void)callbacks;
  if (fail_save) {
    ClayStr path;
    clay_str_init(&path);
    clay_str_printf(&path, "%s/.clay/chats/%s/chat.json", getenv("HOME"),
                    clay_chat_id(commands->chat));
    assert(remove(path.data) == 0);
    assert(mkdir(path.data, 0700) == 0);
    clay_str_free(&path);
  }
  return completion_result;
}

int main(void) {
  char home[] = "/tmp/clay-message-XXXXXX";
  assert(mkdtemp(home));
  assert(setenv("HOME", home, 1) == 0);
  ClayApp *app = clay_app_create();
  ClayCommands *commands = clay_commands_create(app);
  static const ClayProviderType type = {"test", "test", ""};
  ClayConnectedProvider provider = {.type = &type};
  *(ClayConnectedProvider *)clay_array_push(&commands->providers) = provider;
  commands->selected_provider = strdup("test");
  commands->selected_model = strdup("test");
  FILE *output = tmpfile();
  assert(output);
  int original_stdout = dup(STDOUT_FILENO);
  assert(original_stdout >= 0);
  fflush(stdout);
  assert(dup2(fileno(output), STDOUT_FILENO) >= 0);

  int results[] = {0, CLAY_RUN_OUT_OF_ROUNDS, 1, -1};
  for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
    for (fail_save = 0; fail_save <= 1; fail_save++) {
      commands->chat = clay_chat_create("test");
      assert(commands->chat);
      completion_result = results[i];
      off_t start = lseek(STDOUT_FILENO, 0, SEEK_CUR);
      clay_commands_run_message(commands, "hello");
      fflush(stdout);
      off_t end = lseek(STDOUT_FILENO, 0, SEEK_CUR);
      assert(start >= 0 && end >= start);
      char *text = calloc((size_t)(end - start) + 1, 1);
      assert(text);
      assert(pread(fileno(output), text, (size_t)(end - start), start) == end - start);
      assert((strstr(text, "Could not save the chat journal.") != NULL) == fail_save);
      free(text);
      ClayStr path;
      clay_str_init(&path);
      clay_str_printf(&path, "%s/.clay/chats/%s/chat.json", home,
                      clay_chat_id(commands->chat));
      assert((fail_save ? rmdir(path.data) : remove(path.data)) == 0);
      clay_str_clear(&path);
      clay_str_printf(&path, "%s/.clay/chats/%s", home,
                      clay_chat_id(commands->chat));
      assert(rmdir(path.data) == 0);
      clay_str_free(&path);
      clay_chat_destroy(commands->chat);
      commands->chat = NULL;
    }
  }
  fflush(stdout);
  assert(dup2(original_stdout, STDOUT_FILENO) >= 0);
  close(original_stdout);
  fclose(output);
  clay_commands_destroy(commands);
  clay_app_destroy(app);
  ClayStr path;
  clay_str_init(&path);
  clay_str_printf(&path, "%s/.clay/chats/index.json", home);
  remove(path.data);
  clay_str_clear(&path);
  clay_str_printf(&path, "%s/.clay/chats", home);
  assert(rmdir(path.data) == 0);
  clay_str_clear(&path);
  clay_str_printf(&path, "%s/.clay/system_prompt.json", home);
  remove(path.data);
  clay_str_clear(&path);
  clay_str_printf(&path, "%s/.clay", home);
  assert(rmdir(path.data) == 0);
  assert(rmdir(home) == 0);
  clay_str_free(&path);
  return 0;
}
