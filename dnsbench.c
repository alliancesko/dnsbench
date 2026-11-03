#define _POSIX_C_SOURCE 200809L

#ifdef _WIN32
#ifdef _WIN32_WINNT
#undef _WIN32_WINNT
#endif
#define _WIN32_WINNT 0x0600
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <process.h>
typedef SOCKET dns_socket_t;
typedef WSAPOLLFD dns_pollfd_t;
#define DNS_INVALID_SOCKET INVALID_SOCKET
#define DNS_POLLIN POLLRDNORM
#define DNS_POPEN _popen
#define DNS_PCLOSE _pclose
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
typedef int dns_socket_t;
typedef struct pollfd dns_pollfd_t;
#define DNS_INVALID_SOCKET (-1)
#define DNS_POLLIN POLLIN
#define DNS_POPEN popen
#define DNS_PCLOSE pclose
#endif

#define DNS_PORT 53
#define MAX_PACKET 512
#define MAX_SERVERS 4096
#define TEST_COUNT 3

enum test_kind {
    TEST_SINGLE,
    TEST_CACHED,
    TEST_UNCACHED
};

struct server {
    char address[INET6_ADDRSTRLEN];
    char name[128];
    dns_socket_t fd;
    int family;
    uint16_t expected_id;
    int pending;
    int sent;
    int received;
    int errors;
    double sum_ms;
    double sum_sq_ms;
    double min_ms;
    double max_ms;
    double test_avg[TEST_COUNT];
    int test_received[TEST_COUNT];
    int test_sent[TEST_COUNT];
    double total_sum_ms;
    int total_received;
    int total_sent;
    int64_t started;
    int64_t deadline;
};

static void usage(FILE *out, const char *program)
{
    fprintf(out,
        "Usage: %s [options] SERVERS_FILE\n"
        "       %s [options] -c COUNTRY_CODE\n"
        "       %s [options] COUNTRY_CODE\n"
        "       %s -L\n"
        "\n"
        "Options:\n"
        "  -q NAME       query name (default: example.com)\n"
        "  -n COUNT      requests per server (default: 5)\n"
        "  -t MS         timeout per request in ms (default: 1000)\n"
        "  -T TYPE       query type: A or AAAA (default: A)\n"
        "  -p PORT       DNS port (default: 53)\n"
        "  -o FILE       write results to CSV\n"
        "  -c CODE       download and test Wirewiki country list\n"
        "  -L            list countries found on Wirewiki\n"
        "  -h            show this help\n"
        "\n"
        "File format: IP_ADDRESS [DISPLAY NAME], one server per line.\n"
        "Blank lines and text after # are ignored.\n",
        program, program, program, program);
}

static FILE *open_url(const char *url)
{
    char command[512];
    FILE *pipe;

    if (snprintf(command, sizeof(command),
                 "curl -fsSL --max-time 30 -- \"%s\"", url) >= (int)sizeof(command)) {
        fprintf(stderr, "URL is too long\n");
        return NULL;
    }
    pipe = DNS_POPEN(command, "r");
    if (!pipe)
        fprintf(stderr, "Cannot run curl; install it or use a local server file\n");
    return pipe;
}

static int close_url(FILE *pipe, const char *url)
{
    int status = DNS_PCLOSE(pipe);
#ifdef _WIN32
    if (status == 0)
        return 0;
#else
    if (status >= 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0)
        return 0;
#endif
    fprintf(stderr, "Failed to download %s\n", url);
    return -1;
}

static void html_text(char *text)
{
    struct replacement { const char *entity; char value; } replacements[] = {
        { "&amp;", '&' }, { "&quot;", '"' }, { "&#039;", '\'' },
        { "&apos;", '\'' }
    };
    size_t i;
    for (i = 0; i < sizeof(replacements) / sizeof(replacements[0]); i++) {
        char *found;
        while ((found = strstr(text, replacements[i].entity)) != NULL) {
            size_t length = strlen(replacements[i].entity);
            *found = replacements[i].value;
            memmove(found + 1, found + length, strlen(found + length) + 1);
        }
    }
}

