#define _GNU_SOURCE
#include "clay/http.h"
#include "clay/providers/openai.h"
#include <arpa/inet.h>
#include <assert.h>
#include <dirent.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int64_t now;
static int cancel_wait, executed, tokens, errors;
int64_t __wrap_clay_provider_retry_now_ms(void *p) { (void)p; return now; }
void __wrap_clay_provider_retry_sleep_ms(unsigned ms, void *p) { (void)p; now += ms; }
unsigned __wrap_clay_provider_retry_jitter(void) { return 500; }
static int abort_wait(void *p) { (void)p; return cancel_wait && now >= 200; }
static ClayJson *run_tool(const ClayJson *args, void *p) {
    (void)args; (void)p; executed++; return clay_json_object();
}
static void token(const char *s, void *p) { (void)p; assert(!strcmp(s, "done")); tokens++; }
static void error(long status, const char *body, void *p) {
    (void)p; assert(status == 429 && !strcmp(body, "limited")); errors++;
}
static int fd_count(void) {
    DIR *d = opendir("/proc/self/fd");
#ifdef __linux__
    assert(d);
#else
    if (!d) return -1;
#endif
    int n = 0;
    while (readdir(d)) n++;
    closedir(d); return n;
}
static void send_all(int fd, const char *s, size_t n) {
    while (n) { ssize_t k = send(fd, s, n, 0); if (k <= 0) _exit(2); s += k; n -= (size_t)k; }
}
static void serve(int listener, int mode) {
    alarm(10); signal(SIGPIPE, SIG_IGN);
    int count = mode == 0 ? 5 : 1;
    for (int i = 0; i < count; i++) {
        int fd = accept(listener, NULL, NULL); if (fd < 0) _exit(3);
        char req[32768]; size_t used = 0, total = 0;
        while (!total || used < total) {
            ssize_t n = recv(fd, req + used, sizeof(req) - used - 1, 0);
            if (n <= 0) _exit(4);
            used += (size_t)n; req[used] = 0;
            char *end = strstr(req, "\r\n\r\n");
            if (end && !total) {
                char *length = strcasestr(req, "Content-Length: ");
                if (!length || length > end) _exit(5);
                total = (size_t)(end + 4 - req) + strtoul(length + 16, NULL, 10);
                if (total >= sizeof(req)) _exit(6);
            }
        }
        if (mode == 0 && i >= 2 && !strstr(req, "tool_call_id")) _exit(7);
        const char *payload;
        int limited = mode || i == 0 || i == 2 || i == 3;
        if (limited) payload = "limited";
        else if (i == 1) payload = "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"x\",\"function\":{\"name\":\"test\",\"arguments\":\"{}\"}}]}}]}\n\n";
        else payload = "data: {\"choices\":[{\"delta\":{\"content\":\"done\"}}]}\n\n";
        char header[256];
        int n = snprintf(header, sizeof(header), "HTTP/1.1 %s\r\nContent-Length: %zu\r\nConnection: close\r\n%s\r\n", limited ? "429 Too Many Requests" : "200 OK", strlen(payload), mode == 2 ? "Retry-After: 31\r\n" : "");
        send_all(fd, header, (size_t)n);
        for (size_t j = 0; j < strlen(payload); j++) send_all(fd, payload + j, 1);
        close(fd);
    }
    close(listener); _exit(0);
}
static void scenario(int mode) {
    int listener = socket(AF_INET, SOCK_STREAM, 0); assert(listener >= 0);
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(!bind(listener, (struct sockaddr *)&a, sizeof(a)) && !listen(listener, 4));
    socklen_t len = sizeof(a); assert(!getsockname(listener, (struct sockaddr *)&a, &len));
    pid_t child = fork(); assert(child >= 0); if (!child) serve(listener, mode);
    close(listener);
    char url[128]; snprintf(url, sizeof(url), "http://127.0.0.1:%u/v1", ntohs(a.sin_port));
    ClayOpenAI *client = clay_openai_create(url, "offline", "model"); assert(client);
    ClayJson *messages = clay_json_array();
    ClayTool tool = {.name = "test", .fn = run_tool};
    ClayOpenAICallbacks cb = {.on_token = token, .on_error = error, .should_abort = abort_wait};
    now = 0; cancel_wait = mode == 1; executed = tokens = errors = 0;
    int rc = clay_openai_run(client, messages, &tool, 1, 2, &cb);
    if (!mode) { assert(rc == 0 && executed == 1 && tokens == 1 && !errors); assert(now == 5500); }
    else if (mode == 1) { assert(rc == 1 && now == 200 && !executed && !errors); }
    else { assert(rc == -1 && now == 0 && !executed && errors == 1); }
    clay_json_free(messages); clay_openai_destroy(client);
    int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
int main(void) {
    assert(!clay_http_init()); int before = fd_count();
    for (int i = 0; i < 3; i++) { for (int mode = 0; mode < 3; mode++) { scenario(mode); assert(fd_count() == before); } }
    clay_http_cleanup(); puts("retry e2e tests passed"); return 0;
}
