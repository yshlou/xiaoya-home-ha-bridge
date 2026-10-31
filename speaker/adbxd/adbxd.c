/* adbxd.c - minimal ADB daemon listening on TCP for Xiaoya HOME speaker
 *
 * Why:
 *   The stock adbd only ever binds USB FunctionFS. "adb tcpip" prints the
 *   "restarting in TCP mode" banner and exits without listening (no property
 *   backend -> persist.adb.tcp.port never read). Its loopback socket on
 *   127.0.0.1:5037 is not a usable client transport either (verified: it
 *   closes the connection on CNXN).
 *
 *   So this speaks the ADB wire protocol itself, over TCP, on the device.
 *   Supports: shell / exec / cmd services + sync v1 (adb push / pull / stat).
 *
 * v1.1 fixes (defects found in production on 2026-10-31):
 *   1. OPEN dest buffer was 512 bytes -> any shell command longer than 511
 *      characters was silently truncated (and could split a quote, breaking
 *      `sh -c` with "unterminated quoted string"). Now 64 KB.
 *   2. sync: service did not exist; OPEN was answered OKAY+CLSE, so
 *      `adb push/pull` failed with "protocol fault: failed to read stat
 *      response". Implemented full sync v1: STAT / SEND / RECV / LIST / QUIT
 *      with DATA/DONE frames (v2 frames are never used: the feature banner
 *      no longer advertises stat_v2/sendrecv_v2/ls_v2/shell_v2/abb*).
 *   3. WRTE acks echoed the client's (a0,a1) instead of the correct
 *      (our_local, their_remote) - adb clients choke on stdin forwarding
 *      ("adb shell cmd < file" hung forever). Fixed, plus real per-stream
 *      flow control: we now wait for the client's OKAY before the next WRTE
 *      (await_ok), instead of blasting unacknowledged packets.
 *
 * Protocol recap (little endian, 24-byte header):
 *   cmd(4) arg0(4) arg1(4) data_len(4) data_checksum(4) magic(4)
 *   CNXN OPEN OKAY CLSE WRTE AUTH
 *   adbd sends CNXN first; client answers CNXN; then OPEN/OKAY/WRTE/CLSE.
 *   sync service (v1): after OKAY, frames are id(4)+arg(4)+[payload]:
 *     STAT len path -> STAT mode size mtime
 *     SEND len "path,mode" ; DATA len bytes ... ; DONE mtime -> OKAY | FAIL
 *     RECV len path -> DATA len bytes ... DONE 0
 *     LIST len path -> DENT mode size mtime len name ... DONE 0
 *     QUIT          -> (stream closes)
 *
 * Build:
 *   zig cc -target aarch64-linux-musl -O2 -static -s -o adbxd adbxd.c
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* These are int.from_bytes(name, 'little'); note A_OPEN is 0x4e45504f,
   NOT 0x4e4f5050 (a wrong constant silently drops every OPEN packet). */
#define A_CNXN 0x4e584e43   /* "CNXN" */
#define A_OPEN 0x4e45504f   /* "OPEN" */
#define A_OKAY 0x59414b4f   /* "OKAY" */
#define A_CLSE 0x45534c43   /* "CLSE" */
#define A_WRTE 0x45545257   /* "WRTE" */
#define A_AUTH 0x48545541   /* "AUTH" */

#define MAX_STREAMS 8
#define MAXPAYLOAD 4096
#define SYNC_CHUNK (MAXPAYLOAD - 32)   /* room for DATA/DENT frame headers */
#define MAXCMD (64 * 1024)
#define PROTO_VERSION 0x01000000

static int g_port = 5555;
static int g_verbose = 1;
static unsigned g_localid = 1;

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

/* ---------- stream table ---------- */
typedef struct {
    int active;
    unsigned remote_id;   /* client's id for this stream */
    unsigned local_id;    /* our id */
    int kind;             /* 0 = shell/exec child, 1 = sync session */
    int await_ok;         /* we sent WRTE, waiting for client's OKAY */
    pid_t pid;
    int to_child;         /* write end -> child stdin */
    int from_child;       /* read end  <- child stdout */
    /* sync session state */
    int sstate;           /* 0 idle, 1 SEND, 2 RECV, 3 LIST */
    int sfd;              /* file fd, -1 none */
    char spath[1024];
    unsigned char sbuf[8192];   /* sync frame accumulator */
    size_t sfill;
    DIR *dirp;
} stream_t;

static stream_t g_s[MAX_STREAMS];