static int list_countries(void)
{
    const char *url = "https://www.wirewiki.com/dns-servers/by-country";
    char line[1024], code[3] = "";
    FILE *file;
    int count = 0;

    file = open_url(url);
    if (!file)
        return -1;
    while (fgets(line, sizeof(line), file)) {
        char *link = strstr(line, "href=\"/dns-servers/");
        if (link) {
            link += strlen("href=\"/dns-servers/");
            if (((link[0] >= 'a' && link[0] <= 'z') ||
                 (link[0] >= 'A' && link[0] <= 'Z')) &&
                ((link[1] >= 'a' && link[1] <= 'z') ||
                 (link[1] >= 'A' && link[1] <= 'Z')) && link[2] == '"') {
                code[0] = link[0];
                code[1] = link[1];
                code[2] = '\0';
            }
        } else if (*code) {
            char *paragraph = strstr(line, "<p ");
            char *start, *end;
            if (!paragraph)
                continue;
            start = strchr(paragraph, '>');
            if (!start)
                continue;
            start++;
            end = strchr(start, '<');
            if (!end)
                continue;
            *end = '\0';
            html_text(start);
            printf("%s  %s\n", code, start);
            code[0] = '\0';
            count++;
        }
    }
    if (close_url(file, url) < 0)
        return -1;
    if (!count) {
        fprintf(stderr, "Wirewiki country page format was not recognized\n");
        return -1;
    }
    return 0;
}

static int normalize_country(const char *input, char output[3])
{
    int i;
    if (strlen(input) != 2) {
        fprintf(stderr, "Country code must contain two letters, for example: kz\n");
        return -1;
    }
    for (i = 0; i < 2; i++) {
        char value = input[i];
        if (value >= 'A' && value <= 'Z')
            value = (char)(value - 'A' + 'a');
        if (value < 'a' || value > 'z') {
            fprintf(stderr, "Invalid country code: %s\n", input);
            return -1;
        }
        output[i] = value;
    }
    output[2] = '\0';
    return 0;
}

static int64_t monotonic_ns(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (counter.QuadPart / frequency.QuadPart) * 1000000000LL +
           (counter.QuadPart % frequency.QuadPart) * 1000000000LL /
               frequency.QuadPart;
#else
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
#endif
}

static double diff_ms(int64_t end, int64_t start)
{
    return (double)(end - start) / 1000000.0;
}

static uint32_t process_id(void)
{
#ifdef _WIN32
    return (uint32_t)GetCurrentProcessId();
#else
    return (uint32_t)getpid();
#endif
}

