/* adbnetd.c - adbd-over-TCP relay for Xiaoya HOME speaker (RK3308, Buildroot)
 *
 * Why this exists:
 *   The stock adbd (/usr/sbin/adbd, android-tools) only ever binds the USB
 *   FunctionFS transport. "adb tcpip 5555" prints "restarting in TCP mode"
 *   and then exits without ever listening, because the expected
 *   persist.adb.tcp.port property backend (getprop / libcutils property
 *   service) does not exist on this Buildroot image.
 *
 *   BUT adbd does open a local ADB transport socket on 127.0.0.1:5037
 *   (inode matches adbd's own fd). So relaying a LAN port to that loopback
 *   socket gives a real network adb endpoint with zero protocol knowledge.
 *
 * Usage:  adbnetd [-l listen_port] [-d dest_ip] [-p dest_port] [-v]
 * Default: listen 5555 -> 127.0.0.1:5037
 *
 * Build (zig, same toolchain as xbridge):
 *   zig cc -target aarch64-linux-musl -O2 -static -s -o adbnetd adbnetd.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <poll.h>

static int g_listen_port = 5555;
static int g_dst_port = 5037;
static char g_dst_ip[64] = "127.0.0.1";
static int g_verbose = 0;

static void logmsg(const char *fmt, ...)
{
    va_list ap;
    char ts[32];
    time_t now = time(NULL);
    struct tm tmnow;

    localtime_r(&now, &tmnow);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmnow);

    fprintf(stderr, "[%s] ", ts);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static int tcp_connect(const char *ip, int port)
{
    struct sockaddr_in sa;
    int fd;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &sa.sin_addr) != 1)
        return -1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int tcp_listen(int port)
{
    struct sockaddr_in sa;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int on = 1;

    if (fd < 0)
        return -1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* pump both directions until either side closes */
static void pump(int a, int b)
{
    struct pollfd p[2];
    static char buf[8192];
    long total = 0;

    for (;;) {
        p[0].fd = a;
        p[0].events = POLLIN;
        p[1].fd = b;
        p[1].events = POLLIN;

        int r = poll(p, 2, -1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return;
        }

        int i;
        for (i = 0; i < 2; i++) {
            if (!(p[i].revents & (POLLIN | POLLERR | POLLHUP)))
                continue;
            ssize_t n = read(p[i].fd, buf, sizeof(buf));
            if (n <= 0)
                return;

            if (g_verbose && total < 4096) {
                /* hexdump first bytes of each direction for debugging */
                size_t k;
                fprintf(stderr, "[%s %ldB] ", (i == 0) ? "C->D" : "D->C", (long)n);
                for (k = 0; k < (size_t)n && k < 48; k++)
                    fprintf(stderr, "%02x ", (unsigned char)buf[k]);
                fprintf(stderr, "| ");
                for (k = 0; k < (size_t)n && k < 48; k++) {
                    unsigned char c = buf[k];
                    fputc((c >= 32 && c < 127) ? c : '.', stderr);
                }
                fputc('\n', stderr);
                fflush(stderr);
            }
            total += n;

            int out = (i == 0) ? b : a;
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = write(out, buf + off, (size_t)(n - off));
                if (w <= 0)
                    return;
                off += w;
            }
        }
    }
}

int main(int argc, char **argv)
{
    int i;

    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_DFL);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l") && i + 1 < argc) g_listen_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) snprintf(g_dst_ip, sizeof(g_dst_ip), "%s", argv[++i]);
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) g_dst_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "-h")) {
            fprintf(stderr,
                "adbnetd - relay TCP -> adbd local transport\n"
                "  -l <port>  listen port (default 5555)\n"
                "  -d <ip>    destination ip (default 127.0.0.1)\n"
                "  -p <port>  destination port (default 5037)\n"
                "  -v         verbose\n");
            return 0;
        }
    }

    int lfd = tcp_listen(g_listen_port);
    if (lfd < 0) {
        logmsg("cannot listen on %d: %s", g_listen_port, strerror(errno));
        return 1;
    }
    logmsg("adbnetd ready: 0.0.0.0:%d -> %s:%d", g_listen_port, g_dst_ip, g_dst_port);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
        if (cfd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            logmsg("accept error: %s", strerror(errno));
            sleep(1);
            continue;
        }

        char ipbuf[64];
        inet_ntop(AF_INET, &peer.sin_addr, ipbuf, sizeof(ipbuf));
        if (g_verbose)
            logmsg("connect from %s", ipbuf);

        pid_t pid = fork();
        if (pid < 0) {
            close(cfd);
            continue;
        }
        if (pid > 0) {
            close(cfd);
            continue;
        }

        /* child */
        close(lfd);
        int dfd = tcp_connect(g_dst_ip, g_dst_port);
        if (dfd < 0) {
            logmsg("cannot connect adbd local transport %s:%d: %s",
                   g_dst_ip, g_dst_port, strerror(errno));
            close(cfd);
            _exit(1);
        }
        pump(cfd, dfd);
        close(cfd);
        close(dfd);
        _exit(0);
    }

    return 0;
}