static stream_t *stream_new(unsigned remote_id, pid_t pid, int to_child, int from_child)
{
    int i;
    for (i = 0; i < MAX_STREAMS; i++) {
        if (!g_s[i].active) {
            memset(&g_s[i], 0, sizeof(g_s[i]));
            g_s[i].active = 1;
            g_s[i].remote_id = remote_id;
            g_s[i].local_id = g_localid++;
            g_s[i].pid = pid;
            g_s[i].to_child = to_child;
            g_s[i].from_child = from_child;
            g_s[i].sfd = -1;
            g_s[i].await_ok = 0;
            g_s[i].kind = 0;
            return &g_s[i];
        }
    }
    return NULL;
}

static void stream_close(stream_t *s)
{
    if (!s->active)
        return;
    if (s->to_child >= 0)
        close(s->to_child);
    if (s->from_child >= 0)
        close(s->from_child);
    if (s->sfd >= 0)
        close(s->sfd);
    if (s->dirp)
        closedir(s->dirp);
    if (s->pid > 0)
        kill(s->pid, SIGKILL);
    s->active = 0;
}

/* ---------- helpers ---------- */
static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w <= 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

static unsigned checksum(const void *d, size_t n)
{
    const unsigned char *p = (const unsigned char *)d;
    unsigned sum = 0;
    size_t i;
    for (i = 0; i < n; i++)
        sum += p[i];
    return sum;
}

static int send_msg(int fd, unsigned cmd, unsigned a0, unsigned a1,
                    const void *data, size_t len)
{
    unsigned char hdr[24];
    unsigned char *h = hdr;
    unsigned payload_len = (unsigned)len;

    memcpy(h, &cmd, 4);                     h += 4;
    memcpy(h, &a0, 4);                      h += 4;
    memcpy(h, &a1, 4);                      h += 4;
    memcpy(h, &payload_len, 4);             h += 4;
    unsigned cs = checksum(data, len);
    memcpy(h, &cs, 4);                      h += 4;
    unsigned magic = cmd ^ 0xffffffffu;
    memcpy(h, &magic, 4);

    if (write_all(fd, hdr, 24) < 0)
        return -1;
    if (len && write_all(fd, data, len) < 0)
        return -1;
    return 0;
}

/* 发送一个本流的数据 WRTE；每流同时只允许一个未确认 WRTE（await_ok 流控） */
static int wrte_stream(int cfd, stream_t *s, const void *data, size_t len)
{
    if (s->await_ok)
        return -1;
    if (send_msg(cfd, A_WRTE, s->local_id, s->remote_id, data, len) < 0)
        return -1;
    s->await_ok = 1;
    return 0;
}

/* ---------- command runner ---------- */
static stream_t *spawn_cmd(unsigned remote_id, const char *cmdline)
{
    int in[2], out[2];
    if (pipe(in) < 0 || pipe(out) < 0)
        return NULL;

    pid_t pid = fork();
    if (pid < 0) {
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        return NULL;
    }
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        setsid();
        execl("/bin/sh", "sh", "-c", cmdline, (char *)NULL);
        _exit(127);
    }

    close(in[0]);
    close(out[1]);
    int flags = fcntl(out[0], F_GETFL, 0);
    fcntl(out[0], F_SETFL, flags | O_NONBLOCK);

    return stream_new(remote_id, pid, in[1], out[0]);
}

/* ---------- sync (adb push/pull) v1 ---------- */
static void sync_consume(stream_t *s, size_t n)
{
    memmove(s->sbuf, s->sbuf + n, s->sfill - n);
    s->sfill -= n;
}

static void sync_fail(int cfd, stream_t *s, const char *msg)
{
    unsigned char f[8 + 256];
    int ml = (int)strlen(msg);
    if (ml > 255)
        ml = 255;
    memcpy(f, "FAIL", 4);
    memcpy(f + 4, &ml, 4);
    memcpy(f + 8, msg, (size_t)ml);
    logmsg("sync FAIL: %s", msg);
    wrte_stream(cfd, s, f, (size_t)8 + ml);
    send_msg(cfd, A_CLSE, s->local_id, s->remote_id, NULL, 0);
    stream_close(s);
}

