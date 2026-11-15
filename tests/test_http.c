#include "../src/http.c"

#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static void send_all(int fd, const char *s, size_t len) {
    while (len) {
        ssize_t n = send(fd, s, len, 0);
        if (n <= 0) _exit(2);
        s += n;
        len -= (size_t)n;
    }
}

static void serve(int listener) {
    signal(SIGPIPE, SIG_IGN);
    for (int i = 0; i < 6; i++) {
        int fd = accept(listener, NULL, NULL);
        if (fd < 0) _exit(3);
        char req[2048];
        if (recv(fd, req, sizeof(req), 0) <= 0) _exit(4);
        if (i < 2) {
            const char *headers = "HTTP/1.1 100 Continue\r\nRetry-After: old\r\n\r\n"
                "HTTP/1.1 429 Too Many Requests\r\nrEtRy-AfTeR: 3\r\n"
                "RETRY-AFTER: 7\r\nX-RateLimit-Remaining-Tokens: 0\r\n"
                "Set-Cookie: secret\r\nAuthorization: secret\r\nContent-Length: 5\r\n\r\n";
            send_all(fd, headers, strlen(headers));
            send_all(fd, "a\0bcd", 5);
        } else if (i == 2) {
            const char *headers = "HTTP/1.1 302 Found\r\nRetry-After: 9\r\nContent-Length: 0\r\n\r\n";
            send_all(fd, headers, strlen(headers));
        } else if (i == 3) {
            const char *headers = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
            send_all(fd, headers, strlen(headers));
        } else if (i == 4) {
            const char *headers = "HTTP/1.1 429 Too Many Requests\r\nRetry-After: ";
            send_all(fd, headers, strlen(headers));
            char large[700]; memset(large, 'x', sizeof(large));
            send_all(fd, large, sizeof(large));
            for (int j = 0; j < 20; j++) {
                const char *h = "\r\nRetry-After: 1";
                send_all(fd, h, strlen(h));
            }
            const char *tail = "\r\nContent-Length: 70000\r\n\r\n";
            send_all(fd, tail, strlen(tail));
            memset(large, 'b', sizeof(large));
            for (int j = 0; j < 100; j++) send_all(fd, large, sizeof(large));
        } else {
            const char *headers = "HTTP/1.1 429 Too Many Requests\r\nRetry-After: 4\r\nContent-Length: 10\r\n\r\nshort";
            send_all(fd, headers, strlen(headers));
        }
        close(fd);
    }
    close(listener);
    _exit(0);
}

static int chunk(const char *data, size_t len, void *userdata) {
    (void)data;
    *(size_t *)userdata += len;
    return 0;
}

static void test_header_blocks(void) {
    ClayHttpResponse r = {0};
    ClayHttpWriteCtx ctx = {&r, NULL, NULL, 0, 0, 0};
    const char *lines[] = {
        "HTTP/1.1 200 Connection established\r\n", "Retry-After: proxy\r\n", "\r\n",
        "HTTP/1.1 302 Found\r\n", "Retry-After: redirect\r\n", "\r\n",
        "HTTP/1.1 100 Continue\r\n", "Retry-After: interim\r\n", "\r\n",
        "HTTP/2 429\r\n", "retry-after: final\r\n", "\r\n",
        "Retry-After: trailer\r\n"
    };
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++)
        assert(header_cb((char *)lines[i], 1, strlen(lines[i]), &ctx) == strlen(lines[i]));
    assert(r.status == 429 && r.rate_header_count == 1);
    assert(!strcmp(r.rate_headers[0].value, "final"));
    clay_http_response_free(&r);

    const char *status = "HTTP/1.1 429 Too Many Requests\r\n";
    assert(header_cb((char *)status, 1, strlen(status), &ctx) == strlen(status));
    char oversized[sizeof("Retry-After: ") - 1 + CLAY_HTTP_RATE_VALUE_LIMIT + 3];
    size_t prefix = strlen("Retry-After: ");
    memcpy(oversized, "Retry-After: ", prefix);
    memset(oversized + prefix, '1', CLAY_HTTP_RATE_VALUE_LIMIT + 1);
    memcpy(oversized + prefix + CLAY_HTTP_RATE_VALUE_LIMIT + 1, "\r\n", 2);
    assert(header_cb(oversized, 1, sizeof(oversized), &ctx) == sizeof(oversized));
    assert(r.rate_header_count == 0 && r.rate_headers_dropped);
    uint64_t seconds = 42;
    assert(!clay_http_retry_after(&r, &seconds) && seconds == 42);
    clay_http_response_free(&r);
}