static char *trim(char *text)
{
    char *end;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
        text++;
    end = text + strlen(text);
    while (end > text && (end[-1] == ' ' || end[-1] == '\t' ||
                          end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return text;
}

static int parse_positive(const char *text, int min, int max, const char *what)
{
    char *end = NULL;
    long value;
    errno = 0;
    value = strtol(text, &end, 10);
    if (errno || !end || *end || value < min || value > max) {
        fprintf(stderr, "Invalid %s: %s (expected %d..%d)\n",
                what, text, min, max);
        return -1;
    }
    return (int)value;
}

static int parse_servers(FILE *file, const char *source,
                         struct server **out, size_t *out_count)
{
    struct server *servers = NULL;
    size_t count = 0, capacity = 0;
    char line[512];
    unsigned long line_number = 0;

    while (fgets(line, sizeof(line), file)) {
        struct server item;
        struct in_addr v4;
        struct in6_addr v6;
        char *body, *comment, *space, *label;

        line_number++;
        comment = strchr(line, '#');
        if (comment)
            *comment = '\0';
        body = trim(line);
        if (!*body)
            continue;

        space = body;
        while (*space && *space != ' ' && *space != '\t')
            space++;
        if (*space) {
            *space++ = '\0';
            label = trim(space);
        } else {
            label = body;
        }

        memset(&item, 0, sizeof(item));
        item.fd = DNS_INVALID_SOCKET;
        item.min_ms = 1.0 / 0.0;
        snprintf(item.address, sizeof(item.address), "%s", body);
        snprintf(item.name, sizeof(item.name), "%s", label);

        if (inet_pton(AF_INET, body, &v4) == 1) {
            item.family = AF_INET;
        } else if (inet_pton(AF_INET6, body, &v6) == 1) {
            item.family = AF_INET6;
        } else {
            fprintf(stderr, "%s:%lu: invalid IP address: %s\n",
                    source, line_number, body);
            goto fail;
        }

        if (count == MAX_SERVERS) {
            fprintf(stderr, "Too many servers (maximum %d)\n", MAX_SERVERS);
            goto fail;
        }
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 16;
            struct server *grown = realloc(servers, next * sizeof(*grown));
            if (!grown) {
                perror("realloc");
                goto fail;
            }
            servers = grown;
            capacity = next;
        }
        servers[count++] = item;
    }

    if (ferror(file)) {
        fprintf(stderr, "%s: read error\n", source);
        goto fail;
    }
    if (!count) {
        fprintf(stderr, "%s: no servers found\n", source);
        free(servers);
        return -1;
    }
    *out = servers;
    *out_count = count;
    return 0;

fail:
    free(servers);
    return -1;
}

static int load_servers_file(const char *path, struct server **out,
                             size_t *out_count)
{
    FILE *file = fopen(path, "r");
    int result;
    if (!file) {
        perror(path);
        return -1;
    }
    result = parse_servers(file, path, out, out_count);
    if (fclose(file) != 0 && result == 0) {
        perror(path);
        free(*out);
        *out = NULL;
        *out_count = 0;
        return -1;
    }
    return result;
}

static int load_servers_url(const char *url, struct server **out,
                            size_t *out_count)
{
    FILE *pipe = open_url(url);
    int result, close_result;
    if (!pipe)
        return -1;
    result = parse_servers(pipe, url, out, out_count);
    close_result = close_url(pipe, url);
    if (result == 0 && close_result < 0) {
        free(*out);
        *out = NULL;
        *out_count = 0;
        return -1;
    }
    return result;
}

static size_t socket_batch_size(size_t total)
{
#ifdef _WIN32
    size_t limit = 256;
#else
    long open_max = sysconf(_SC_OPEN_MAX);
    size_t limit = 128;

    if (open_max > 32)
        limit = (size_t)(open_max - 16);
    else if (open_max > 0)
        limit = 1;
    if (limit > 256)
        limit = 256;
#endif
    if (limit > total)
        limit = total;
    return limit ? limit : 1;
}

static int socket_last_error(void)
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

static int socket_would_block(int error)
{
#ifdef _WIN32
    return error == WSAEWOULDBLOCK;
#else
    return error == EAGAIN || error == EWOULDBLOCK;
#endif
}

static void print_socket_error(const char *address, const char *operation)
{
#ifdef _WIN32
    fprintf(stderr, "%s: %s: Winsock error %d\n",
            address, operation, socket_last_error());
#else
    fprintf(stderr, "%s: %s: %s\n",
            address, operation, strerror(socket_last_error()));
#endif
}

static void close_socket(dns_socket_t socket_value)
{
#ifdef _WIN32
    closesocket(socket_value);
#else
    close(socket_value);
#endif
}

static int set_nonblocking(dns_socket_t socket_value)
{
#ifdef _WIN32
    u_long enabled = 1;
    return ioctlsocket(socket_value, FIONBIO, &enabled) == 0 ? 0 : -1;
#else
    int flags = fcntl(socket_value, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(socket_value, F_SETFL, flags | O_NONBLOCK);
#endif
}

static int socket_platform_init(void)
{
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        fprintf(stderr, "WSAStartup failed\n");
        return -1;
    }
#endif
    return 0;
}

static void socket_platform_cleanup(void)
{
#ifdef _WIN32
    WSACleanup();
#endif
}

static void close_server_sockets(struct server *servers, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        if (servers[i].fd != DNS_INVALID_SOCKET)
            close_socket(servers[i].fd);
        servers[i].fd = DNS_INVALID_SOCKET;
    }
}

static int open_server_sockets(struct server *servers, size_t count, int port)
{
    size_t i;

    for (i = 0; i < count; i++) {
        struct server *server = &servers[i];
        struct sockaddr_storage address;
        socklen_t address_len;

        memset(&address, 0, sizeof(address));
        if (server->family == AF_INET) {
            struct sockaddr_in *v4 = (struct sockaddr_in *)&address;
            v4->sin_family = AF_INET;
            v4->sin_port = htons((uint16_t)port);
            inet_pton(AF_INET, server->address, &v4->sin_addr);
            address_len = sizeof(*v4);
        } else {
            struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)&address;
            v6->sin6_family = AF_INET6;
            v6->sin6_port = htons((uint16_t)port);
            inet_pton(AF_INET6, server->address, &v6->sin6_addr);
            address_len = sizeof(*v6);
        }

        server->fd = socket(server->family, SOCK_DGRAM, 0);
        if (server->fd == DNS_INVALID_SOCKET) {
            print_socket_error(server->address, "socket");
            close_server_sockets(servers, i);
            return -1;
        }
        if (set_nonblocking(server->fd) < 0) {
            print_socket_error(server->address, "nonblocking mode");
            close_server_sockets(servers, i + 1);
            return -1;
        }
        if (connect(server->fd, (struct sockaddr *)&address, address_len) != 0) {
            print_socket_error(server->address, "connect");
            close_server_sockets(servers, i + 1);
            return -1;
        }
    }
    return 0;
}