/* 消费 sync 流缓冲区里的完整帧；不完整则留在缓冲区等下一个 WRTE */
static void sync_process(int cfd, stream_t *s)
{
    for (;;) {
        if (s->sfill < 8)
            return;
        char id[5];
        memcpy(id, s->sbuf, 4);
        id[4] = 0;
        unsigned arg;
        memcpy(&arg, s->sbuf + 4, 4);

        if (!memcmp(id, "STAT", 4)) {
            if (s->sfill < 8 + (size_t)arg)
                return;
            if (arg >= sizeof(s->spath)) {
                sync_fail(cfd, s, "path too long");
                return;
            }
            char path[1024];
            memcpy(path, s->sbuf + 8, arg);
            path[arg] = 0;
            sync_consume(s, 8 + (size_t)arg);
            struct stat st;
            if (stat(path, &st) != 0) {
                sync_fail(cfd, s, "stat failed");
                return;
            }
            unsigned char out[16];
            unsigned m = (unsigned)st.st_mode, sz = (unsigned)st.st_size;
            unsigned t = (unsigned)st.st_mtime;
            memcpy(out, "STAT", 4);
            memcpy(out + 4, &m, 4);
            memcpy(out + 8, &sz, 4);
            memcpy(out + 12, &t, 4);
            if (wrte_stream(cfd, s, out, 16) < 0)
                return;
        } else if (!memcmp(id, "SEND", 4)) {
            if (s->sfill < 8 + (size_t)arg)
                return;
            if (arg >= 1000) {
                sync_fail(cfd, s, "path too long");
                return;
            }
            char tmp[1025];
            memcpy(tmp, s->sbuf + 8, arg);
            tmp[arg] = 0;
            sync_consume(s, 8 + (size_t)arg);
            unsigned mode = 0100644;
            char *comma = strchr(tmp, ',');
            if (comma) {
                *comma = 0;
                mode = (unsigned)strtoul(comma + 1, NULL, 8);
            }
            snprintf(s->spath, sizeof(s->spath), "%s", tmp);
            s->sfd = open(s->spath, O_WRONLY | O_CREAT | O_TRUNC,
                          (mode_t)(mode & 0777));
            if (s->sfd < 0) {
                sync_fail(cfd, s, "cannot open local file");
                return;
            }
            s->sstate = 1;
            if (g_verbose)
                logmsg("sync SEND -> %s", s->spath);
        } else if (!memcmp(id, "DATA", 4)) {
            if (s->sfill < 8 + (size_t)arg)
                return;
            if (s->sstate != 1 || s->sfd < 0) {
                sync_fail(cfd, s, "DATA outside SEND");
                return;
            }
            if (write_all(s->sfd, s->sbuf + 8, arg) < 0) {
                sync_fail(cfd, s, "write failed");
                return;
            }
            sync_consume(s, 8 + (size_t)arg);
        } else if (!memcmp(id, "DONE", 4)) {
            if (s->sfill < 8)
                return;
            unsigned mt;
            memcpy(&mt, s->sbuf + 4, 4);
            sync_consume(s, 8);
            if (s->sstate == 1 && s->sfd >= 0) {
                close(s->sfd);
                s->sfd = -1;
                struct timespec ts[2];
                ts[0].tv_sec = 0;
                ts[0].tv_nsec = UTIME_OMIT;
                ts[1].tv_sec = (time_t)mt;
                ts[1].tv_nsec = 0;
                utimensat(AT_FDCWD, s->spath, ts, 0);
                s->sstate = 0;
                if (g_verbose)
                    logmsg("sync SEND done: %s", s->spath);
            }
            unsigned char ok[8];
            memcpy(ok, "OKAY", 4);
            unsigned zero = 0;
            memcpy(ok + 4, &zero, 4);
            if (wrte_stream(cfd, s, ok, 4) < 0)
                return;
        } else if (!memcmp(id, "RECV", 4)) {
            if (s->sfill < 8 + (size_t)arg)
                return;
            if (arg >= sizeof(s->spath)) {
                sync_fail(cfd, s, "path too long");
                return;
            }
            char path[1024];
            memcpy(path, s->sbuf + 8, arg);
            path[arg] = 0;
            sync_consume(s, 8 + (size_t)arg);
            s->sfd = open(path, O_RDONLY);
            if (s->sfd < 0) {
                sync_fail(cfd, s, "cannot open local file");
                return;
            }
            snprintf(s->spath, sizeof(s->spath), "%s", path);
            s->sstate = 2;
            if (g_verbose)
                logmsg("sync RECV <- %s", path);
        } else if (!memcmp(id, "LIST", 4)) {
            if (s->sfill < 8 + (size_t)arg)
                return;
            if (arg >= sizeof(s->spath)) {
                sync_fail(cfd, s, "path too long");
                return;
            }
            char path[1024];
            memcpy(path, s->sbuf + 8, arg);
            path[arg] = 0;
            sync_consume(s, 8 + (size_t)arg);
            s->dirp = opendir(path);
            if (!s->dirp) {
                sync_fail(cfd, s, "opendir failed");
                return;
            }
            snprintf(s->spath, sizeof(s->spath), "%s", path);
            s->sstate = 3;
            if (g_verbose)
                logmsg("sync LIST %s", path);
        } else if (!memcmp(id, "QUIT", 4)) {
            sync_consume(s, 8);
            send_msg(cfd, A_CLSE, s->local_id, s->remote_id, NULL, 0);
            stream_close(s);
            return;
        } else {
            sync_fail(cfd, s, "unknown sync command");
            return;
        }
    }
}

