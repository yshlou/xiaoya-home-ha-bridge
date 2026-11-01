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
 * v1.2 fixes (defects found in production on 2026-10-31, evening):
 *   4. Output truncation on fast-producing commands: the child-output loop
 *      did read() FIRST and only then checked await_ok - bytes read while a
 *      WRTE was still unacknowledged were silently discarded, so
 *      "echo a; echo b; cat file" randomly lost everything after the first
 *      block (slow output always survived: the OKAY arrived during the
 *      sleep). await_ok is now checked BEFORE read(): data stays in the
 *      pipe until the client acks.
 *   5. Push hang / stdin data loss: client->us WRTE frames carry the
 *      device-side stream id in a1 (a0 is ALWAYS the sender's own id), but
 *      the lookup matched local_id == a0. That only worked while both
 *      sides numbered streams in lockstep (fresh adb server: 1,1 2,2...);
 *      once the adb server's ids drifted (ours 1, client 47) every client
 *      WRTE missed its stream and was dropped after an accidental bare
 *      OKAY ack -> sync frames vanished (push waited forever for a STAT
 *      that never came) and stdin bytes never reached the child. Lookup
 *      now matches local_id == a1, consistent with the OKAY/CLSE handlers.
 *   6. SIGPIPE was at default disposition: writing stdin to a child that
 *      had already exited raised SIGPIPE and killed the whole connection
 *      handler. Now ignored; write_all() reports EPIPE and the stream is
 *      closed cleanly.
 *   7. LIST hardening: per-entry path buffer 1100 -> 1350 bytes
 *      (spath 1023 + '/' + d_name 255).
 *   8. CNXN max-data was announced as 4096, so adb clients refused any
 *      shell command over ~4KB with "error: shell command too long" - the
 *      64KB command buffer from v1.1 was unreachable. Now announces 64KB;
 *      g_in doubled to 128KB to match.
 *   9. sync frame accumulator was 8KB: a pushed file arrives as one DATA
 *      frame fragmented across many WRTEs - anything over ~8KB would
 *      overflow the accumulator and FAIL. Now 96KB (one full 64KB DATA
 *      frame plus slack).
 *  10. STAT on a missing path replied FAIL; adb clients abort the push
 *      with "stat response has wrong message id" (they expect a STAT
 *      reply with zeroed fields meaning "not exists", like real adbd).
 *      Now replies STAT+zeros and continues.
 *  11. The SEND/DONE ack was 4 bytes; this adb client reads the full
 *      8-byte response header (id + size) and blocked forever missing the
 *      last 4 (push finished but the client never exited). Now sends
 *      OKAY + 4 zero bytes, matching real adbd.
 *  12. sync DATA payloads are now streamed straight to disk instead of
 *      being buffered until the whole frame arrives: this adb client
 *      pipelines several 64KB WRTEs without waiting for per-packet acks,
 *      so buffering whole frames overflowed any fixed buffer (200KB push
 *      died with "sync buffer overflow").
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
#define MAXPAYLOAD_IO (64 * 1024)  /* v1.2: CNXN 上报的最大包长。旧值 4096 会让 adb 客户端直接拒发长命令（"shell command too long"），64KB 命令缓冲形同虚设 */
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
    unsigned char sbuf[1104];  /* v1.2: 只存帧头+路径(≤8+1023)。DATA 帧负载不进缓冲、直接流写盘——该 adb 客户端会流水线连发多个 64KB WRTE，任何"整帧攒齐再落盘"的缓冲都会溢出 */
    size_t sfill;
    unsigned sdata_remain;    /* v1.2: SEND 状态下当前 DATA 帧剩余待落盘字节数 */
    unsigned sneed;           /* v1.2: 正在攒的帧还差多少字节才完整(8 或 8+路径长) */
    unsigned char oq[16 * 1024];  /* v1.2: 待发帧队列——该客户端会流水线连发，我方 WRTE 未被 ack 时新回复先排队，OKAY 到达后按序冲刷 */
    size_t oq_fill;
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

    if (g_verbose) {
        char c[5];
        memcpy(c, &cmd, 4);
        c[4] = 0;
        logmsg("TX %s a0=%u a1=%u len=%u", c, a0, a1, (unsigned)len);
    }
    if (write_all(fd, hdr, 24) < 0)
        return -1;
    if (len && write_all(fd, data, len) < 0)
        return -1;
    return 0;
}

/* 发送一个本流的数据 WRTE；await_ok 流控 + 待发队列：
 * 客户端流水线化（可能在我方上一包被 ack 前就发后续请求），
 * 未确认时不能丢帧，排队等 OKAY 后冲刷。 */
