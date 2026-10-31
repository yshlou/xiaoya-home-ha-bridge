/*
 * xbridge.c — 小雅HOME 本地 MQTT 桥接守护进程
 *
 * 功能：
 *   - 连接局域网 MQTT broker，带 LWT / 自动重连
 *   - HA MQTT discovery 自动注册组合实体（button/number/sensor；
 *     HA 2026.9 起已移除 MQTT media_player 平台，改用此方案）
 *   - 命令：cmd/tts、cmd/play（URL，MP3/WAV/电台流）、cmd/stop、cmd/volume、
 *           cmd/media（play/pause/stop/toggle/prev/next/reload，pause 用 SIGSTOP 实现）、
 *           cmd/exec（默认关闭，-e 开启）
 *   - 歌单：-l 指定音乐目录 HTTP 列表页，支持上一曲/下一曲/播完自动连播/曲目上报
 *   - 按键：直接读 /dev/input/event*，发布到 event/button（不影响 XYOS 同时读取）
 *   - 状态自愈：每 60s 重发 available/音量/播放状态/曲目（均 retain），
 *     任何一端断线重连错过消息时自动恢复，实体不再卡“不可用”
 *   - 音频：minimp3 解码 MP3 / 自解析 WAV 头 → 管道喂给 aplay（走 asound.conf 的路由）
 *
 * 编译（见 build.cmd / build.yml）：
 *   zig cc -target aarch64-linux-musl -O2 -DHAVE_MINIMP3 -static -s -o xbridge xbridge.c
 *
 * 纯 POSIX + musl 可编译，无外部依赖。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <stdint.h>
#include <poll.h>
#include <ctype.h>
#include <strings.h>

#ifdef HAVE_MINIMP3
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#endif

/* ---------------- 配置 ---------------- */
static char g_broker[128] = "";
static int  g_port        = 1883;
static char g_user[64]    = "";
static char g_pass[64]    = "";
static char g_prefix[64]  = "xiaoya";
static char g_devid[64]   = "xiaoya_home";
static char g_alsadev[64] = "real_playback";
static int  g_allow_exec     = 0;
static int  g_pmdiscovery    = 1;
static int  g_verbose        = 0;
static volatile int g_run    = 1;
static int  g_probe          = 0;

/* 歌单（NAS 音乐目录 HTTP 列表，仅 MP3） */
static char g_playlist_url[512] = "";      /* -l 选项 */
#define PL_MAX 512
static char *g_pl[PL_MAX];
static int  g_plcnt = 0;
static int  g_plidx = -1;

/* 主题 */
static char T_CMD_TTS[96], T_CMD_PLAY[96], T_CMD_STOP[96], T_CMD_VOL[96],
            T_CMD_MEDIA[96], T_CMD_EXEC[96], T_STATE_PB[96], T_STATE_VOL[96],
            T_STATE_AVAIL[96], T_STATE_TRACK[96], T_EV_BUTTON[96], T_DISC_MP[128];
static char T_DISC_BTN_PLAY[128], T_DISC_BTN_PAUSE[128], T_DISC_BTN_STOP[128],
            T_DISC_NUM_VOL[128], T_DISC_SEN_PB[128], T_DISC_SEN_BTN[128];
static char T_DISC_BTN_TOG[128], T_DISC_BTN_PREV[128], T_DISC_BTN_NEXT[128],
            T_DISC_SEN_TRACK[128];

static void init_topics(void)
{
    snprintf(T_CMD_TTS,   sizeof(T_CMD_TTS),   "%s/cmd/tts",    g_prefix);
    snprintf(T_CMD_PLAY,  sizeof(T_CMD_PLAY),  "%s/cmd/play",   g_prefix);
    snprintf(T_CMD_STOP,  sizeof(T_CMD_STOP),  "%s/cmd/stop",   g_prefix);
    snprintf(T_CMD_VOL,   sizeof(T_CMD_VOL),   "%s/cmd/volume", g_prefix);
    snprintf(T_CMD_MEDIA, sizeof(T_CMD_MEDIA), "%s/cmd/media",  g_prefix);
    snprintf(T_CMD_EXEC,  sizeof(T_CMD_EXEC),  "%s/cmd/exec",   g_prefix);
    snprintf(T_STATE_PB,  sizeof(T_STATE_PB),  "%s/state/playback", g_prefix);
    snprintf(T_STATE_VOL, sizeof(T_STATE_VOL),  "%s/state/volume",   g_prefix);
    snprintf(T_STATE_AVAIL, sizeof(T_STATE_AVAIL), "%s/state/available", g_prefix);
    snprintf(T_EV_BUTTON, sizeof(T_EV_BUTTON), "%s/event/button", g_prefix);
    snprintf(T_STATE_TRACK, sizeof(T_STATE_TRACK), "%s/state/track", g_prefix);
    snprintf(T_DISC_MP,   sizeof(T_DISC_MP),   "homeassistant/media_player/%s/config", g_devid);
    /* HA 2026.9 已移除 MQTT media_player 平台，改用组合实体（button/number/sensor） */
    snprintf(T_DISC_BTN_PLAY,  sizeof(T_DISC_BTN_PLAY),  "homeassistant/button/%s/play/config", g_devid);
    snprintf(T_DISC_BTN_PAUSE, sizeof(T_DISC_BTN_PAUSE), "homeassistant/button/%s/pause/config", g_devid);
    snprintf(T_DISC_BTN_STOP,  sizeof(T_DISC_BTN_STOP),  "homeassistant/button/%s/stop/config", g_devid);
    snprintf(T_DISC_NUM_VOL,   sizeof(T_DISC_NUM_VOL),   "homeassistant/number/%s/volume/config", g_devid);
    snprintf(T_DISC_SEN_PB,    sizeof(T_DISC_SEN_PB),    "homeassistant/sensor/%s/playback/config", g_devid);
    snprintf(T_DISC_SEN_BTN,   sizeof(T_DISC_SEN_BTN),   "homeassistant/sensor/%s/button/config", g_devid);
    /* v2：上一曲/播放暂停/下一曲 + 当前曲目（旧 play/pause/stop 三键废弃） */
    snprintf(T_DISC_BTN_TOG,   sizeof(T_DISC_BTN_TOG),   "homeassistant/button/%s/toggle/config", g_devid);
    snprintf(T_DISC_BTN_PREV,  sizeof(T_DISC_BTN_PREV),  "homeassistant/button/%s/prev/config", g_devid);
    snprintf(T_DISC_BTN_NEXT,  sizeof(T_DISC_BTN_NEXT),  "homeassistant/button/%s/next/config", g_devid);
    snprintf(T_DISC_SEN_TRACK, sizeof(T_DISC_SEN_TRACK), "homeassistant/sensor/%s/track/config", g_devid);
}