static int build_query(unsigned char *packet, size_t capacity, const char *name,
                       uint16_t id, uint16_t qtype)
{
    unsigned char *cursor = packet;
    const char *label = name;
    const char *dot;

    if (capacity < 17)
        return -1;
    memset(packet, 0, capacity);
    packet[0] = (unsigned char)(id >> 8);
    packet[1] = (unsigned char)(id & 0xff);
    packet[2] = 0x01; /* recursion desired */
    packet[5] = 0x01; /* one question */
    cursor += 12;

    while (*label) {
        size_t length;
        dot = strchr(label, '.');
        length = dot ? (size_t)(dot - label) : strlen(label);
        if (!length || length > 63 || (size_t)(cursor - packet) + length + 5 > capacity)
            return -1;
        *cursor++ = (unsigned char)length;
        memcpy(cursor, label, length);
        cursor += length;
        if (!dot)
            break;
        label = dot + 1;
        if (!*label) /* allow one trailing dot */
            break;
    }
    *cursor++ = 0;
    *cursor++ = (unsigned char)(qtype >> 8);
    *cursor++ = (unsigned char)(qtype & 0xff);
    *cursor++ = 0;
    *cursor++ = 1; /* IN */
    return (int)(cursor - packet);
}

static int valid_response(const unsigned char *packet, int length, uint16_t id)
{
    uint16_t response_id;
    if (length < 12)
        return 0;
    response_id = (uint16_t)((packet[0] << 8) | packet[1]);
    return response_id == id && (packet[2] & 0x80) != 0;
}

static void record_success(struct server *server, double elapsed)
{
    server->received++;
    server->sum_ms += elapsed;
    server->sum_sq_ms += elapsed * elapsed;
    if (elapsed < server->min_ms)
        server->min_ms = elapsed;
    if (elapsed > server->max_ms)
        server->max_ms = elapsed;
}

static void reset_measurements(struct server *servers, size_t count)
{
    size_t i;
    for (i = 0; i < count; i++) {
        servers[i].sent = 0;
        servers[i].received = 0;
        servers[i].errors = 0;
        servers[i].sum_ms = 0.0;
        servers[i].sum_sq_ms = 0.0;
        servers[i].min_ms = 1.0 / 0.0;
        servers[i].max_ms = 0.0;
    }
}

static void save_test(struct server *servers, size_t count, enum test_kind test)
{
    size_t i;
    for (i = 0; i < count; i++) {
        struct server *server = &servers[i];
        server->test_sent[test] = server->sent;
        server->test_received[test] = server->received;
        server->test_avg[test] = server->received
            ? server->sum_ms / server->received : -1.0;
        server->total_sent += server->sent;
        server->total_received += server->received;
        server->total_sum_ms += server->sum_ms;
    }
}

static uint16_t next_id(void)
{
    static uint32_t state;
    if (!state) {
        state = (uint32_t)monotonic_ns() ^ process_id();
    }
    state = state * 1103515245u + 12345u;
    return (uint16_t)(state >> 16);
}

static int poll_sockets(dns_pollfd_t *pollfds, size_t count, int timeout_ms)
{
#ifdef _WIN32
    return WSAPoll(pollfds, (ULONG)count, timeout_ms);
#else
    return poll(pollfds, count, timeout_ms);
#endif
}

static int poll_was_interrupted(void)
{
#ifdef _WIN32
    return WSAGetLastError() == WSAEINTR;
#else
    return errno == EINTR;
#endif
}

