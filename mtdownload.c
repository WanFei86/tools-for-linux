// mtdownload.c
// 编译: gcc -O2 -Wall -pthread mtdownload.c -o mtdownload
// 用法: ./mtdownload <URL> <线程数> [输出文件名]

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>

#define BUF_SIZE 8192
#define MAX_THREADS 64
#define MAX_REDIRECTS 5

/* ---------- 数据结构 ---------- */

typedef struct {
    char   host[256];
    char   path[1024];   // 包含 query，例如 /a/b.zip?x=1
    int    port;
    int    https;        // 本示例仅支持 http（见文末说明）
} UrlInfo;

typedef struct {
    UrlInfo url;

    long long start;     // 本线程下载起始字节
    long long end;       // 本线程下载结束字节（含）
    int       index;     // 线程编号

    int       fd;        // 输出文件描述符
    long long file_size;

    long long downloaded; // 已下载字节（用于进度显示）
    int       ok;         // 是否成功

    pthread_mutex_t *lock;      // 用于更新总进度
    long long       *total_done;// 全局已下载
} ThreadArg;

/* ---------- 解析 URL ---------- */

static int parse_url(const char *url, UrlInfo *out)
{
    memset(out, 0, sizeof(*out));
    const char *p = url;

    if (strncasecmp(p, "http://", 7) == 0) {
        p += 7;
        out->https = 0;
        out->port = 80;
    } else if (strncasecmp(p, "https://", 8) == 0) {
        p += 8;
        out->https = 1;
        out->port = 443;   // 注意：本示例未实现 TLS
    } else {
        return -1;
    }

    // host[:port]
    const char *slash = strchr(p, '/');
    const char *host_end = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', host_end - p);

    size_t hlen = colon ? (size_t)(colon - p) : (size_t)(host_end - p);
    if (hlen == 0 || hlen >= sizeof(out->host)) return -1;
    memcpy(out->host, p, hlen);
    out->host[hlen] = '\0';

    if (colon) {
        out->port = atoi(colon + 1);
        if (out->port <= 0) return -1;
    }

    if (slash) {
        snprintf(out->path, sizeof(out->path), "%s", slash);
    } else {
        snprintf(out->path, sizeof(out->path), "/");
    }
    return 0;
}

/* ---------- 建立 TCP 连接 ---------- */

static int tcp_connect(const char *host, int port)
{
    struct addrinfo hints, *res, *rp;
    char portstr[16];
    int sockfd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", port);

    if (getaddrinfo(host, portstr, &hints, &res) != 0)
        return -1;

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        sockfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sockfd < 0) continue;
        if (connect(sockfd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(sockfd);
        sockfd = -1;
    }
    freeaddrinfo(res);
    return sockfd;
}

/* ---------- 发送 HTTP 请求 ---------- */

static int send_request(int sockfd, const UrlInfo *u,
                        long long start, long long end, int head_only)
{
    char req[2048];
    int n = snprintf(req, sizeof(req),
        "%s %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: mt-downloader/1.0\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n",
        head_only ? "HEAD" : "GET",
        u->path, u->host, u->port);

    if (!head_only && start >= 0) {
        n += snprintf(req + n, sizeof(req) - n,
                      "Range: bytes=%lld-%lld\r\n", start, end);
    }
    n += snprintf(req + n, sizeof(req) - n, "\r\n");

    return send(sockfd, req, n, 0) == n ? 0 : -1;
}

/* ---------- 读取 HTTP 头部 ---------- */
/* 返回状态码，out_headers 保存原始头部，body_buf/body_len 保存已读到的正文 */

static int read_http_headers(int sockfd, char *out_headers, size_t hdr_size,
                             char *body_buf, size_t body_size, size_t *body_len)
{
    char buf[BUF_SIZE];
    size_t total = 0;
    char *header_end = NULL;
    int status = -1;

    *body_len = 0;

    while (total < hdr_size - 1) {
        ssize_t r = recv(sockfd, buf, sizeof(buf), 0);
        if (r <= 0) break;
        if (total + r > hdr_size - 1) r = hdr_size - 1 - total;
        memcpy(out_headers + total, buf, r);
        total += r;
        out_headers[total] = '\0';

        header_end = strstr(out_headers, "\r\n\r\n");
        if (header_end) {
            size_t hdr_len = header_end - out_headers + 4;
            size_t extra   = total - hdr_len;
            if (extra > 0) {
                if (extra > body_size) extra = body_size;
                memcpy(body_buf, out_headers + hdr_len, extra);
                *body_len = extra;
            }
            break;
        }
    }

    if (header_end) {
        // 解析第一行 "HTTP/1.1 200 OK"
        char *sp = strchr(out_headers, ' ');
        if (sp) status = atoi(sp + 1);
    }
    return status;
}