static int wrte_stream(int cfd, stream_t *s, const void *data, size_t len)
{
    if (s->await_ok) {
        if (s->oq_fill + len > sizeof(s->oq)) {
            logmsg("wrte queue overflow (stream %u, %zu+%zu)",
                   s->local_id, s->oq_fill, len);
            return -1;
        }
        memcpy(s->oq + s->oq_fill, data, len);
        s->oq_fill += len;
        return 0;
    }
    if (send_msg(cfd, A_WRTE, s->local_id, s->remote_id, data, len) < 0)
        return -1;
    s->await_ok = 1;
    return 0;
}

/* OKAY 到达后调用：按序冲刷待发队列（一次只发一包，发完若还有剩余则
 * 继续等下一次 OKAY） */
static void wrte_flush(int cfd, stream_t *s)
{
    while (s->oq_fill > 0) {
        size_t n = s->oq_fill;
        if (n > MAXPAYLOAD)
            n = MAXPAYLOAD;
        if (send_msg(cfd, A_WRTE, s->local_id, s->remote_id, s->oq, n) < 0)
            return;
        s->await_ok = 1;
        memmove(s->oq, s->oq + n, s->oq_fill - n);
        s->oq_fill -= n;
        if (s->oq_fill > 0)
            return;
    }
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

/* v1.2: 前置声明：sync_input 的 RECV/LIST 分支会立即开始泵数据 */
static void sync_pump(int cfd, stream_t *s);

/* v1.2: sync 字节流解析器。每收到一段客户端数据调用一次。
 * DATA 帧负载不进缓冲、直接流写文件（sdata_remain 跟踪剩余字节数），
 * 头缓冲只存帧头+路径（≤1039B）。该 adb 客户端会流水线连发多个
 * 64KB WRTE（不等逐包确认），任何"整帧攒齐再落盘"的缓冲都会溢出。 */
static void sync_input(int cfd, stream_t *s, const unsigned char *data, size_t len)
{
    size_t off = 0;
    for (;;) {
        if (s->sdata_remain > 0) {
            /* DATA 帧负载：先排空 sbuf 里压着的，再吃本次数据 */
            size_t n;
            if (s->sfill > 0) {
                n = s->sfill;
                if (n > s->sdata_remain)
                    n = s->sdata_remain;
                if (write_all(s->sfd, s->sbuf, n) < 0) {
                    sync_fail(cfd, s, "write failed");
                    return;
                }
                s->sdata_remain -= n;
                memmove(s->sbuf, s->sbuf + n, s->sfill - n);
                s->sfill -= n;
            }
            if (s->sdata_remain > 0 && off < len) {
                n = len - off;
                if (n > s->sdata_remain)
                    n = s->sdata_remain;
                if (write_all(s->sfd, data + off, n) < 0) {
                    sync_fail(cfd, s, "write failed");
                    return;
                }
                s->sdata_remain -= n;
                off += n;
            }
            if (s->sdata_remain > 0)
                return;             /* 负载未到齐，等下一个 WRTE */
            continue;
        }
        if (s->sneed == 0 && s->sfill >= 8) {
            /* 有完整帧头：定本帧总长 */
            char id[5];
            unsigned arg;
            memcpy(id, s->sbuf, 4);
            id[4] = 0;
            memcpy(&arg, s->sbuf + 4, 4);
            if (!memcmp(id, "DATA", 4) || !memcmp(id, "DONE", 4) ||
                !memcmp(id, "QUIT", 4)) {
                s->sneed = 8;
            } else {
                if (arg >= 1024) {
                    sync_fail(cfd, s, "path too long");
                    return;
                }
                s->sneed = 8 + arg;
            }
        }
        if (s->sfill < s->sneed || (s->sneed == 0 && s->sfill < 8)) {
            /* 攒字节（含首次攒 8 字节帧头——sneed 未定时也要攒，否则永远进不了帧解析） */
            size_t n = len - off;
            if (n > sizeof(s->sbuf) - s->sfill)
                n = sizeof(s->sbuf) - s->sfill;
            if (n == 0)
                return;             /* 本次数据用尽且帧未齐 */
            memcpy(s->sbuf + s->sfill, data + off, n);
            s->sfill += n;
            off += n;
            continue;
        }
        /* 帧已齐：分发 */
        {
            char id[5];
            unsigned arg;
            memcpy(id, s->sbuf, 4);
            id[4] = 0;
            memcpy(&arg, s->sbuf + 4, 4);

            if (!memcmp(id, "STAT", 4)) {
                char path[1025];
                memcpy(path, s->sbuf + 8, arg);
                path[arg] = 0;
                sync_consume(s, s->sneed);
                s->sneed = 0;
                /* stat 失败也回 STAT+全零（真实 adbd 行为），客户端据此判定
                 * "远端不存在"并继续 push；回 FAIL 会让客户端报
                 * "stat response has wrong message id" 直接中止。 */
                struct stat st;
                unsigned m = 0, sz = 0, t = 0;
                if (stat(path, &st) == 0) {
                    m = (unsigned)st.st_mode;
                    sz = (unsigned)st.st_size;
                    t = (unsigned)st.st_mtime;
                }
                unsigned char out[16];
                memcpy(out, "STAT", 4);
                memcpy(out + 4, &m, 4);
                memcpy(out + 8, &sz, 4);
                memcpy(out + 12, &t, 4);
                if (wrte_stream(cfd, s, out, 16) < 0)
                    return;
            } else if (!memcmp(id, "SEND", 4)) {
                char path[1025];
                memcpy(path, s->sbuf + 8, arg);
                path[arg] = 0;
                sync_consume(s, s->sneed);
                s->sneed = 0;
                unsigned mode = 0100644;
                char *comma = strchr(path, ',');
                if (comma) {
                    *comma = 0;
                    mode = (unsigned)strtoul(comma + 1, NULL, 8);
                }
                snprintf(s->spath, sizeof(s->spath), "%s", path);
                /* v1.2: 真实 adbd 的 secure_mkdirs——SEND 自动创建缺失的父目录。
                 * `adb push <目录> <目标>` 时客户端按 目标/子目录/文件 发 SEND，
                 * 没有这一步目录 push 必失败（文件本身由 open(O_CREAT) 创建）。 */
                {
                    char *p;
                    for (p = s->spath + 1; (p = strchr(p, '/')) != NULL; p++) {
                        *p = 0;
                        mkdir(s->spath, 0755);   /* 已存在时静默忽略 */
                        *p = '/';
                    }
                }
                s->sfd = open(s->spath, O_WRONLY | O_CREAT | O_TRUNC,
                              (mode_t)(mode & 0777));
                if (s->sfd < 0) {
                    sync_fail(cfd, s, "cannot open local file");
                    return;
                }
                s->sstate = 1;
                s->sdata_remain = 0;
                if (g_verbose)
                    logmsg("sync SEND -> %s", s->spath);
            } else if (!memcmp(id, "DATA", 4)) {
                sync_consume(s, 8);
                s->sneed = 0;
                if (s->sstate != 1 || s->sfd < 0) {
                    sync_fail(cfd, s, "DATA outside SEND");
                    return;
                }
                s->sdata_remain = arg;
            } else if (!memcmp(id, "DONE", 4)) {
                sync_consume(s, 8);
                s->sneed = 0;
                if (s->sstate == 1 && s->sfd >= 0) {
                    close(s->sfd);
                    s->sfd = -1;
                    struct timespec ts[2];
                    ts[0].tv_sec = 0;
                    ts[0].tv_nsec = UTIME_OMIT;
                    ts[1].tv_sec = (time_t)arg;
                    ts[1].tv_nsec = 0;
                    utimensat(AT_FDCWD, s->spath, ts, 0);
                    if (g_verbose)
                        logmsg("sync SEND done: %s", s->spath);
                }
                unsigned char ok[8];
                memcpy(ok, "OKAY", 4);
                unsigned zero = 0;
                memcpy(ok + 4, &zero, 4);
                /* 必须发足 8 字节（id+size）：客户端读完整应答头，只发 4 字节
                 * 会让它在 DONE 后永远等最后 4 字节（push 完成后不退出）。 */
                if (wrte_stream(cfd, s, ok, 8) < 0)
                    return;
            } else if (!memcmp(id, "RECV", 4)) {
                char path[1025];
                memcpy(path, s->sbuf + 8, arg);
                path[arg] = 0;
                sync_consume(s, s->sneed);
                s->sneed = 0;
                s->sfd = open(path, O_RDONLY);
                if (s->sfd < 0) {
                    sync_fail(cfd, s, "cannot open local file");
                    return;
                }
                snprintf(s->spath, sizeof(s->spath), "%s", path);
                s->sstate = 2;
                if (g_verbose)
                    logmsg("sync RECV <- %s", path);
                sync_pump(cfd, s);
            } else if (!memcmp(id, "LIST", 4)) {
                char path[1025];
                memcpy(path, s->sbuf + 8, arg);
                path[arg] = 0;
                sync_consume(s, s->sneed);
                s->sneed = 0;
                s->dirp = opendir(path);
                if (!s->dirp) {
                    sync_fail(cfd, s, "opendir failed");
                    return;
                }
                snprintf(s->spath, sizeof(s->spath), "%s", path);
                s->sstate = 3;
                if (g_verbose)
                    logmsg("sync LIST %s", path);
                sync_pump(cfd, s);
            } else if (!memcmp(id, "QUIT", 4)) {
                sync_consume(s, 8);
                s->sneed = 0;
                send_msg(cfd, A_CLSE, s->local_id, s->remote_id, NULL, 0);
                stream_close(s);
                return;
            } else {
                sync_fail(cfd, s, "unknown sync command");
                return;
            }
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
        /* v1.2: 文件读完只回 DONE、不关流——目录 pull 会在同一条 sync 流上
         * 继续 RECV 下一个文件（真实 adbd 如此）；收尾由客户端 QUIT/CLSE 完成。 */
        close(s->sfd);
        s->sfd = -1;
        s->sstate = 0;
        memcpy(out, "DONE", 4);
        unsigned zero = 0;
        memcpy(out + 4, &zero, 4);
        wrte_stream(cfd, s, out, 8);
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
            char full[1350];   /* spath(1023) + '/' + d_name(255)，v1.2 加固 */
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
        /* v1.2: LIST 结束符必须 20 字节（DONE+4 个零字段）：客户端在 LIST 循环里
         * 按完整 DENT 头(20B)读每一帧，8 字节 DONE 会让它永远等剩下 12 字节。
         * 流保持打开，客户端随后在同一条流上逐个 RECV 文件，收尾靠 QUIT/CLSE。 */
        unsigned char done[20];
        memset(done, 0, sizeof(done));
        memcpy(done, "DONE", 4);
        wrte_stream(cfd, s, done, 20);
    }
}

/* ---------- per-connection ---------- */
static unsigned char g_in[128 * 1024];   /* v1.2: 客户端最大可发 64KB 包，留双倍余量 */
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
    if (send_msg(cfd, A_CNXN, PROTO_VERSION, MAXPAYLOAD_IO, banner, strlen(banner)) < 0)
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

        /* child produced output -> WRTE to client (paced by await_ok).
         * v1.2 修复：必须先判 await_ok 再 read()。旧代码先读后判，把已读
         * 出来的数据直接丢弃——快速输出的第二块起被随机截断。数据留在
         * 管道里（默认 64KB 容量足够客户端 ACK 往返），等 OKAY 到达再读。 */
        for (i = 1; i < n; i++) {
            if (mapping[i] < 0)
                continue;
            if (!(p[i].revents & (POLLIN | POLLHUP)))
                continue;
            stream_t *s = &g_s[mapping[i]];
            if (s->await_ok)
                continue;   /* 未确认前绝不读，避免读出后丢弃 */
            char buf[MAXPAYLOAD];
            ssize_t got = read(s->from_child, buf, sizeof(buf));
            if (got < 0) {
                if (errno == EAGAIN)
                    continue;
                logmsg("child read err fd=%d errno=%d", s->from_child, errno);
                continue;   /* 非 EAGAIN 错误不当作 EOF */
            }
            if (got > 0) {
                if (wrte_stream(cfd, s, buf, (size_t)got) < 0)
                    return;
                continue;
            }
            /* EOF */
            logmsg("child EOF pid=%d", (int)s->pid);
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
                /* v1.2 修复：客户端发来的包里 a0 恒为发送方自己的流 id，
                 * 我们这一侧的流 id 在 a1。旧代码按 a0 匹配，双方流号一旦
                 * 不同步（adb server 流号漂移后如 47），所有客户端 WRTE
                 * 都找不到流：sync 帧被丢弃=push 挂死，stdin 数据进不了
                 * 子进程。与 OKAY/CLSE 处理一致，改按 a1 匹配。 */
                int found = 0;
                for (i = 0; i < MAX_STREAMS; i++) {
                    if (g_s[i].active && g_s[i].local_id == a1) {
                        found = 1;
                        /* 先确认，客户端才会继续发 */
                        send_msg(cfd, A_OKAY, a1, a0, NULL, 0);
                        if (g_s[i].kind == 0) {
                            if (g_s[i].to_child >= 0 &&
                                write_all(g_s[i].to_child, payload, dlen) < 0)
                                stream_close(&g_s[i]);
                        } else if (g_s[i].kind == 1) {
                            sync_input(cfd, &g_s[i], payload, dlen);
                        }
                        break;
                    }
                }
                if (!found)
                    send_msg(cfd, A_OKAY, a1, a0, NULL, 0);
            } else if (cmd == A_OKAY) {
                /* 客户端确认了我们的 WRTE -> 解锁并冲刷待发队列 */
                for (i = 0; i < MAX_STREAMS; i++) {
                    if (g_s[i].active && g_s[i].local_id == a1) {
                        g_s[i].await_ok = 0;
                        wrte_flush(cfd, &g_s[i]);
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
    signal(SIGPIPE, SIG_IGN);   /* v1.2: EPIPE 由 write_all 返回 -1 处理；默认行为会杀死整个连接处理进程 */

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l") && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-q")) g_verbose = 0;
        else if (!strcmp(argv[i], "-h")) {
            fprintf(stderr, "adbxd v1.2 - minimal adb daemon over TCP\n"
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
        logmsg("adbxd v1.2 ready on 0.0.0.0:%d (shell/exec/cmd/sync)", g_port);

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