/* RECV / LIST 的数据泵：await_ok 清零后由主循环反复调用 */
static void sync_pump(int cfd, stream_t *s)
{
    if (s->await_ok)
        return;

    if (s->sstate == 2 && s->sfd >= 0) {
        unsigned char out[8 + SYNC_CHUNK];
        ssize_t n = read(s->sfd, out + 8, SYNC_CHUNK);
        if (n < 0 && errno == EINTR)
            return;
        if (n < 0) {
            sync_fail(cfd, s, "read failed");
            return;
        }
        if (n > 0) {
            memcpy(out, "DATA", 4);
            memcpy(out + 4, &n, 4);
            wrte_stream(cfd, s, out, (size_t)8 + (size_t)n);
            return;
        }
        memcpy(out, "DONE", 4);
        unsigned zero = 0;
        memcpy(out + 4, &zero, 4);
        wrte_stream(cfd, s, out, 8);
        send_msg(cfd, A_CLSE, s->local_id, s->remote_id, NULL, 0);
        stream_close(s);
        return;
    }

    if (s->sstate == 3 && s->dirp) {
        unsigned char out[MAXPAYLOAD];
        size_t fill = 0;
        struct dirent *ent;
        int any = 0;
        while ((ent = readdir(s->dirp)) != NULL) {
            if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
                continue;
            size_t nl = strlen(ent->d_name);
            if (fill + 20 + nl > MAXPAYLOAD)
                break;
            char full[1100];
            snprintf(full, sizeof(full), "%s/%s", s->spath, ent->d_name);
            struct stat st;
            unsigned m = 0, sz = 0, t = 0;
            if (stat(full, &st) == 0) {
                m = (unsigned)st.st_mode;
                sz = (unsigned)st.st_size;
                t = (unsigned)st.st_mtime;
            }
            memcpy(out + fill, "DENT", 4);
            memcpy(out + fill + 4, &m, 4);
            memcpy(out + fill + 8, &sz, 4);
            memcpy(out + fill + 12, &t, 4);
            memcpy(out + fill + 16, &nl, 4);
            memcpy(out + fill + 20, ent->d_name, nl);
            fill += 20 + nl;
            any = 1;
        }
        if (any) {
            wrte_stream(cfd, s, out, fill);
            return;
        }
        closedir(s->dirp);
        s->dirp = NULL;
        s->sstate = 0;
        unsigned char done[8];
        memcpy(done, "DONE", 4);
        unsigned zero = 0;
        memcpy(done + 4, &zero, 4);
        wrte_stream(cfd, s, done, 8);
        send_msg(cfd, A_CLSE, s->local_id, s->remote_id, NULL, 0);
        stream_close(s);
    }
}

/* ---------- per-connection ---------- */
static unsigned char g_in[64 * 1024];
static size_t g_infill = 0;
static char g_dest[MAXCMD + 1];