/* ---------- 从响应头中提取某个字段 ---------- */

static long long get_header_ll(const char *headers, const char *key)
{
    const char *p = headers;
    size_t klen = strlen(key);
    while ((p = strcasestr(p, key)) != NULL) {
        // 确认是行首（或前面是 \n）
        if (p == headers || p[-1] == '\n') {
            const char *v = p + klen;
            while (*v == ' ' || *v == '\t') v++;
            return atoll(v);
        }
        p += klen;
    }
    return -1;
}

/* ---------- 处理重定向 ---------- */
/* 简化版：仅做一次性的 HTTP 重定向跟踪（在 HEAD 阶段） */

static int resolve_redirects(UrlInfo *u, long long *file_size, int *accept_ranges)
{
    int redirects = 0;
    while (redirects++ < MAX_REDIRECTS) {
        int sockfd = tcp_connect(u->host, u->port);
        if (sockfd < 0) return -1;

        if (send_request(sockfd, u, -1, -1, 1) < 0) {
            close(sockfd);
            return -1;
        }

        char headers[8192], body[1024];
        size_t blen;
        int status = read_http_headers(sockfd, headers, sizeof(headers),
                                       body, sizeof(body), &blen);
        close(sockfd);

        if (status == 200) {
            long long cl = get_header_ll(headers, "Content-Length:");
            *file_size = cl;
            *accept_ranges = (strcasestr(headers, "Accept-Ranges: bytes") != NULL);
            return 0;
        } else if (status == 301 || status == 302 || status == 303 ||
                   status == 307 || status == 308) {
            const char *loc = strcasestr(headers, "Location:");
            if (!loc) return -1;
            loc += strlen("Location:");
            while (*loc == ' ' || *loc == '\t') loc++;
            char newurl[2048];
            size_t i = 0;
            while (*loc && *loc != '\r' && *loc != '\n' && i < sizeof(newurl) - 1)
                newurl[i++] = *loc++;
            newurl[i] = '\0';
            printf("重定向 -> %s\n", newurl);
            if (parse_url(newurl, u) != 0) return -1;
        } else {
            fprintf(stderr, "HEAD 请求失败, HTTP %d\n", status);
            return -1;
        }
    }
    return -1;
}

/* ---------- 下载线程 ---------- */

static void *download_worker(void *arg)
{
    ThreadArg *t = (ThreadArg *)arg;

    int sockfd = tcp_connect(t->url.host, t->url.port);
    if (sockfd < 0) {
        fprintf(stderr, "[线程 %d] 连接失败\n", t->index);
        t->ok = 0;
        return NULL;
    }

    if (send_request(sockfd, &t->url, t->start, t->end, 0) < 0) {
        fprintf(stderr, "[线程 %d] 发送请求失败\n", t->index);
        close(sockfd);
        t->ok = 0;
        return NULL;
    }

    char headers[8192], body[BUF_SIZE];
    size_t blen = 0;
    int status = read_http_headers(sockfd, headers, sizeof(headers),
                                   body, sizeof(body), &blen);

    // 若服务器不支持 Range，会返回 200 而不是 206
    if (status != 206 && !(status == 200 && t->start == 0)) {
        fprintf(stderr, "[线程 %d] 服务器不支持 Range (HTTP %d)\n", t->index, status);
        close(sockfd);
        t->ok = 0;
        return NULL;
    }

    long long offset = t->start;

    // 处理 header 之后已经读到的部分 body
    if (blen > 0) {
        long long want = t->end - offset + 1;
        size_t write_len = blen > (size_t)want ? (size_t)want : blen;
        if (pwrite(t->fd, body, write_len, offset) != (ssize_t)write_len) {
            perror("pwrite");
            close(sockfd);
            t->ok = 0;
            return NULL;
        }
        offset += write_len;

        pthread_mutex_lock(t->lock);
        *t->total_done += write_len;
        pthread_mutex_unlock(t->lock);
    }

    char buf[BUF_SIZE];
    while (offset <= t->end) {
        long long remain = t->end - offset + 1;
        size_t to_read = remain < BUF_SIZE ? (size_t)remain : BUF_SIZE;

        ssize_t r = recv(sockfd, buf, to_read, 0);
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("recv");
            break;
        }
        if (r == 0) break; // 服务器关闭

        ssize_t written = 0;
        while (written < r) {
            ssize_t w = pwrite(t->fd, buf + written, r - written, offset + written);
            if (w < 0) {
                if (errno == EINTR) continue;
                perror("pwrite");
                close(sockfd);
                t->ok = 0;
                return NULL;
            }
            written += w;
        }
        offset += r;

        pthread_mutex_lock(t->lock);
        *t->total_done += r;
        pthread_mutex_unlock(t->lock);
    }

    close(sockfd);
    t->ok = (offset == t->end + 1);
    return NULL;
}