static void run_round(struct server *servers, size_t count, const char *query_name,
                      uint16_t qtype, int timeout_ms)
{
    dns_pollfd_t *pollfds = calloc(count, sizeof(*pollfds));
    unsigned char packet[MAX_PACKET];
    size_t pending = 0, i;

    if (!pollfds) {
        perror("calloc");
        exit(1);
    }

    for (i = 0; i < count; i++) {
        struct server *server = &servers[i];
        int packet_len;
        int written;

        server->expected_id = next_id();
        packet_len = build_query(packet, sizeof(packet), query_name,
                                 server->expected_id, qtype);
        if (packet_len < 0) {
            fprintf(stderr, "Invalid query name: %s\n", query_name);
            free(pollfds);
            exit(1);
        }
        server->started = monotonic_ns();
        server->deadline = server->started + (int64_t)timeout_ms * 1000000LL;
        written = send(server->fd, (const char *)packet, packet_len, 0);
        server->sent++;
        if (written != packet_len) {
            server->errors++;
            server->pending = 0;
        } else {
            server->pending = 1;
            pending++;
        }
        pollfds[i].fd = server->fd;
        pollfds[i].events = DNS_POLLIN;
    }

    while (pending) {
        int64_t now, earliest = 0;
        int wait_ms, ready;
        int have_earliest = 0;

        now = monotonic_ns();
        for (i = 0; i < count; i++) {
            if (servers[i].pending &&
                (!have_earliest || servers[i].deadline < earliest)) {
                earliest = servers[i].deadline;
                have_earliest = 1;
            }
        }
        if (!have_earliest)
            break;
        wait_ms = (int)diff_ms(earliest, now);
        if (wait_ms < 0)
            wait_ms = 0;
        else if (wait_ms < timeout_ms)
            wait_ms++; /* poll uses integer milliseconds; round upward */

        ready = poll_sockets(pollfds, count, wait_ms);
        now = monotonic_ns();
        if (ready < 0 && !poll_was_interrupted()) {
            print_socket_error("poll", "wait");
            break;
        }

        for (i = 0; i < count; i++) {
            struct server *server = &servers[i];
            if (!server->pending)
                continue;
            if (pollfds[i].revents & (DNS_POLLIN | POLLERR | POLLHUP)) {
                unsigned char response[MAX_PACKET];
                int length = recv(server->fd, (char *)response, (int)sizeof(response), 0);
                if (length >= 0 && valid_response(response, length, server->expected_id)) {
                    record_success(server, diff_ms(now, server->started));
                    server->pending = 0;
                    pending--;
                } else if (length < 0 && !socket_would_block(socket_last_error())) {
                    server->errors++;
                    server->pending = 0;
                    pending--;
                }
            }
            if (server->pending && now >= server->deadline) {
                server->pending = 0;
                pending--;
            }
        }
    }
    free(pollfds);
}

static int compare_servers(const void *left, const void *right)
{
    const struct server *a = left, *b = right;
    double avg_a, avg_b;
    if (!!a->received != !!b->received)
        return a->received ? -1 : 1;
    if (a->received) {
        avg_a = a->sum_ms / a->received;
        avg_b = b->sum_ms / b->received;
        if (avg_a < avg_b)
            return -1;
        if (avg_a > avg_b)
            return 1;
    }
    return strcmp(a->address, b->address);
}

static void format_value(char *buffer, size_t size, double value)
{
    if (value < 0.0)
        snprintf(buffer, size, "-");
    else
        snprintf(buffer, size, "%.2f", value);
}

static void print_suite_results(struct server *servers, size_t count)
{
    size_t i;
    int address_width = (int)strlen("DNS server");

    for (i = 0; i < count; i++) {
        int length = (int)strlen(servers[i].address);
        if (length > address_width)
            address_width = length;
    }
    printf("%-4s | %-*s | %9s | %9s | %9s | %9s | %9s\n",
           "Rank", address_width, "DNS server",
           "Single", "Cached", "Uncached", "Average", "OK/All");
    for (i = 0; i < count; i++) {
        struct server *server = &servers[i];
        char rank[32], single[16], cached[16], uncached[16], average[16];

        if (server->total_received)
            snprintf(rank, sizeof(rank), "%lu", (unsigned long)(i + 1));
        else
            snprintf(rank, sizeof(rank), "-");
        format_value(single, sizeof(single), server->test_avg[TEST_SINGLE]);
        format_value(cached, sizeof(cached), server->test_avg[TEST_CACHED]);
        format_value(uncached, sizeof(uncached), server->test_avg[TEST_UNCACHED]);
        format_value(average, sizeof(average), server->total_received
                     ? server->total_sum_ms / server->total_received : -1.0);
        printf("%-4s | %-*s | %9s | %9s | %9s | %9s | %4d/%-4d\n",
               rank, address_width, server->address,
               single, cached, uncached, average,
               server->total_received, server->total_sent);
    }
}