static void handle_client(int cfd)
{
    memset(g_s, 0, sizeof(g_s));

    /* handshake: we speak first, as adbd does.
     * 特性串一律不报 v2（stat_v2/sendrecv_v2/ls_v2/shell_v2/abb*），
     * 让 adb 客户端走我们真正实现的 v1 sync / legacy shell。 */
    char banner[32];
    snprintf(banner, sizeof(banner), "device::");
    if (send_msg(cfd, A_CNXN, PROTO_VERSION, MAXPAYLOAD, banner, strlen(banner)) < 0)
        return;

    for (;;) {
        struct pollfd p[MAX_STREAMS + 1];
        int n = 0, i;

        p[n].fd = cfd;
        p[n].events = POLLIN;
        n++;
        int mapping[MAX_STREAMS + 1];
        memset(mapping, -1, sizeof(mapping));
        for (i = 0; i < MAX_STREAMS; i++) {
            if (g_s[i].active && g_s[i].kind == 0 && g_s[i].from_child >= 0) {
                p[n].fd = g_s[i].from_child;
                p[n].events = POLLIN;
                mapping[n] = i;
                n++;
            }
        }

        int r = poll(p, (unsigned)n, -1);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return;
        }

        /* child produced output -> WRTE to client (paced by await_ok) */
        for (i = 1; i < n; i++) {
            if (!(p[i].revents & (POLLIN | POLLHUP)))
                continue;
            if (mapping[i] < 0)
                continue;
            stream_t *s = &g_s[mapping[i]];
            char buf[MAXPAYLOAD];
            ssize_t got = read(s->from_child, buf, sizeof(buf));
            if (got < 0 && errno == EAGAIN)
                continue;
            if (got > 0) {
                if (s->await_ok)
                    continue;   /* 等客户端 OKAY，数据暂留管道 */
                if (wrte_stream(cfd, s, buf, (size_t)got) < 0)
                    return;
                continue;
            }
            /* EOF */
            if (s->pid > 0) {
                int st;
                waitpid(s->pid, &st, 0);
                s->pid = 0;
            }
            if (send_msg(cfd, A_CLSE, s->local_id, s->remote_id, NULL, 0) < 0)
                return;
            close(s->from_child);
            s->from_child = -1;
            close(s->to_child);
            s->to_child = -1;
            s->active = 0;
        }

        /* client spoke */
        if (!(p[0].revents & (POLLIN | POLLERR | POLLHUP)))
            continue;

        ssize_t got = read(cfd, g_in + g_infill, sizeof(g_in) - g_infill);
        if (got <= 0)
            return;
        g_infill += (size_t)got;
        if (g_verbose)
            logmsg("read %zd bytes (buf now %zu)", got, g_infill);

        while (g_infill >= 24) {
            unsigned cmd, a0, a1, dlen, magic;
            unsigned char *h = g_in;
            memcpy(&cmd, h, 4);       h += 4;
            memcpy(&a0, h, 4);        h += 4;
            memcpy(&a1, h, 4);        h += 4;
            memcpy(&dlen, h, 4);      h += 4;
            h += 4;                   /* checksum unused */
            memcpy(&magic, h, 4);

            if ((cmd ^ 0xffffffffu) != magic) {
                logmsg("bad magic cmd=%08x magic=%08x, disconnect", cmd, magic);
                return;
            }
            if (g_verbose) {
                char c[5];
                memcpy(c, &cmd, 4);
                c[4] = 0;
                logmsg("pkt %s a0=%u a1=%u len=%u", c, a0, a1, dlen);
            }
            if (dlen > sizeof(g_in) - 24 || g_infill < 24 + (size_t)dlen)
                break;   /* wait for rest of packet */

            unsigned char *payload = g_in + 24;

            if (cmd == A_OPEN) {
                size_t dl = dlen < MAXCMD ? dlen : MAXCMD;
                memcpy(g_dest, payload, dl);
                g_dest[dl] = 0;
                if (g_verbose)
                    logmsg("OPEN remote_id=%u dest=%.60s%s", a0, g_dest,
                           dlen > 60 ? "..." : "");

                const char *cmdptr = NULL;
                int is_sync = 0;
                if (!strncmp(g_dest, "shell:", 6))        cmdptr = g_dest + 6;
                else if (!strncmp(g_dest, "shell,v2:", 9)) cmdptr = g_dest + 9;
                else if (!strncmp(g_dest, "exec:", 5))    cmdptr = g_dest + 5;
                else if (!strncmp(g_dest, "cmd:", 4))     cmdptr = g_dest + 4;
                else if (!strncmp(g_dest, "sync:", 5))    is_sync = 1;

                if (is_sync) {
                    stream_t *s = stream_new(a0, 0, -1, -1);
                    if (s) {
                        s->kind = 1;
                        s->sfd = -1;
                        s->sstate = 0;
                        s->sfill = 0;
                        send_msg(cfd, A_OKAY, s->local_id, s->remote_id, NULL, 0);
                    } else {
                        send_msg(cfd, A_CLSE, 0, a0, NULL, 0);
                    }
                } else if (cmdptr) {
                    stream_t *s = spawn_cmd(a0, cmdptr);
                    if (s && send_msg(cfd, A_OKAY, s->local_id, s->remote_id, NULL, 0) == 0) {
                        /* fall through to consume bytes */
                    } else {
                        send_msg(cfd, A_CLSE, 0, a0, NULL, 0);
                    }
                } else {
                    /* unsupported service - accept then close immediately */
                    stream_t *s = stream_new(a0, 0, -1, -1);
                    if (s) {
                        send_msg(cfd, A_OKAY, s->local_id, s->remote_id, NULL, 0);
                        send_msg(cfd, A_CLSE, s->local_id, s->remote_id, NULL, 0);
                        s->active = 0;
                    } else {
                        send_msg(cfd, A_CLSE, 0, a0, NULL, 0);
                    }
                }
            } else if (cmd == A_WRTE) {
                int found = 0;
                for (i = 0; i < MAX_STREAMS; i++) {
                    if (g_s[i].active && g_s[i].local_id == a0) {
                        found = 1;
                        /* 先确认，客户端才会继续发 */
                        send_msg(cfd, A_OKAY, a1, a0, NULL, 0);
                        if (g_s[i].kind == 0) {
                            if (g_s[i].to_child >= 0 &&
                                write_all(g_s[i].to_child, payload, dlen) < 0)
                                stream_close(&g_s[i]);
                        } else if (g_s[i].kind == 1) {
                            if (g_s[i].sfill + dlen <= sizeof(g_s[i].sbuf)) {
                                memcpy(g_s[i].sbuf + g_s[i].sfill, payload, dlen);
                                g_s[i].sfill += dlen;
                                sync_process(cfd, &g_s[i]);
                            } else {
                                sync_fail(cfd, &g_s[i], "sync buffer overflow");
                            }
                        }
                        break;
                    }
                }
                if (!found)
                    send_msg(cfd, A_OKAY, a1, a0, NULL, 0);
            } else if (cmd == A_OKAY) {
                /* 客户端确认了我们的 WRTE -> 解锁下一包 */
                for (i = 0; i < MAX_STREAMS; i++) {
                    if (g_s[i].active && g_s[i].local_id == a1) {
                        g_s[i].await_ok = 0;
                        break;
                    }
                }
            } else if (cmd == A_CLSE) {
                for (i = 0; i < MAX_STREAMS; i++) {
                    if (g_s[i].active && g_s[i].local_id == a1) {
                        if (g_s[i].kind == 0) {
                            /* shell：只关 stdin，让输出自然排干后走 EOF */
                            if (g_s[i].to_child >= 0) {
                                close(g_s[i].to_child);
                                g_s[i].to_child = -1;
                            }
                        } else {
                            stream_close(&g_s[i]);
                        }
                        break;
                    }
                }
            } else if (cmd == A_CNXN) {
                /* client answered handshake; nothing more needed */
            } else if (cmd == A_AUTH) {
                if (g_verbose)
                    logmsg("client sent AUTH - ignoring (no auth required)");
            }

            /* consume this packet */
            size_t total = 24 + (size_t)dlen;
            memmove(g_in, g_in + total, g_infill - total);
            g_infill -= total;
        }

        /* RECV / LIST 数据泵 */
        for (i = 0; i < MAX_STREAMS; i++) {
            if (g_s[i].active && g_s[i].kind == 1 &&
                (g_s[i].sstate == 2 || g_s[i].sstate == 3))
                sync_pump(cfd, &g_s[i]);
        }
    }
}