/* ---------- 进度显示线程 ---------- */

typedef struct {
    long long *total_done;
    long long file_size;
    int *stop;
    pthread_mutex_t *lock;
} ProgressArg;

static void *progress_worker(void *arg)
{
    ProgressArg *p = (ProgressArg *)arg;
    const char *spin = "|/-\\";
    int i = 0;
    time_t last = 0, start = time(NULL);
    long long last_done = 0;

    while (!*p->stop) {
        sleep(1);
        time_t now = time(NULL);
        pthread_mutex_lock(p->lock);
        long long done = *p->total_done;
        pthread_mutex_unlock(p->lock);

        double pct = p->file_size > 0
                   ? (double)done * 100.0 / (double)p->file_size : 0.0;
        double speed = (double)(done - last_done) / (double)(now - last + 1) / 1024.0;

        printf("\r%c  %.2f%%  %.2f / %.2f MB  %.1f KB/s   ",
               spin[i++ & 3],
               pct,
               (double)done / 1048576.0,
               (double)p->file_size / 1048576.0,
               speed);
        fflush(stdout);
        last = now;
        last_done = done;
    }

    (void)start;
    return NULL;
}

/* ---------- main ---------- */

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "用法: %s <URL> <线程数> [输出文件名]\n", argv[0]);
        return 1;
    }

    const char *url_str = argv[1];
    int nthreads = atoi(argv[2]);
    if (nthreads < 1) nthreads = 1;
    if (nthreads > MAX_THREADS) nthreads = MAX_THREADS;

    UrlInfo u;
    if (parse_url(url_str, &u) != 0) {
        fprintf(stderr, "URL 解析失败\n");
        return 1;
    }
    if (u.https) {
        fprintf(stderr, "本示例未实现 TLS，请使用 http:// URL\n");
        return 1;
    }

    printf("主机: %s:%d\n路径: %s\n", u.host, u.port, u.path);

    /* 1. HEAD 请求获取文件大小 */
    long long file_size = -1;
    int accept_ranges = 0;
    if (resolve_redirects(&u, &file_size, &accept_ranges) != 0) {
        fprintf(stderr, "无法获取文件信息\n");
        return 1;
    }
    printf("文件大小: %lld 字节\nAccept-Ranges: %s\n",
           file_size, accept_ranges ? "yes" : "no");

    if (file_size <= 0) {
        fprintf(stderr, "文件大小未知，无法多线程下载\n");
        return 1;
    }
    if (!accept_ranges) {
        printf("服务器不支持 Range，退化为单线程\n");
        nthreads = 1;
    }
    if ((long long)nthreads > file_size) nthreads = (int)file_size;

    /* 2. 打开/创建输出文件 */
    const char *outfile = argc >= 4 ? argv[3] : "download.out";
    int fd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        perror("open");
        return 1;
    }
    if (ftruncate(fd, file_size) != 0) {
        perror("ftruncate");
        close(fd);
        return 1;
    }

    /* 3. 切分区间 */
    long long chunk = file_size / nthreads;
    long long rem   = file_size % nthreads;

    ThreadArg *args = calloc(nthreads, sizeof(ThreadArg));
    pthread_t *tids = calloc(nthreads, sizeof(pthread_t));

    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    long long total_done = 0;

    long long pos = 0;
    for (int i = 0; i < nthreads; i++) {
        long long sz = chunk + (i < rem ? 1 : 0);
        args[i].url        = u;
        args[i].start      = pos;
        args[i].end        = pos + sz - 1;
        args[i].index      = i;
        args[i].fd         = fd;
        args[i].file_size  = file_size;
        args[i].lock       = &lock;
        args[i].total_done = &total_done;
        args[i].ok         = 0;
        pos += sz;
    }

    /* 4. 进度线程 */
    int stop = 0;
    ProgressArg parg = { &total_done, file_size, &stop, &lock };
    pthread_t ptid;
    pthread_create(&ptid, NULL, progress_worker, &parg);

    /* 5. 启动下载线程 */
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&tids[i], NULL, download_worker, &args[i]) != 0) {
            perror("pthread_create");
            args[i].ok = 0;
            tids[i] = 0;
        }
    }

    /* 6. 等待 */
    int all_ok = 1;
    for (int i = 0; i < nthreads; i++) {
        if (tids[i]) pthread_join(tids[i], NULL);
        if (!args[i].ok) all_ok = 0;
    }

    /* 7. 关闭进度线程 */
    stop = 1;
    pthread_join(ptid, NULL);
    printf("\n");

    close(fd);

    if (all_ok)
        printf("下载完成: %s (%lld 字节)\n", outfile, file_size);
    else
        printf("部分线程失败，请重试或检查网络\n");

    free(args);
    free(tids);
    return all_ok ? 0 : 2;
}