static void test_retry_after(void) {
    ClayHttpResponse r = {0};
    uint64_t seconds = 42;
    assert(!clay_http_retry_after(&r, &seconds) && seconds == 42);
    r.rate_header_count = 1;
    r.rate_headers[0].name = "retry-after";
    struct { const char *value; int valid; uint64_t seconds; } cases[] = {
        {"0", 1, 0}, {"123", 1, 123},
        {"18446744073709551615", 1, UINT64_MAX},
        {"18446744073709551616", 0, 0}, {"", 0, 0},
        {"-1", 0, 0}, {"1x", 0, 0}, {"1, 2", 0, 0},
        {"Sun, 06 Nov 1994 08:49:37 GMT", 1, 0},
        {"Sunday, 06-Nov-94 08:49:37 GMT", 0, 0},
        {"Sun Nov  6 08:49:37 1994", 0, 0},
        {"Mon, 06 Nov 1994 08:49:37 GMT", 0, 0},
        {"Sun, 30 Feb 2020 08:49:37 GMT", 0, 0},
        {"Sun, 06 Nov 1994 08:49:37 UTC", 0, 0},
        {"Sun, 06 Nov 1994 08:49:37 GMT extra", 0, 0}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        strcpy(r.rate_headers[0].value, cases[i].value);
        seconds = 42;
        assert(clay_http_retry_after(&r, &seconds) == cases[i].valid);
        assert(seconds == (cases[i].valid ? cases[i].seconds : 42));
    }
    strcpy(r.rate_headers[0].value, "Thu, 01 Jan 2037 00:00:00 GMT");
    time_t future = curl_getdate(r.rate_headers[0].value, NULL);
    assert(future != (time_t)-1);
    if (difftime(future, time(NULL)) > 0) {
        time_t before = time(NULL);
        assert(clay_http_retry_after(&r, &seconds));
        time_t after = time(NULL);
        assert(seconds <= (uint64_t)difftime(future, before));
        assert(seconds >= (uint64_t)difftime(future, after));
    }
    strcpy(r.rate_headers[0].value, "7");
    r.rate_headers[1] = r.rate_headers[0];
    r.rate_header_count = 2;
    assert(!clay_http_retry_after(&r, &seconds));
    r.rate_header_count = 1;
    r.rate_headers_dropped = 1;
    assert(!clay_http_retry_after(&r, &seconds));
}

int main(void) {
    test_header_blocks();
    test_retry_after();
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0);
    assert(listen(fd, 4) == 0);
    socklen_t addrlen = sizeof(addr);
    assert(getsockname(fd, (struct sockaddr *)&addr, &addrlen) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) serve(fd);
    close(fd);
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%u/", (unsigned)ntohs(addr.sin_port));
    assert(clay_http_init() == 0);
    for (int i = 0; i < 6; i++) {
        size_t received = 0;
        ClayHttpRequest req = {0};
        req.url = url;
        req.timeout_seconds = 5;
        if (i == 1 || i == 4) { req.on_chunk = chunk; req.userdata = &received; }
        ClayHttpResponse r;
        int rc = clay_http_request(&req, &r);
        assert(i == 5 ? rc != 0 : rc == 0);
        if (i < 2) {
            assert(r.status == 429 && r.rate_header_count == 3 && !r.rate_headers_dropped);
            assert(!strcmp(r.rate_headers[0].name, "retry-after"));
            assert(!strcmp(r.rate_headers[0].value, "3"));
            assert(!strcmp(r.rate_headers[1].value, "7"));
            uint64_t seconds;
            assert(!clay_http_retry_after(&r, &seconds));
            assert(!strcmp(r.rate_headers[2].name, "x-ratelimit-remaining-tokens"));
            assert(r.error_body_len == 5 && !memcmp(r.error_body, "a\0bcd", 5));
            assert(i ? received == 5 && !r.body : r.body_len == 5 && !memcmp(r.body, "a\0bcd", 5));
        } else if (i == 2) {
            assert(r.status == 302 && r.rate_header_count == 1 && !strcmp(r.rate_headers[0].value, "9"));
            uint64_t seconds;
            assert(clay_http_retry_after(&r, &seconds) && seconds == 9);
            assert(!r.error_body);
        } else if (i == 3) {
            assert(r.status == 200 && r.rate_header_count == 0 && !r.error_body && r.body_len == 2);
        } else if (i == 4) {
            assert(r.status == 429 && r.rate_header_count == CLAY_HTTP_RATE_HEADER_LIMIT && r.rate_headers_dropped);
            assert(r.error_body_len == CLAY_HTTP_ERROR_BODY_LIMIT && r.error_body_truncated && received == 70000);
            uint64_t seconds = 42;
            assert(!clay_http_retry_after(&r, &seconds) && seconds == 42);
        } else {
            assert(r.status == 429 && r.error_body_len == 5 && !memcmp(r.error_body, "short", 5));
        }
        clay_http_response_free(&r);
        assert(!r.body && !r.error_body && !r.rate_header_count);
        clay_http_response_free(&r);
    }
    clay_http_cleanup();
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("http tests passed");
    return 0;
}