int main(int argc, char **argv)
{
    int i;
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_DFL);

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l") && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-q")) g_verbose = 0;
        else if (!strcmp(argv[i], "-h")) {
            fprintf(stderr, "adbxd v1.1 - minimal adb daemon over TCP\n"
                            "  -l <port>  listen port (default 5555)\n"
                            "  -q         quiet\n"
                            "  services: shell exec cmd sync(v1 push/pull)\n");
            return 0;
        }
    }

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0)
        return 1;
    int on = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port = htons(g_port);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(lfd, 8) < 0) {
        logmsg("cannot listen on %d: %s", g_port, strerror(errno));
        return 1;
    }
    logmsg("adbxd v1.1 ready on 0.0.0.0:%d (shell/exec/cmd/sync)", g_port);

    for (;;) {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        int cfd = accept(lfd, (struct sockaddr *)&peer, &plen);
        if (cfd < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            sleep(1);
            continue;
        }
        char ip[64];
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        if (g_verbose)
            logmsg("adb client connected: %s", ip);

        pid_t pid = fork();
        if (pid < 0) {
            close(cfd);
            continue;
        }
        if (pid > 0) {
            close(cfd);
            continue;
        }
        close(lfd);
        handle_client(cfd);
        close(cfd);
        _exit(0);
    }
    return 0;
}