static int write_csv(const char *path, struct server *servers, size_t count)
{
    FILE *file = fopen(path, "w");
    size_t i;

    if (!file) {
        perror(path);
        return -1;
    }
    fprintf(file, "rank,dns_server,single_ms,cached_ms,uncached_ms,average_ms,successful,total\n");
    for (i = 0; i < count; i++) {
        struct server *server = &servers[i];
        if (server->total_received)
            fprintf(file, "%lu,%s,", (unsigned long)(i + 1), server->address);
        else
            fprintf(file, ",%s,", server->address);

        if (server->test_avg[TEST_SINGLE] >= 0.0)
            fprintf(file, "%.2f", server->test_avg[TEST_SINGLE]);
        fputc(',', file);
        if (server->test_avg[TEST_CACHED] >= 0.0)
            fprintf(file, "%.2f", server->test_avg[TEST_CACHED]);
        fputc(',', file);
        if (server->test_avg[TEST_UNCACHED] >= 0.0)
            fprintf(file, "%.2f", server->test_avg[TEST_UNCACHED]);
        fputc(',', file);
        if (server->total_received)
            fprintf(file, "%.2f", server->total_sum_ms / server->total_received);
        fprintf(file, ",%d,%d\n", server->total_received, server->total_sent);
    }
    if (fclose(file) != 0) {
        perror(path);
        return -1;
    }
    return 0;
}