/* ---------------- 日志 ---------------- */
static pthread_mutex_t g_logmtx = PTHREAD_MUTEX_INITIALIZER;
static void logmsg(const char *fmt, ...)
{
    char ts[32];
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", &tmv);
    pthread_mutex_lock(&g_logmtx);
    printf("[%s] ", ts);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
    pthread_mutex_unlock(&g_logmtx);
}

/* ---------------- MQTT 底层 ---------------- */
static int g_sock = -1;                       /* 受 g_sockmtx 保护 */
static pthread_mutex_t g_sockmtx = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_mqtt_connected = 0;
static char g_clientid[80];

static int sendall(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    while (n > 0) {
        ssize_t r = send(fd, p, n, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int read_exact(int fd, void *buf, size_t n)
{
    unsigned char *p = buf;
    while (n > 0) {
        ssize_t r = recv(fd, p, n, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (r == 0) return -1;               /* 对端关闭 */
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int read_varint(int fd, int *out)
{
    int mult = 1, val = 0, i;
    unsigned char b;
    for (i = 0; i < 4; i++) {
        if (read_exact(fd, &b, 1) != 0) return -1;
        val += (b & 0x7F) * mult;
        if (!(b & 0x80)) { *out = val; return 0; }
        mult *= 128;
    }
    return -1;
}

static int put_varint(unsigned char *buf, int len)
{
    int i = 0;
    do {
        unsigned char d = (unsigned char)(len % 128);
        len /= 128;
        if (len > 0) d |= 0x80;
        buf[i++] = d;
    } while (len > 0 && i < 4);
    return i;
}

/* 加锁发送（线程安全，断线时静默丢弃） */
static int mqtt_publish(const char *topic, const char *payload, int retain)
{
    unsigned char body[2048], hdr[8];
    int tl = (int)strlen(topic), pl = (int)strlen(payload);
    if (tl + pl + 8 > (int)sizeof(body)) return -1;
    body[0] = (unsigned char)(tl >> 8);
    body[1] = (unsigned char)(tl & 0xFF);
    memcpy(body + 2, topic, (size_t)tl);
    memcpy(body + 2 + tl, payload, (size_t)pl);
    int bl = 2 + tl + pl;
    hdr[0] = (unsigned char)(0x30 | (retain ? 1 : 0));
    int hl = 1;
    hl += put_varint(hdr + 1, bl);

    pthread_mutex_lock(&g_sockmtx);
    int fd = g_sock, rc = -1;
    if (fd >= 0 && g_mqtt_connected)
        rc = (sendall(fd, hdr, (size_t)hl) == 0 && sendall(fd, body, (size_t)bl) == 0) ? 0 : -1;
    pthread_mutex_unlock(&g_sockmtx);
    return rc;
}

static int mqtt_publish_fmt(const char *topic, int retain, const char *fmt, ...)
{
    char payload[1536];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(payload, sizeof(payload), fmt, ap);
    va_end(ap);
    return mqtt_publish(topic, payload, retain);
}

static int tcp_connect_host(const char *host, int port)
{
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%d", port);
    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0) return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
    return fd;
}

static int mqtt_send_connect(int fd)
{
    unsigned char body[512];
    int p = 0;
    /* variable header: MQTT level4 */
    body[p++] = 0x00; body[p++] = 0x04;
    memcpy(body + p, "MQTT", 4); p += 4;
    body[p++] = 0x04;                     /* level 4 */
    int clen = (int)strlen(g_clientid);
    int flags = 0x02;                     /* clean session */
    int ul = (int)strlen(g_user), pl = (int)strlen(g_pass);
    int wt = (int)strlen(T_STATE_AVAIL);
    if (wt)  flags |= 0x04;               /* will */
    if (ul)  flags |= 0x80;
    if (pl)  flags |= 0x40;
    body[p++] = (unsigned char)flags;
    body[p++] = 0x00; body[p++] = 60;     /* keepalive 60s */
    body[p++] = (unsigned char)(clen >> 8); body[p++] = (unsigned char)(clen & 0xFF);
    memcpy(body + p, g_clientid, (size_t)clen); p += clen;
    if (wt) {                              /* will: offline */
        body[p++] = 0x00; body[p++] = (unsigned char)(wt & 0xFF);
        memcpy(body + p, T_STATE_AVAIL, (size_t)wt); p += wt;
        body[p++] = 0x00; body[p++] = 0x07;
        memcpy(body + p, "offline", 7); p += 7;
    }
    if (ul) {
        body[p++] = (unsigned char)(ul >> 8); body[p++] = (unsigned char)(ul & 0xFF);
        memcpy(body + p, g_user, (size_t)ul); p += ul;
    }
    if (pl) {
        body[p++] = (unsigned char)(pl >> 8); body[p++] = (unsigned char)(pl & 0xFF);
        memcpy(body + p, g_pass, (size_t)pl); p += pl;
    }
    unsigned char hdr[8];
    hdr[0] = 0x10;
    int hl = 1;
    hl += put_varint(hdr + 1, p);
    if (sendall(fd, hdr, (size_t)hl) != 0) return -1;
    if (sendall(fd, body, (size_t)p) != 0) return -1;
    /* CONNACK */
    unsigned char resp[4];
    if (read_exact(fd, resp, 4) != 0) return -1;
    if (resp[0] != 0x20 || resp[1] != 0x02) return -1;
    return (resp[3] == 0) ? 0 : -1;       /* rc=0 接受 */
}

static int mqtt_send_subscribe(int fd, const char *topic)
{
    unsigned char body[256];
    int tl = (int)strlen(topic), p = 0;
    body[p++] = 0x00; body[p++] = 0x01;   /* msg id */
    body[p++] = (unsigned char)(tl >> 8); body[p++] = (unsigned char)(tl & 0xFF);
    memcpy(body + p, topic, (size_t)tl); p += tl;
    body[p++] = 0x00;                     /* qos0 */
    unsigned char hdr[8];
    hdr[0] = 0x82;
    int hl = 1;
    hl += put_varint(hdr + 1, p);
    if (sendall(fd, hdr, (size_t)hl) != 0) return -1;
    return sendall(fd, body, (size_t)p);
}

static int mqtt_send_ping(int fd)
{
    unsigned char ping[2] = {0xC0, 0x00};
    return sendall(fd, ping, 2);
}

/* ---------------- 播放引擎 ---------------- */
static pthread_mutex_t g_pbmtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_pbcond = PTHREAD_COND_INITIALIZER;
static char g_joburl[768];
static int  g_hasjob = 0;
static volatile int g_abort = 0;
static volatile int g_pbstate = 0;        /* 0 idle 1 playing 2 paused */
static pid_t g_aplay_pid = 0;
static int   g_aplay_fd = -1;
static char  g_lasturl[768] = "";

static void publish_pbstate(int st)
{
    g_pbstate = st;
    mqtt_publish(T_STATE_PB, st == 1 ? "playing" : (st == 2 ? "paused" : "idle"), 1);
}

static void kill_aplay(void)
{
    if (g_aplay_fd >= 0) {
        close(g_aplay_fd);
        g_aplay_fd = -1;
    }
    if (g_aplay_pid > 0) {
        kill(g_aplay_pid, SIGKILL);
        waitpid(g_aplay_pid, NULL, 0);
        g_aplay_pid = 0;
    }
}

static int write_all_fd(int fd, const void *buf, size_t n)
{
    const unsigned char *p = buf;
    while (n > 0) {
        ssize_t r = write(fd, p, n);
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;                    /* aplay 退出/管道断 */
        }
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* fork/exec aplay，返回写管道 fd */
static pid_t spawn_aplay(int *out_fd, int raw, int ch, int rate)
{
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(fds[0], 0);
        close(fds[0]);
        close(fds[1]);
        char cbuf[16], rbuf[16];
        snprintf(cbuf, sizeof(cbuf), "%d", ch);
        snprintf(rbuf, sizeof(rbuf), "%d", rate);
        if (raw)
            execlp("aplay", "aplay", "-q", "-D", g_alsadev,
                   "-t", "raw", "-f", "S16_LE", "-c", cbuf, "-r", rbuf, "-", (char *)NULL);
        else
            execlp("aplay", "aplay", "-q", "-D", g_alsadev, "-", (char *)NULL);
        _exit(127);
    }
    close(fds[0]);
    *out_fd = fds[1];
    return pid;
}

/* 极简 HTTP GET：返回已连 socket（头已读完），body 由调用者读 */
static int http_open(const char *url)
{
    if (strncmp(url, "http://", 7) != 0) {
        logmsg("仅支持 http:// URL: %s", url);
        return -1;
    }
    char host[128] = "", path[512] = "/";
    int port = 80;
    const char *p = url + 7;
    const char *slash = strchr(p, '/');
    if (slash) {
        size_t hl = (size_t)(slash - p);
        if (hl >= sizeof(host)) hl = sizeof(host) - 1;
        memcpy(host, p, hl);
        host[hl] = 0;
        snprintf(path, sizeof(path), "%s", slash);
    } else {
        snprintf(host, sizeof(host), "%s", p);
    }
    char *colon = strrchr(host, ':');
    if (colon) { port = atoi(colon + 1); *colon = 0; }
    if (!host[0]) return -1;

    int fd = tcp_connect_host(host, port);
    if (fd < 0) {
        logmsg("连接失败: %s:%d", host, port);
        return -1;
    }
    char req[1024];
    snprintf(req, sizeof(req),
             "GET %s HTTP/1.1\r\nHost: %s:%d\r\nUser-Agent: xbridge/1.0\r\n"
             "Connection: close\r\nAccept: */*\r\n\r\n", path, host, port);
    if (sendall(fd, req, strlen(req)) != 0) {
        close(fd);
        return -1;
    }
    /* 读响应头 */
    char hdr[4096];
    size_t fill = 0;
    while (fill < sizeof(hdr) - 1) {
        char c;
        ssize_t r = recv(fd, &c, 1, 0);
        if (r <= 0) break;
        hdr[fill++] = c;
        if (fill >= 4 && !memcmp(hdr + fill - 4, "\r\n\r\n", 4)) break;
    }
    hdr[fill] = 0;
    int status = 0;
    if (sscanf(hdr, "HTTP/%*d.%*d %d", &status) != 1) {
        close(fd);
        return -1;
    }
    if (status != 200) {
        logmsg("HTTP %d: %s", status, url);
        close(fd);
        return -1;
    }
    if (g_verbose) logmsg("HTTP 200 %s", url);
    return fd;                              /* body 从这个 socket 继续 */
}

static int http_read(int fd, void *buf, size_t n)
{
    ssize_t r;
    do {
        r = recv(fd, buf, n, 0);
    } while (r < 0 && errno == EINTR);
    return (int)r;
}

/* 播放一个 URL（阻塞直至完成/中止），由 playback 线程调用 */
static void do_play(const char *url)
{
    g_abort = 0;
    int fd = http_open(url);
    if (fd < 0) {
        publish_pbstate(0);
        return;
    }
    /* 嗅探前几字节判断格式 */
    unsigned char peek[16];
    size_t pfill = 0;
    while (pfill < 4) {
        int r = http_read(fd, peek + pfill, 4 - pfill);
        if (r <= 0) break;
        pfill += (size_t)r;
    }
    if (pfill == 0) {
        close(fd);
        publish_pbstate(0);
        return;
    }
    int is_mp3 = 0;
    if (pfill >= 3 && !memcmp(peek, "ID3", 3)) is_mp3 = 1;
    else if (peek[0] == 0xFF && (peek[1] & 0xE0) == 0xE0) is_mp3 = 1;
    else if (pfill >= 4 && !memcmp(peek, "RIFF", 4)) is_mp3 = 0;
    else is_mp3 = 1;                        /* 默认按 MP3（HA TTS 代理是 mp3） */

#ifdef HAVE_MINIMP3
    if (is_mp3) {
        static mp3dec_t dec;
        mp3dec_init(&dec);
        static unsigned char buf[64 * 1024];
        size_t fill = 0;
        memcpy(buf, peek, pfill);
        fill = pfill;
        int eof = 0, started = 0;
        publish_pbstate(1);
        while (!g_abort) {
            if (!eof && fill < sizeof(buf)) {
                int r = http_read(fd, buf + fill, sizeof(buf) - fill);
                if (r > 0) { fill += (size_t)r; continue; }
                if (r == 0) eof = 1;
                else break;
            }
            mp3dec_frame_info_t info;
            short pcm[MINIMP3_MAX_SAMPLES_PER_FRAME];
            int samples = mp3dec_decode_frame(&dec, buf, (int)fill, pcm, &info);
            if (info.frame_bytes == 0) {
                if (eof || fill == 0) break;
                memmove(buf, buf + 1, fill - 1);   /* 重新同步 */
                fill--;
                continue;
            }
            if (samples > 0) {
                if (!started) {
                    g_aplay_pid = spawn_aplay(&g_aplay_fd, 1, info.channels, info.hz);
                    if (g_aplay_pid <= 0) { logmsg("aplay 启动失败"); break; }
                    started = 1;
                    logmsg("播放中: %s (%dHz %dch)", url, info.hz, info.channels);
                }
                if (write_all_fd(g_aplay_fd, pcm,
                                 (size_t)samples * (size_t)info.channels * 2) != 0) {
                    logmsg("aplay 管道断开");
                    break;
                }
            }
            if (info.frame_bytes > 0) {
                fill -= (size_t)info.frame_bytes;
                memmove(buf, buf + info.frame_bytes, fill);
            }
            if (eof && fill == 0) break;
        }
    } else
#endif
    {
        /* WAV：这版 aplay 从管道读不了头，必须自己解析后按 raw 模式喂 */
        unsigned char hdr[128];
        size_t hfill = 0;
        memcpy(hdr, peek, pfill);
        hfill = pfill;
        while (hfill < sizeof(hdr)) {
            int r = http_read(fd, hdr + hfill, sizeof(hdr) - hfill);
            if (r > 0) { hfill += (size_t)r; continue; }
            break;                          /* EOF 或出错，尽力而为 */
        }
        int ch = 2, rate = 44100, bits = 16, dataoff = -1;
        if (hfill >= 44 && !memcmp(hdr, "RIFF", 4) && !memcmp(hdr + 8, "WAVE", 4)) {
            size_t pos = 12;
            while (pos + 8 <= hfill) {
                unsigned csz = (unsigned)hdr[pos+4] | ((unsigned)hdr[pos+5]<<8)
                             | ((unsigned)hdr[pos+6]<<16) | ((unsigned)hdr[pos+7]<<24);
                if (!memcmp(hdr + pos, "fmt ", 4) && pos + 24 <= hfill) {
                    ch   = hdr[pos+10] | (hdr[pos+11]<<8);
                    rate = (unsigned)hdr[pos+12] | ((unsigned)hdr[pos+13]<<8)
                         | ((unsigned)hdr[pos+14]<<16) | ((unsigned)hdr[pos+15]<<24);
                    bits = hdr[pos+22] | (hdr[pos+23]<<8);
                }
                if (!memcmp(hdr + pos, "data", 4)) {
                    dataoff = (int)(pos + 8);
                    break;
                }
                pos += 8 + csz + (csz & 1);
            }
        }
        if (dataoff < 0 || bits != 16 || ch < 1 || ch > 8 || rate < 8000 || rate > 192000) {
            logmsg("WAV 头无效(off=%d bits=%d ch=%d rate=%d)，放弃", dataoff, bits, ch, rate);
            close(fd);
            publish_pbstate(0);
            return;
        }
        g_aplay_pid = spawn_aplay(&g_aplay_fd, 1, ch, rate);
        if (g_aplay_pid <= 0) {
            logmsg("aplay 启动失败");
            close(fd);
            publish_pbstate(0);
            return;
        }
        publish_pbstate(1);
        logmsg("播放中(WAV %dHz %dch): %s", rate, ch, url);
        if (write_all_fd(g_aplay_fd, hdr + dataoff, hfill - (size_t)dataoff) != 0) goto wav_done;
        unsigned char chunk[16 * 1024];
        while (!g_abort) {
            int r = http_read(fd, chunk, sizeof(chunk));
            if (r > 0) {
                if (write_all_fd(g_aplay_fd, chunk, (size_t)r) != 0) break;
            } else if (r == 0) break;
            else if (errno == EINTR) continue;
            else break;
        }
    wav_done:
        ;
    }
    close(fd);
    kill_aplay();
    if (!g_abort) publish_pbstate(0);
    else publish_pbstate(0);
}

static void playback_submit(const char *url)
{
    pthread_mutex_lock(&g_pbmtx);
    snprintf(g_joburl, sizeof(g_joburl), "%s", url);
    snprintf(g_lasturl, sizeof(g_lasturl), "%s", url);
    g_hasjob = 1;
    g_abort = 1;                            /* 打断当前 */
    pthread_cond_signal(&g_pbcond);
    pthread_mutex_unlock(&g_pbmtx);
}

static void playback_stop(void)
{
    pthread_mutex_lock(&g_pbmtx);
    g_abort = 1;
    g_hasjob = 0;
    pthread_mutex_unlock(&g_pbmtx);
    kill_aplay();
    publish_pbstate(0);
}

static void playback_pause_resume(int pause)
{
    if (g_aplay_pid > 0)
        kill(g_aplay_pid, pause ? SIGSTOP : SIGCONT);
    publish_pbstate(pause ? 2 : 1);
}

/* ---------------- 歌单 ---------------- */
static void url_decode(const char *src, char *dst, size_t dstsz)
{
    size_t o = 0;
    for (const char *p = src; *p && o + 1 < dstsz; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) && isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], 0 };
            dst[o++] = (char)strtol(hex, NULL, 16);
            p += 2;
        } else if (*p == '+') {
            dst[o++] = ' ';
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = 0;
}

/* 从音乐目录 HTTP 列表解析 MP3（busybox httpd 目录页 href="xxx.mp3"） */
static void fetch_playlist(void)
{
    if (!g_playlist_url[0]) return;
    int fd = http_open(g_playlist_url);
    if (fd < 0) { logmsg("歌单目录拉取失败: %s", g_playlist_url); return; }
    static char buf[64 * 1024];
    size_t fill = 0;
    while (fill < sizeof(buf) - 1) {
        int r = http_read(fd, buf + fill, sizeof(buf) - 1 - fill);
        if (r > 0) { fill += (size_t)r; continue; }
        break;
    }
    close(fd);
    buf[fill] = 0;

    for (int i = 0; i < g_plcnt; i++) { free(g_pl[i]); g_pl[i] = NULL; }
    g_plcnt = 0;
    g_plidx = -1;

    char base[512];
    snprintf(base, sizeof(base), "%s", g_playlist_url);
    char *slash = strrchr(base, '/');
    if (slash) slash[1] = 0; else strncat(base, "/", sizeof(base) - strlen(base) - 1);

    char *p = buf;
    while ((p = strstr(p, "href=\"")) != NULL && g_plcnt < PL_MAX) {
        p += 6;
        char *e = strchr(p, '"');
        if (!e) break;
        size_t len = (size_t)(e - p);
        if (len > 4 && len < 300 && strncasecmp(e - 4, ".mp3", 4) == 0
            && *p != '/' && !strchr(p, '?')) {
            size_t need = strlen(base) + len + 2;
            char *url = malloc(need);
            if (url) {
                snprintf(url, need, "%s%.*s", base, (int)len, p);
                g_pl[g_plcnt++] = url;
            }
        }
        p = e + 1;
    }
    logmsg("歌单就绪: %d 首（来自 %s）", g_plcnt, g_playlist_url);
}

static void publish_track(void)
{
    char name[256] = "";
    if (g_plidx >= 0 && g_plidx < g_plcnt && g_pl[g_plidx]) {
        const char *u = g_pl[g_plidx];
        const char *slash = strrchr(u, '/');
        url_decode(slash ? slash + 1 : u, name, sizeof(name));
    }
    mqtt_publish(T_STATE_TRACK, name, 1);
}

static void playlist_play_index(int idx)
{
    if (g_plcnt == 0) { logmsg("歌单为空"); return; }
    if (idx < 0) idx = g_plcnt - 1;               /* 环绕 */
    if (idx >= g_plcnt) idx = 0;
    g_plidx = idx;
    publish_track();
    logmsg("播放 #%d/%d: %s", g_plidx + 1, g_plcnt, g_pl[g_plidx]);
    playback_submit(g_pl[g_plidx]);
}

static void *playback_thread(void *arg)
{
    (void)arg;
    while (g_run) {
        pthread_mutex_lock(&g_pbmtx);
        while (!g_hasjob && g_run)
            pthread_cond_wait(&g_pbcond, &g_pbmtx);
        int has = g_hasjob;
        g_hasjob = 0;
        char url[768];
        if (has) snprintf(url, sizeof(url), "%s", g_joburl);
        pthread_mutex_unlock(&g_pbmtx);
        if (!g_run) break;
        if (has) {
            kill_aplay();
            do_play(url);
            /* 自然播完（未被新任务打断/停止）且歌单未到尾 → 自动连播 */
            if (!g_abort && !g_hasjob && g_plcnt > 0
                && g_plidx >= 0 && g_plidx < g_plcnt - 1) {
                g_plidx++;
                publish_track();
                logmsg("自动连播 #%d/%d", g_plidx + 1, g_plcnt);
                playback_submit(g_pl[g_plidx]);
            }
        }
    }
    return NULL;
}

/* ---------------- 音量 ---------------- */
/* 注意：本机 amixer 的 sset 找不到 softvol 控件，必须用 cset name=...；
 * Master Playback Volume 实际量程 0-99，把 0-100% 映射过去。 */
static int g_volpct = -1;                /* 最近设置的音量百分比，-1 = 尚未设置 */
static void set_volume(int pct)
{
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    g_volpct = pct;
    int raw = pct * 99 / 100;
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "amixer -c 0 cset name='Master Playback Volume' %d,%d >/dev/null 2>&1",
             raw, raw);
    system(cmd);
    char s[8];
    snprintf(s, sizeof(s), "%d", pct);
    mqtt_publish(T_STATE_VOL, s, 1);
    logmsg("音量 -> %d%% (raw %d)", pct, raw);
}

/* ---------------- 命令分发 ---------------- */
static void handle_command(const char *topic, const unsigned char *payload, int plen)
{
    char val[768];
    int n = plen < (int)sizeof(val) - 1 ? plen : (int)sizeof(val) - 1;
    memcpy(val, payload, (size_t)n);
    val[n] = 0;
    /* 去尾部空白 */
    while (n > 0 && (val[n - 1] == '\n' || val[n - 1] == '\r' || val[n - 1] == ' ')) val[--n] = 0;

    if (g_verbose) logmsg("CMD %s: %s", topic, val);

    if (!strcmp(topic, T_CMD_TTS) || !strcmp(topic, T_CMD_PLAY)) {
        if (!strncmp(val, "http://", 7) || !strncmp(val, "https://", 8))
            playback_submit(val);
        else
            logmsg("忽略非 http URL: %s", val);
    } else if (!strcmp(topic, T_CMD_STOP)) {
        playback_stop();
    } else if (!strcmp(topic, T_CMD_VOL)) {
        double f = atof(val);
        if (f <= 1.0 && strchr(val, '.')) f *= 100.0;   /* 兼容 0.0-1.0 */
        set_volume((int)(f + 0.5));
    } else if (!strcmp(topic, T_CMD_MEDIA)) {
        if (!strcmp(val, "stop")) playback_stop();
        else if (!strcmp(val, "pause")) playback_pause_resume(1);
        else if (!strcmp(val, "play")) {
            if (g_pbstate == 2) playback_pause_resume(0);
            else if (g_lasturl[0]) playback_submit(g_lasturl);
        }
        else if (!strcmp(val, "toggle")) {
            if (g_pbstate == 1) playback_pause_resume(1);
            else if (g_pbstate == 2) playback_pause_resume(0);
            else playlist_play_index(g_plidx >= 0 ? g_plidx : 0);
        }
        else if (!strcmp(val, "prev")) playlist_play_index(g_plidx - 1);
        else if (!strcmp(val, "next")) playlist_play_index(g_plidx + 1);
        else if (!strcmp(val, "reload")) { fetch_playlist(); publish_track(); }
        /* 兼容旧命令 next_track/previous_track */
        else if (!strcmp(val, "next_track")) playlist_play_index(g_plidx + 1);
        else if (!strcmp(val, "previous_track")) playlist_play_index(g_plidx - 1);
    } else if (!strcmp(topic, T_CMD_EXEC) && g_allow_exec) {
        logmsg("EXEC: %s", val);
        system(val);
    } else if (g_verbose) {
        logmsg("未处理 topic: %s", topic);
    }
}

/* ---------------- 网络线程 ---------------- */
static void publish_online_and_discovery(void)
{
    mqtt_publish(T_STATE_AVAIL, "online", 1);
    /* 清理 v1 旧 discovery（空 retained 载荷会让 HA 移除对应实体） */
    mqtt_publish(T_DISC_MP, "", 1);
    mqtt_publish(T_DISC_BTN_PLAY, "", 1);
    mqtt_publish(T_DISC_BTN_PAUSE, "", 1);
    mqtt_publish(T_DISC_BTN_STOP, "", 1);
    mqtt_publish(T_DISC_SEN_BTN, "", 1);

    /* v2 组合实体：上一曲 / 播放暂停(合一键) / 下一曲 + 音量 + 状态/曲目 */
    mqtt_publish_fmt(T_DISC_BTN_TOG, 1,
        "{\"name\":\"播放/暂停\",\"unique_id\":\"%s_btn_toggle\","
        "\"command_topic\":\"%s/cmd/media\",\"payload_press\":\"toggle\","
        "\"availability_topic\":\"%s/state/available\","
        "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
        "\"icon\":\"mdi:play-pause\",\"device\":{\"identifiers\":[\"%s\"],\"name\":\"小雅HOME\",\"model\":\"XYOS-RK3308\",\"manufacturer\":\"xbridge\"}}",
        g_devid, g_prefix, g_prefix, g_devid);
    mqtt_publish_fmt(T_DISC_BTN_PREV, 1,
        "{\"name\":\"上一曲\",\"unique_id\":\"%s_btn_prev\","
        "\"command_topic\":\"%s/cmd/media\",\"payload_press\":\"prev\","
        "\"availability_topic\":\"%s/state/available\","
        "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
        "\"icon\":\"mdi:skip-previous\",\"device\":{\"identifiers\":[\"%s\"],\"name\":\"小雅HOME\",\"model\":\"XYOS-RK3308\",\"manufacturer\":\"xbridge\"}}",
        g_devid, g_prefix, g_prefix, g_devid);
    mqtt_publish_fmt(T_DISC_BTN_NEXT, 1,
        "{\"name\":\"下一曲\",\"unique_id\":\"%s_btn_next\","
        "\"command_topic\":\"%s/cmd/media\",\"payload_press\":\"next\","
        "\"availability_topic\":\"%s/state/available\","
        "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
        "\"icon\":\"mdi:skip-next\",\"device\":{\"identifiers\":[\"%s\"],\"name\":\"小雅HOME\",\"model\":\"XYOS-RK3308\",\"manufacturer\":\"xbridge\"}}",
        g_devid, g_prefix, g_prefix, g_devid);
    mqtt_publish_fmt(T_DISC_NUM_VOL, 1,
        "{\"name\":\"音量\",\"unique_id\":\"%s_num_volume\","
        "\"command_topic\":\"%s/cmd/volume\","
        "\"state_topic\":\"%s/state/volume\","
        "\"min\":0,\"max\":100,\"step\":1,\"mode\":\"slider\","
        "\"availability_topic\":\"%s/state/available\","
        "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
        "\"icon\":\"mdi:volume-high\",\"device\":{\"identifiers\":[\"%s\"],\"name\":\"小雅HOME\",\"model\":\"XYOS-RK3308\",\"manufacturer\":\"xbridge\"}}",
        g_devid, g_prefix, g_prefix, g_prefix, g_devid);
    mqtt_publish_fmt(T_DISC_SEN_PB, 1,
        "{\"name\":\"播放状态\",\"unique_id\":\"%s_sen_playback\","
        "\"state_topic\":\"%s/state/playback\","
        "\"availability_topic\":\"%s/state/available\","
        "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
        "\"icon\":\"mdi:speaker\",\"device\":{\"identifiers\":[\"%s\"],\"name\":\"小雅HOME\",\"model\":\"XYOS-RK3308\",\"manufacturer\":\"xbridge\"}}",
        g_devid, g_prefix, g_prefix, g_devid);
    mqtt_publish_fmt(T_DISC_SEN_TRACK, 1,
        "{\"name\":\"当前曲目\",\"unique_id\":\"%s_sen_track\","
        "\"state_topic\":\"%s/state/track\","
        "\"availability_topic\":\"%s/state/available\","
        "\"payload_available\":\"online\",\"payload_not_available\":\"offline\","
        "\"icon\":\"mdi:music-note\",\"device\":{\"identifiers\":[\"%s\"],\"name\":\"小雅HOME\",\"model\":\"XYOS-RK3308\",\"manufacturer\":\"xbridge\"}}",
        g_devid, g_prefix, g_prefix, g_devid);
    publish_track();
}

/* 周期状态刷新：MQTT 保留消息只对新订阅会话重放；若 HA 在某条消息发出的
 * 瞬间恰好重连，会错过该消息（曾导致实体卡在“不可用”）。每 60s 把全部
 * 状态重发一遍（均带 retain），任何一端断线重连后 1 分钟内自愈。 */
static void publish_state_refresh(void)
{
    mqtt_publish(T_STATE_AVAIL, "online", 1);
    mqtt_publish(T_STATE_PB,
                 g_pbstate == 1 ? "playing" : (g_pbstate == 2 ? "paused" : "idle"), 1);
    if (g_volpct >= 0) {
        char s[8];
        snprintf(s, sizeof(s), "%d", g_volpct);
        mqtt_publish(T_STATE_VOL, s, 1);
    }
    publish_track();
}

static void *net_thread(void *arg)
{
    (void)arg;
    int backoff = 2;
    time_t last_ping = 0;
    time_t last_state = 0;
    while (g_run) {
        int fd = tcp_connect_host(g_broker, g_port);
        if (fd < 0) {
            if (g_verbose) logmsg("连接 %s:%d 失败，%ds 后重试", g_broker, g_port, backoff);
            for (int i = 0; i < backoff && g_run; i++) sleep(1);
            backoff = backoff < 30 ? backoff * 2 : 30;
            continue;
        }
        backoff = 2;
        if (mqtt_send_connect(fd) != 0) {
            logmsg("MQTT CONNECT 被拒（检查用户名密码）");
            close(fd);
            for (int i = 0; i < 5 && g_run; i++) sleep(1);
            continue;
        }
        pthread_mutex_lock(&g_sockmtx);
        g_sock = fd;
        pthread_mutex_unlock(&g_sockmtx);
        g_mqtt_connected = 1;
        logmsg("MQTT 已连接 %s:%d", g_broker, g_port);
        if (g_playlist_url[0] && g_plcnt == 0)
            fetch_playlist();               /* 启动时拉取失败则重连后补拉 */
        publish_online_and_discovery();
        mqtt_send_subscribe(fd, T_CMD_TTS);
        mqtt_send_subscribe(fd, T_CMD_PLAY);
        mqtt_send_subscribe(fd, T_CMD_STOP);
        mqtt_send_subscribe(fd, T_CMD_VOL);
        mqtt_send_subscribe(fd, T_CMD_MEDIA);
        if (g_allow_exec) mqtt_send_subscribe(fd, T_CMD_EXEC);
        last_ping = time(NULL);
        last_state = time(NULL);

        /* 读取循环 */
        int alive = 1;
        while (alive && g_run) {
            struct pollfd pf = {fd, POLLIN, 0};
            int r = poll(&pf, 1, 1000);
            if (!g_run) break;
            if (r == 0) {
                if (time(NULL) - last_ping >= 25) {
                    if (mqtt_send_ping(fd) != 0) break;
                    last_ping = time(NULL);
                }
                if (time(NULL) - last_state >= 60) {
                    publish_state_refresh();
                    if (g_verbose) logmsg("状态周期重发(60s)");
                    last_state = time(NULL);
                }
                continue;
            }
            if (r < 0) {
                if (errno == EINTR) continue;
                break;
            }
            unsigned char b1;
            if (read_exact(fd, &b1, 1) != 0) break;
            int rem;
            if (read_varint(fd, &rem) != 0) break;
            unsigned char body[4096];
            if (rem > (int)sizeof(body)) {
                /* 超长包：丢弃内容 */
                int left = rem;
                char tmp[512];
                while (left > 0) {
                    int c = left < (int)sizeof(tmp) ? left : (int)sizeof(tmp);
                    if (read_exact(fd, tmp, (size_t)c) != 0) { alive = 0; break; }
                    left -= c;
                }
                continue;
            }
            if (rem > 0 && read_exact(fd, body, (size_t)rem) != 0) break;
            unsigned char t = (unsigned char)(b1 & 0xF0);
            if (t == 0x30) {                  /* PUBLISH */
                if (rem < 2) break;
                int tl = (body[0] << 8) | body[1];
                if (tl + 2 > rem) break;
                char topic[128];
                int cpl = tl < (int)sizeof(topic) - 1 ? tl : (int)sizeof(topic) - 1;
                memcpy(topic, body + 2, (size_t)cpl);
                topic[cpl] = 0;
                int qos = (b1 >> 1) & 0x03;
                int poff = 2 + tl + (qos > 0 ? 2 : 0);
                if (poff <= rem)
                    handle_command(topic, body + poff, rem - poff);
            }
            /* CONNACK/SUBACK/PINGRESP/PUBACK 忽略 */
        }
        g_mqtt_connected = 0;
        pthread_mutex_lock(&g_sockmtx);
        if (g_sock == fd) g_sock = -1;
        pthread_mutex_unlock(&g_sockmtx);
        close(fd);
        if (g_run) {
            logmsg("MQTT 断开，重连中...");
            publish_pbstate(0);
        }
    }
    return NULL;
}

/* ---------------- 按键线程 ---------------- */
typedef struct {
    int64_t tv_sec;
    int64_t tv_usec;
    uint16_t type;
    uint16_t code;
    int32_t value;
} in_ev_t;

static void *button_thread(void *arg)
{
    (void)arg;
    struct pollfd pfds[16];
    int n = 0;
    DIR *d = opendir("/dev/input");
    if (!d) {
        logmsg("无 /dev/input，按键不可用");
        return NULL;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && n < 16) {
        if (strncmp(ent->d_name, "event", 5) != 0) continue;
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/%s", ent->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        pfds[n].fd = fd;
        pfds[n].events = POLLIN;
        pfds[n].revents = 0;
        n++;
    }
    closedir(d);
    if (n == 0) {
        logmsg("未找到输入设备，按键不可用");
        return NULL;
    }
    logmsg("监听 %d 个输入设备（按键）", n);
    in_ev_t ev;
    while (g_run) {
        int r = poll(pfds, (nfds_t)n, 1000);
        if (r <= 0) continue;
        for (int i = 0; i < n; i++) {
            if (!(pfds[i].revents & POLLIN)) continue;
            ssize_t rd = read(pfds[i].fd, &ev, sizeof(ev));
            if (rd != (ssize_t)sizeof(ev)) continue;
            if (ev.type != 1) continue;    /* EV_KEY */
            if (g_probe) {
                printf("BUTTON type=1 code=%u value=%d\n", ev.code, ev.value);
                fflush(stdout);
                continue;
            }
            if (ev.value == 1 || ev.value == 0) {
                int rc = mqtt_publish_fmt(T_EV_BUTTON, 0, "{\"code\":%u,\"value\":%d}",
                                          ev.code, ev.value);
                logmsg("按键 code=%u value=%d 发布%s", ev.code, ev.value,
                       rc == 0 ? "OK" : "失败");
            }
        }
    }
    for (int i = 0; i < n; i++) close(pfds[i].fd);
    return NULL;
}

/* ---------------- main ---------------- */
static void on_signal(int sig)
{
    (void)sig;
    g_run = 0;
}

static void usage(void)
{
    printf(
        "xbridge — 小雅HOME 本地 MQTT 桥接\n"
        "用法: xbridge -b <broker_ip> [选项]\n"
        "  -b <ip>        MQTT broker 地址（必填）\n"
        "  -p <port>      端口，默认 1883\n"
        "  -u <user>      用户名\n"
        "  -P <pass>      密码\n"
        "  -t <prefix>    主题前缀，默认 xiaoya\n"
        "  -i <id>        设备 id，默认 xiaoya_home\n"
        "  -d <alsadev>   ALSA 播放设备，默认 real_playback\n"
        "  -l <url>       歌单目录 URL（音乐服务器目录页，只收 MP3）\n"
        "                 上一曲/下一曲/播完自动连播依赖此选项\n"
        "  -e             允许 cmd/exec 远程执行（默认关闭）\n"
        "  -no-pmdiscovery  不在 discovery 中带 play_media_topic\n"
        "  -probe         按键探测模式（打印 code，不连 broker）\n"
        "  -v             详细日志\n"
        "  -h             帮助\n");
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-b") && i + 1 < argc) snprintf(g_broker, sizeof(g_broker), "%s", argv[++i]);
        else if (!strcmp(argv[i], "-p") && i + 1 < argc) g_port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-u") && i + 1 < argc) snprintf(g_user, sizeof(g_user), "%s", argv[++i]);
        else if (!strcmp(argv[i], "-P") && i + 1 < argc) snprintf(g_pass, sizeof(g_pass), "%s", argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) snprintf(g_prefix, sizeof(g_prefix), "%s", argv[++i]);
        else if (!strcmp(argv[i], "-i") && i + 1 < argc) snprintf(g_devid, sizeof(g_devid), "%s", argv[++i]);
        else if (!strcmp(argv[i], "-d") && i + 1 < argc) snprintf(g_alsadev, sizeof(g_alsadev), "%s", argv[++i]);
        else if (!strcmp(argv[i], "-l") && i + 1 < argc) snprintf(g_playlist_url, sizeof(g_playlist_url), "%s", argv[++i]);
        else if (!strcmp(argv[i], "-e")) g_allow_exec = 1;
        else if (!strcmp(argv[i], "-no-pmdiscovery")) g_pmdiscovery = 0;
        else if (!strcmp(argv[i], "-probe")) g_probe = 1;
        else if (!strcmp(argv[i], "-v")) g_verbose = 1;
        else if (!strcmp(argv[i], "-h")) { usage(); return 0; }
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    init_topics();
    snprintf(g_clientid, sizeof(g_clientid), "xbridge-%s-%d", g_devid, (int)getpid());

    if (g_probe) {
        logmsg("按键探测模式（Ctrl+C 退出），按音箱上的按键：");
        button_thread(NULL);
        return 0;
    }
    if (!g_broker[0]) {
        usage();
        return 1;
    }

    logmsg("xbridge 启动 broker=%s:%d prefix=%s alsa=%s", g_broker, g_port, g_prefix, g_alsadev);
    if (g_playlist_url[0]) {
        logmsg("歌单目录: %s", g_playlist_url);
        fetch_playlist();
    }
    pthread_t tnet, tplay, tbtn;
    pthread_create(&tplay, NULL, playback_thread, NULL);
    pthread_create(&tbtn, NULL, button_thread, NULL);
    pthread_create(&tnet, NULL, net_thread, NULL);
    pthread_join(tnet, NULL);
    kill_aplay();
    logmsg("xbridge 退出");
    return 0;
}