static int run_suite(struct server *servers, size_t count, const char *query_name,
                     uint16_t qtype, int series_count, int timeout_ms, int port)
{
    uint64_t seed;
    size_t offset, batch_size = socket_batch_size(count);
    int i;

    printf("  1/3  Single query\n");
    for (offset = 0; offset < count; offset += batch_size) {
        size_t current = count - offset < batch_size ? count - offset : batch_size;
        if (open_server_sockets(servers + offset, current, port) < 0)
            return -1;
        reset_measurements(servers + offset, current);
        run_round(servers + offset, current, query_name, qtype, timeout_ms);
        save_test(servers + offset, current, TEST_SINGLE);
        close_server_sockets(servers + offset, current);
    }

    printf("  2/3  Cached queries (%d per server)\n", series_count);
    for (offset = 0; offset < count; offset += batch_size) {
        size_t current = count - offset < batch_size ? count - offset : batch_size;
        if (open_server_sockets(servers + offset, current, port) < 0)
            return -1;
        reset_measurements(servers + offset, current);
        run_round(servers + offset, current, query_name, qtype, timeout_ms);
        reset_measurements(servers + offset, current);
        for (i = 0; i < series_count; i++)
            run_round(servers + offset, current, query_name, qtype, timeout_ms);
        save_test(servers + offset, current, TEST_CACHED);
        close_server_sockets(servers + offset, current);
    }

    printf("  3/3  Uncached queries (%d per server)\n", series_count);
    seed = (uint64_t)monotonic_ns() ^ process_id();
    for (offset = 0; offset < count; offset += batch_size) {
        size_t current = count - offset < batch_size ? count - offset : batch_size;
        if (open_server_sockets(servers + offset, current, port) < 0)
            return -1;
        reset_measurements(servers + offset, current);
        for (i = 0; i < series_count; i++) {
            char unique_name[512];
            snprintf(unique_name, sizeof(unique_name), "dnsbench-%08x%08x-%x-%d.%s",
                     (unsigned int)(seed >> 32), (unsigned int)seed,
                     process_id(), i, query_name);
            run_round(servers + offset, current, unique_name, qtype, timeout_ms);
        }
        save_test(servers + offset, current, TEST_UNCACHED);
        close_server_sockets(servers + offset, current);
    }

    for (i = 0; i < (int)count; i++) {
        servers[i].sent = servers[i].total_sent;
        servers[i].received = servers[i].total_received;
        servers[i].sum_ms = servers[i].total_sum_ms;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *query_name = "example.com";
    const char *file_path = NULL;
    const char *positional = NULL;
    const char *country_option = NULL;
    const char *csv_output = NULL;
    char country[3], country_url[128];
    struct server *servers = NULL;
    size_t count = 0, i;
    int requests = 5, timeout_ms = 1000, port = DNS_PORT;
    uint16_t qtype = 1;
    int option, successes = 0, show_countries = 0, csv_error = 0;
    int options_done = 0;

    for (option = 1; option < argc; option++) {
        const char *argument = argv[option];
        const char *value = NULL;
        int parsed;

        if (!options_done && strcmp(argument, "--") == 0) {
            options_done = 1;
            continue;
        }
        if (options_done || argument[0] != '-' || argument[1] == '\0') {
            if (positional) {
                fprintf(stderr, "Only one country code or server file is allowed\n");
                return 1;
            }
            positional = argument;
            continue;
        }
        if (strcmp(argument, "-h") == 0) {
            usage(stdout, argv[0]);
            return 0;
        }
        if (strcmp(argument, "-L") == 0) {
            show_countries = 1;
            continue;
        }
        if (!strchr("qntTpoc", argument[1]) || argument[1] == '\0') {
            fprintf(stderr, "Unknown option: %s\n", argument);
            usage(stderr, argv[0]);
            return 1;
        }
        if (argument[2] != '\0')
            value = argument + 2;
        else if (++option < argc)
            value = argv[option];
        else {
            fprintf(stderr, "Option -%c requires a value\n", argument[1]);
            return 1;
        }

        switch (argument[1]) {
        case 'q': query_name = value; break;
        case 'n':
            parsed = parse_positive(value, 1, 1000, "request count");
            if (parsed < 0) return 1;
            requests = parsed;
            break;
        case 't':
            parsed = parse_positive(value, 1, 60000, "timeout");
            if (parsed < 0) return 1;
            timeout_ms = parsed;
            break;
        case 'p':
            parsed = parse_positive(value, 1, 65535, "port");
            if (parsed < 0) return 1;
            port = parsed;
            break;
        case 'o': csv_output = value; break;
        case 'c': country_option = value; break;
        case 'T':
            if (strcmp(value, "A") == 0) qtype = 1;
            else if (strcmp(value, "AAAA") == 0) qtype = 28;
            else {
                fprintf(stderr, "Unsupported query type: %s (use A or AAAA)\n", value);
                return 1;
            }
            break;
        }
    }
    if (show_countries) {
        if (country_option || positional) {
            usage(stderr, argv[0]);
            return 1;
        }
        return list_countries() == 0 ? 0 : 1;
    }
    if (country_option && positional) {
        fprintf(stderr, "Use either -c CODE or SERVERS_FILE, not both\n");
        return 1;
    }
    if (!country_option && !positional) {
        usage(stderr, argv[0]);
        return 1;
    }
    if (!country_option && strlen(positional) == 2)
        country_option = positional;

    if (country_option) {
        if (normalize_country(country_option, country) < 0)
            return 1;
        snprintf(country_url, sizeof(country_url),
                 "https://www.wirewiki.com/dns-servers/%s.txt", country);
        if (load_servers_url(country_url, &servers, &count) < 0)
            return 1;
    } else {
        file_path = positional;
        if (load_servers_file(file_path, &servers, &count) < 0)
            return 1;
    }
    if (socket_platform_init() < 0) {
        free(servers);
        return 1;
    }
    atexit(socket_platform_cleanup);
    printf("Testing %lu DNS servers (%s %s, timeout %d ms)\n",
           (unsigned long)count, qtype == 1 ? "A" : "AAAA", query_name, timeout_ms);
    if (run_suite(servers, count, query_name, qtype, requests, timeout_ms, port) < 0) {
        free(servers);
        return 1;
    }
    printf("\nTimes are in milliseconds.\n\n");

    qsort(servers, count, sizeof(*servers), compare_servers);
    print_suite_results(servers, count);
    if (csv_output) {
        if (write_csv(csv_output, servers, count) == 0)
            printf("\nCSV saved to %s\n", csv_output);
        else
            csv_error = 1;
    }
    for (i = 0; i < count; i++) {
        if (servers[i].received)
            successes++;
    }
    free(servers);
    if (csv_error)
        return 1;
    return successes ? 0 : 2;
}
