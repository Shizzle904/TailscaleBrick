#define _POSIX_C_SOURCE 200809L

#include "ts_ctrl.h"
#include "cJSON.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TS_PID_FILE   "/tmp/tailscaled.pid"
#define TS_MODE_FILE  "/tmp/tailscaled.mode"
#define TS_LOG_FILE   "/tmp/tailscaled.log"
#define TS_BOOT_FILE  "/mnt/SDCARD/System/starts/tailscale_boot.sh"
#define TS_BOOT_DIR   "/mnt/SDCARD/System/starts"

typedef enum {
    TS_CMD_NONE = 0,
    TS_CMD_REFRESH,
    TS_CMD_START_ONLY,
    TS_CMD_START_UP,
    TS_CMD_DOWN,
    TS_CMD_STOP,
    TS_CMD_LOGOUT,
    TS_CMD_PING
} TsCmdKind;

typedef struct {
    TsCmdKind kind;
    char      arg[64];
} TsCmd;

#define TS_CMD_CAP 16

static pthread_mutex_t g_ts_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_ts_cv = PTHREAD_COND_INITIALIZER;
static pthread_t       g_ts_thr;
static int             g_ts_started = 0;
static int             g_ts_quit = 0;

static char       g_cfg_path[256] = "config.ini";
static TsConfig   g_cfg;
static TsSnapshot g_snap;
static TsCmd      g_cmds[TS_CMD_CAP];
static int        g_cmd_count = 0;

static void safe_copy(char *dst, size_t dst_sz, const char *src)
{
    size_t len;
    if (!dst || dst_sz == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    len = strlen(src);
    if (len >= dst_sz) len = dst_sz - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static char *trim_inplace(char *s)
{
    size_t len;
    while (*s && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) s++;
    len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
    return s;
}

static uint64_t now_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static int is_valid_ip_str(const char *s)
{
    size_t len;
    if (!s || !s[0]) return 0;
    len = strlen(s);
    if (len >= 60) return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (!isxdigit(c) && c != '.' && c != ':') return 0;
    }
    return 1;
}

static void pick_best_ip(const cJSON *ips_arr, char *dst, size_t dst_sz)
{
    dst[0] = '\0';
    if (!cJSON_IsArray(ips_arr)) return;
    int n = cJSON_GetArraySize(ips_arr);
    for (int i = 0; i < n; i++) {
        cJSON *el = cJSON_GetArrayItem(ips_arr, i);
        if (cJSON_IsString(el) && el->valuestring && el->valuestring[0]) {
            if (dst[0] == '\0') safe_copy(dst, dst_sz, el->valuestring);
            if (strchr(el->valuestring, '.') != NULL) {
                safe_copy(dst, dst_sz, el->valuestring);
                return;
            }
        }
    }
}

static int cmp_peers(const void *a, const void *b)
{
    const TsPeer *pa = (const TsPeer *)a;
    const TsPeer *pb = (const TsPeer *)b;
    if (pb->online != pa->online) return pb->online - pa->online;
    return strcasecmp(pa->hostname, pb->hostname);
}

int ts_parse_status_json(const char *json_str, TsSnapshot *snap)
{
    cJSON *root;
    cJSON *f;
    cJSON *self_obj;
    cJSON *peers_obj;
    cJSON *health_arr;

    if (!json_str || !json_str[0] || !snap) return -1;

    snap->backend_state[0] = '\0';
    snap->self_ip[0] = '\0';
    snap->self_host[0] = '\0';
    snap->self_dns[0] = '\0';
    snap->auth_url[0] = '\0';
    snap->magic_dns_suffix[0] = '\0';
    snap->health[0] = '\0';
    snap->version[0] = '\0';
    memset(snap->peers, 0, sizeof(snap->peers));
    snap->peer_count = 0;
    snap->online_peers = 0;

    root = cJSON_Parse(json_str);
    if (!root) return -1;

    f = cJSON_GetObjectItemCaseSensitive(root, "BackendState");
    if (cJSON_IsString(f) && f->valuestring) safe_copy(snap->backend_state, sizeof(snap->backend_state), f->valuestring);

    f = cJSON_GetObjectItemCaseSensitive(root, "AuthURL");
    if (cJSON_IsString(f) && f->valuestring) safe_copy(snap->auth_url, sizeof(snap->auth_url), f->valuestring);

    f = cJSON_GetObjectItemCaseSensitive(root, "Version");
    if (cJSON_IsString(f) && f->valuestring) safe_copy(snap->version, sizeof(snap->version), f->valuestring);

    f = cJSON_GetObjectItemCaseSensitive(root, "MagicDNSSuffix");
    if (cJSON_IsString(f) && f->valuestring) safe_copy(snap->magic_dns_suffix, sizeof(snap->magic_dns_suffix), f->valuestring);

    health_arr = cJSON_GetObjectItemCaseSensitive(root, "Health");
    if (cJSON_IsArray(health_arr)) {
        int hn = cJSON_GetArraySize(health_arr);
        for (int i = 0; i < hn; i++) {
            cJSON *hi = cJSON_GetArrayItem(health_arr, i);
            if (cJSON_IsString(hi) && hi->valuestring && hi->valuestring[0]) {
                size_t cur = strlen(snap->health);
                if (cur > 0 && cur + 2 < sizeof(snap->health)) {
                    memcpy(snap->health + cur, "; ", 3);
                    cur += 2;
                }
                if (cur + 1 < sizeof(snap->health)) {
                    safe_copy(snap->health + cur, sizeof(snap->health) - cur, hi->valuestring);
                }
            }
        }
    }

    self_obj = cJSON_GetObjectItemCaseSensitive(root, "Self");
    if (cJSON_IsObject(self_obj)) {
        f = cJSON_GetObjectItemCaseSensitive(self_obj, "HostName");
        if (cJSON_IsString(f) && f->valuestring) safe_copy(snap->self_host, sizeof(snap->self_host), f->valuestring);
        f = cJSON_GetObjectItemCaseSensitive(self_obj, "DNSName");
        if (cJSON_IsString(f) && f->valuestring) safe_copy(snap->self_dns, sizeof(snap->self_dns), f->valuestring);
        pick_best_ip(cJSON_GetObjectItemCaseSensitive(self_obj, "TailscaleIPs"),
                     snap->self_ip, sizeof(snap->self_ip));
    }

    peers_obj = cJSON_GetObjectItemCaseSensitive(root, "Peer");
    if (cJSON_IsObject(peers_obj)) {
        for (cJSON *p = peers_obj->child; p != NULL && snap->peer_count < TS_MAX_PEERS; p = p->next) {
            if (!cJSON_IsObject(p)) continue;
            TsPeer *pr = &snap->peers[snap->peer_count];
            memset(pr, 0, sizeof(*pr));

            f = cJSON_GetObjectItemCaseSensitive(p, "HostName");
            if (cJSON_IsString(f) && f->valuestring) safe_copy(pr->hostname, sizeof(pr->hostname), f->valuestring);
            f = cJSON_GetObjectItemCaseSensitive(p, "DNSName");
            if (cJSON_IsString(f) && f->valuestring) safe_copy(pr->dns_name, sizeof(pr->dns_name), f->valuestring);
            f = cJSON_GetObjectItemCaseSensitive(p, "OS");
            if (cJSON_IsString(f) && f->valuestring) safe_copy(pr->os, sizeof(pr->os), f->valuestring);
            f = cJSON_GetObjectItemCaseSensitive(p, "Relay");
            if (cJSON_IsString(f) && f->valuestring) safe_copy(pr->relay, sizeof(pr->relay), f->valuestring);
            f = cJSON_GetObjectItemCaseSensitive(p, "CurAddr");
            if (cJSON_IsString(f) && f->valuestring) safe_copy(pr->cur_addr, sizeof(pr->cur_addr), f->valuestring);
            f = cJSON_GetObjectItemCaseSensitive(p, "Online");
            pr->online = cJSON_IsTrue(f) ? 1 : 0;
            f = cJSON_GetObjectItemCaseSensitive(p, "Active");
            pr->active = cJSON_IsTrue(f) ? 1 : 0;
            f = cJSON_GetObjectItemCaseSensitive(p, "ExitNode");
            pr->exit_node = cJSON_IsTrue(f) ? 1 : 0;
            f = cJSON_GetObjectItemCaseSensitive(p, "RxBytes");
            if (cJSON_IsNumber(f) && f->valuedouble > 0) pr->rx_bytes = (uint64_t)f->valuedouble;
            f = cJSON_GetObjectItemCaseSensitive(p, "TxBytes");
            if (cJSON_IsNumber(f) && f->valuedouble > 0) pr->tx_bytes = (uint64_t)f->valuedouble;

            pick_best_ip(cJSON_GetObjectItemCaseSensitive(p, "TailscaleIPs"), pr->ip, sizeof(pr->ip));
            if (pr->hostname[0] == '\0' && pr->dns_name[0] != '\0') {
                safe_copy(pr->hostname, sizeof(pr->hostname), pr->dns_name);
            }
            if (pr->online) snap->online_peers++;
            snap->peer_count++;
        }
    }

    if (snap->peer_count > 1) {
        qsort(snap->peers, (size_t)snap->peer_count, sizeof(TsPeer), cmp_peers);
    }

    cJSON_Delete(root);
    return 0;
}

int ts_config_load_file(const char *path, TsConfig *cfg)
{
    FILE *fp;
    char line[512];

    if (!cfg) return -1;
    memset(cfg, 0, sizeof(*cfg));
    cfg->accept_routes = 1;
    cfg->accept_dns = 0;
    safe_copy(cfg->hostname, sizeof(cfg->hostname), "trimui-brick");
    safe_copy(cfg->tun_mode, sizeof(cfg->tun_mode), "auto");

    if (!path || !path[0]) return -1;
    fp = fopen(path, "r");
    if (!fp) return -1;

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *t = trim_inplace(line);
        char *eq;
        char *key;
        char *val;
        if (*t == '\0' || *t == '#' || *t == ';') continue;
        eq = strchr(t, '=');
        if (!eq) continue;
        *eq = '\0';
        key = trim_inplace(t);
        val = trim_inplace(eq + 1);
        if (strcmp(key, "accept_routes") == 0) {
            cfg->accept_routes = (atoi(val) != 0);
        } else if (strcmp(key, "accept_dns") == 0) {
            cfg->accept_dns = (atoi(val) != 0);
        } else if (strcmp(key, "hostname") == 0 && val[0]) {
            safe_copy(cfg->hostname, sizeof(cfg->hostname), val);
        } else if (strcmp(key, "login_server") == 0) {
            safe_copy(cfg->login_server, sizeof(cfg->login_server), val);
        } else if (strcmp(key, "auth_key") == 0) {
            safe_copy(cfg->auth_key, sizeof(cfg->auth_key), val);
        } else if (strcmp(key, "tun_mode") == 0 && val[0]) {
            safe_copy(cfg->tun_mode, sizeof(cfg->tun_mode), val);
        }
    }
    fclose(fp);
    return 0;
}

int ts_config_save_file(const char *path, const TsConfig *cfg)
{
    char tmp[300];
    FILE *fp;

    if (!path || !path[0] || !cfg) return -1;
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) return -1;
    fp = fopen(tmp, "wb");
    if (!fp) return -1;

    fprintf(fp, "accept_routes=%d\n", cfg->accept_routes ? 1 : 0);
    fprintf(fp, "accept_dns=%d\n", cfg->accept_dns ? 1 : 0);
    fprintf(fp, "hostname=%s\n", cfg->hostname[0] ? cfg->hostname : "trimui-brick");
    fprintf(fp, "tun_mode=%s\n", cfg->tun_mode[0] ? cfg->tun_mode : "auto");
    if (cfg->login_server[0]) {
        fprintf(fp, "login_server=%s\n", cfg->login_server);
    } else {
        fprintf(fp, "# login_server=https://headscale.example.com\n");
    }
    if (cfg->auth_key[0]) {
        fprintf(fp, "auth_key=%s\n", cfg->auth_key);
    } else {
        fprintf(fp, "# auth_key=tskey-auth-...\n");
    }

    if (fflush(fp) != 0 || fclose(fp) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* ---------------- Subprocess Runner ---------------- */

static int run_daemon_cmd(const char *arg1, const char *arg2,
                          char *out_buf, size_t out_cap, int timeout_ms)
{
    int pfd[2];
    pid_t pid;
    size_t len = 0;
    uint64_t deadline = now_mono_ms() + (uint64_t)timeout_ms;

    if (out_buf && out_cap > 0) out_buf[0] = '\0';
    if (pipe(pfd) != 0) return -1;

    pid = fork();
    if (pid < 0) {
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }

    if (pid == 0) {
        char *argv[6];
        int argc = 0;
        close(pfd[0]);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        if (pfd[1] > STDERR_FILENO) close(pfd[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            if (devnull > STDERR_FILENO) close(devnull);
        }
        argv[argc++] = "/bin/sh";
        argv[argc++] = "./scripts/daemon.sh";
        argv[argc++] = (char *)arg1;
        if (arg2 && arg2[0]) argv[argc++] = (char *)arg2;
        argv[argc] = NULL;
        execvp("/bin/sh", argv);
        _exit(127);
    }

    close(pfd[1]);
    int flags = fcntl(pfd[0], F_GETFL, 0);
    if (flags >= 0) fcntl(pfd[0], F_SETFL, flags | O_NONBLOCK);

    int timed_out = 0;
    for (;;) {
        uint64_t now = now_mono_ms();
        if (now >= deadline) {
            timed_out = 1;
            break;
        }
        pthread_mutex_lock(&g_ts_mu);
        int q = g_ts_quit;
        pthread_mutex_unlock(&g_ts_mu);
        if (q) {
            timed_out = 1;
            break;
        }

        struct pollfd p;
        p.fd = pfd[0];
        p.events = POLLIN;
        p.revents = 0;
        int wait_ms = (int)(deadline - now);
        if (wait_ms > 250) wait_ms = 250;
        int prc = poll(&p, 1, wait_ms);
        if (prc < 0 && errno == EINTR) continue;

        if (prc > 0 && (p.revents & (POLLIN | POLLHUP | POLLERR))) {
            char tmp[2048];
            ssize_t r = read(pfd[0], tmp, sizeof(tmp));
            if (r > 0) {
                if (out_buf && out_cap > 1 && len + 1 < out_cap) {
                    size_t take = (size_t)r;
                    if (take > (out_cap - 1) - len) take = (out_cap - 1) - len;
                    memcpy(out_buf + len, tmp, take);
                    len += take;
                    out_buf[len] = '\0';
                }
            } else if (r == 0) {
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                break;
            }
        }
    }

    if (timed_out) {
        kill(pid, SIGKILL);
    }
    close(pfd[0]);

    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (timed_out) return -2;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static int read_daemon_proc_info(int *out_pid, uint64_t *out_rss_kb, char *out_mode, size_t mode_sz)
{
    FILE *fp;
    int pid = 0;
    *out_pid = 0;
    *out_rss_kb = 0;
    if (out_mode && mode_sz > 0) out_mode[0] = '\0';

    fp = fopen(TS_PID_FILE, "r");
    if (!fp) return 0;
    if (fscanf(fp, "%d", &pid) != 1 || pid <= 0) {
        fclose(fp);
        return 0;
    }
    fclose(fp);

    if (kill((pid_t)pid, 0) != 0) return 0;

    char pbuf[64];
    snprintf(pbuf, sizeof(pbuf), "/proc/%d/cmdline", pid);
    fp = fopen(pbuf, "rb");
    if (!fp) return 0;
    char cmd[256];
    size_t n = fread(cmd, 1, sizeof(cmd) - 1, fp);
    fclose(fp);
    for (size_t i = 0; i < n; i++) {
        if (cmd[i] == '\0') cmd[i] = ' ';
    }
    cmd[n] = '\0';
    if (strstr(cmd, "tailscaled") == NULL) return 0;

    *out_pid = pid;

    snprintf(pbuf, sizeof(pbuf), "/proc/%d/status", pid);
    fp = fopen(pbuf, "r");
    if (fp) {
        char line[128];
        while (fgets(line, sizeof(line), fp)) {
            unsigned long long kb = 0;
            if (sscanf(line, "VmRSS: %llu kB", &kb) == 1) {
                *out_rss_kb = (uint64_t)kb;
                break;
            }
        }
        fclose(fp);
    }

    fp = fopen(TS_MODE_FILE, "r");
    if (fp) {
        char mbuf[32] = "";
        if (fgets(mbuf, sizeof(mbuf), fp)) {
            safe_copy(out_mode, mode_sz, trim_inplace(mbuf));
        }
        fclose(fp);
    }

    return 1;
}

static void refresh_snapshot_blocking(void)
{
    int pid = 0;
    uint64_t rss_kb = 0;
    char mode[24] = "";
    int running = read_daemon_proc_info(&pid, &rss_kb, mode, sizeof(mode));
    int boot = ts_get_boot_autostart();

    TsSnapshot next;
    memset(&next, 0, sizeof(next));
    next.daemon_running = running;
    next.daemon_pid = pid;
    next.rss_kb = rss_kb;
    safe_copy(next.tun_mode, sizeof(next.tun_mode), mode);
    next.boot_autostart = boot;

    if (running) {
        char *jbuf = (char *)malloc(262144);
        if (jbuf) {
            int rc = run_daemon_cmd("status", NULL, jbuf, 262144, 4000);
            if (rc == 0 && jbuf[0] != '\0') {
                ts_parse_status_json(jbuf, &next);
            } else {
                safe_copy(next.backend_state, sizeof(next.backend_state), "Starting");
            }
            free(jbuf);
        }
    } else {
        safe_copy(next.backend_state, sizeof(next.backend_state), "Stopped");
    }

    pthread_mutex_lock(&g_ts_mu);
    /* Preserve per-peer ping results & toast message */
    for (int i = 0; i < next.peer_count; i++) {
        if (next.peers[i].ip[0] == '\0') continue;
        for (int j = 0; j < g_snap.peer_count; j++) {
            if (strcmp(next.peers[i].ip, g_snap.peers[j].ip) == 0 &&
                g_snap.peers[j].ping_result[0] != '\0') {
                safe_copy(next.peers[i].ping_result, sizeof(next.peers[i].ping_result),
                          g_snap.peers[j].ping_result);
                break;
            }
        }
    }
    next.busy = g_snap.busy;
    safe_copy(next.busy_label, sizeof(next.busy_label), g_snap.busy_label);
    safe_copy(next.last_msg, sizeof(next.last_msg), g_snap.last_msg);
    next.msg_seq = g_snap.msg_seq;
    g_snap = next;
    pthread_mutex_unlock(&g_ts_mu);
}

static void set_toast_msg(const char *msg)
{
    pthread_mutex_lock(&g_ts_mu);
    safe_copy(g_snap.last_msg, sizeof(g_snap.last_msg), msg);
    g_snap.msg_seq++;
    pthread_mutex_unlock(&g_ts_mu);
}

static void set_peer_ping_result(const char *ip, const char *res)
{
    pthread_mutex_lock(&g_ts_mu);
    for (int i = 0; i < g_snap.peer_count; i++) {
        if (strcmp(g_snap.peers[i].ip, ip) == 0) {
            safe_copy(g_snap.peers[i].ping_result, sizeof(g_snap.peers[i].ping_result), res);
            break;
        }
    }
    pthread_mutex_unlock(&g_ts_mu);
}

static void *ts_worker_fn(void *arg)
{
    (void)arg;
    refresh_snapshot_blocking();

    for (;;) {
        TsCmd cmd;
        memset(&cmd, 0, sizeof(cmd));

        pthread_mutex_lock(&g_ts_mu);
        if (g_cmd_count == 0 && !g_ts_quit) {
            int interval_ms = (strcmp(g_snap.backend_state, "NeedsLogin") == 0) ? 1500 : 2500;
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += interval_ms / 1000;
            ts.tv_nsec += (long)(interval_ms % 1000) * 1000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&g_ts_cv, &g_ts_mu, &ts);
        }
        if (g_ts_quit) {
            pthread_mutex_unlock(&g_ts_mu);
            break;
        }
        if (g_cmd_count > 0) {
            cmd = g_cmds[0];
            memmove(&g_cmds[0], &g_cmds[1], (size_t)(g_cmd_count - 1) * sizeof(TsCmd));
            g_cmd_count--;
            g_snap.busy = 1;
            switch (cmd.kind) {
            case TS_CMD_START_ONLY: safe_copy(g_snap.busy_label, sizeof(g_snap.busy_label), "正在启动守护进程..."); break;
            case TS_CMD_START_UP:   safe_copy(g_snap.busy_label, sizeof(g_snap.busy_label), "正在连接 Tailscale..."); break;
            case TS_CMD_DOWN:       safe_copy(g_snap.busy_label, sizeof(g_snap.busy_label), "正在断开连接..."); break;
            case TS_CMD_STOP:       safe_copy(g_snap.busy_label, sizeof(g_snap.busy_label), "正在停止后台服务..."); break;
            case TS_CMD_LOGOUT:     safe_copy(g_snap.busy_label, sizeof(g_snap.busy_label), "正在注销账号..."); break;
            case TS_CMD_PING:       safe_copy(g_snap.busy_label, sizeof(g_snap.busy_label), "正在测试连通性..."); break;
            default:                safe_copy(g_snap.busy_label, sizeof(g_snap.busy_label), "正在刷新..."); break;
            }
        }
        pthread_mutex_unlock(&g_ts_mu);

        if (cmd.kind == TS_CMD_START_ONLY) {
            char out[512];
            int rc = run_daemon_cmd("start", NULL, out, sizeof(out), 15000);
            set_toast_msg(rc == 0 ? "后台守护已启动" : "守护启动失败，请查看日志");
        } else if (cmd.kind == TS_CMD_START_UP) {
            char out[512];
            int rc = run_daemon_cmd("up", NULL, out, sizeof(out), 25000);
            if (rc != 0) {
                set_toast_msg("连接失败，请查看日志");
            } else {
                set_toast_msg("已发起连接");
            }
        } else if (cmd.kind == TS_CMD_DOWN) {
            char out[512];
            int rc = run_daemon_cmd("down", NULL, out, sizeof(out), 8000);
            set_toast_msg(rc == 0 ? "已断开组网 (后台守护保留)" : "断开失败");
        } else if (cmd.kind == TS_CMD_STOP) {
            char out[512];
            run_daemon_cmd("stop", NULL, out, sizeof(out), 10000);
            set_toast_msg("后台服务已停止");
        } else if (cmd.kind == TS_CMD_LOGOUT) {
            char out[512];
            int rc = run_daemon_cmd("logout", NULL, out, sizeof(out), 10000);
            set_toast_msg(rc == 0 ? "已注销当前账号" : "注销失败");
        } else if (cmd.kind == TS_CMD_PING) {
            char out[512];
            if (!is_valid_ip_str(cmd.arg)) {
                set_toast_msg("无效的节点 IP");
            } else {
                int rc = run_daemon_cmd("ping", cmd.arg, out, sizeof(out), 8000);
                char *in_p = strstr(out, "in ");
                char summary[96];
                if (rc == 0 && in_p) {
                    char *nl = strpbrk(in_p, "\r\n");
                    if (nl) *nl = '\0';
                    snprintf(summary, sizeof(summary), "OK (%s)", in_p + 3);
                } else if (rc == 0) {
                    safe_copy(summary, sizeof(summary), "OK");
                } else {
                    safe_copy(summary, sizeof(summary), "超时/不可达");
                }
                set_peer_ping_result(cmd.arg, summary);
                char toast[144];
                snprintf(toast, sizeof(toast), "Ping %.48s: %.80s", cmd.arg, summary);
                set_toast_msg(toast);
            }
        }

        refresh_snapshot_blocking();

        if (cmd.kind != TS_CMD_NONE) {
            pthread_mutex_lock(&g_ts_mu);
            g_snap.busy = 0;
            g_snap.busy_label[0] = '\0';
            pthread_mutex_unlock(&g_ts_mu);
        }
    }
    return NULL;
}

static void enqueue_cmd(TsCmdKind kind, const char *arg)
{
    pthread_mutex_lock(&g_ts_mu);
    if (g_cmd_count < TS_CMD_CAP) {
        g_cmds[g_cmd_count].kind = kind;
        safe_copy(g_cmds[g_cmd_count].arg, sizeof(g_cmds[g_cmd_count].arg), arg ? arg : "");
        g_cmd_count++;
        pthread_cond_signal(&g_ts_cv);
    }
    pthread_mutex_unlock(&g_ts_mu);
}

int ts_init(const char *config_path)
{
    pthread_mutex_lock(&g_ts_mu);
    safe_copy(g_cfg_path, sizeof(g_cfg_path),
              (config_path && config_path[0]) ? config_path : "config.ini");
    ts_config_load_file(g_cfg_path, &g_cfg);
    memset(&g_snap, 0, sizeof(g_snap));
    safe_copy(g_snap.backend_state, sizeof(g_snap.backend_state), "Checking");
    g_ts_quit = 0;
    g_cmd_count = 0;
    pthread_mutex_unlock(&g_ts_mu);

    if (pthread_create(&g_ts_thr, NULL, ts_worker_fn, NULL) != 0) {
        return -1;
    }
    g_ts_started = 1;
    return 0;
}

void ts_shutdown(void)
{
    pthread_mutex_lock(&g_ts_mu);
    g_ts_quit = 1;
    pthread_cond_broadcast(&g_ts_cv);
    pthread_mutex_unlock(&g_ts_mu);

    if (g_ts_started) {
        pthread_join(g_ts_thr, NULL);
        g_ts_started = 0;
    }
}

void ts_get_snapshot(TsSnapshot *out)
{
    if (!out) return;
    pthread_mutex_lock(&g_ts_mu);
    *out = g_snap;
    pthread_mutex_unlock(&g_ts_mu);
}

void ts_get_config(TsConfig *out)
{
    if (!out) return;
    pthread_mutex_lock(&g_ts_mu);
    *out = g_cfg;
    pthread_mutex_unlock(&g_ts_mu);
}

int ts_save_config(const TsConfig *cfg)
{
    int rc;
    if (!cfg) return -1;
    pthread_mutex_lock(&g_ts_mu);
    g_cfg = *cfg;
    rc = ts_config_save_file(g_cfg_path, &g_cfg);
    pthread_mutex_unlock(&g_ts_mu);
    return rc;
}

void ts_post_refresh(void)    { enqueue_cmd(TS_CMD_REFRESH, NULL); }
void ts_post_start_only(void) { enqueue_cmd(TS_CMD_START_ONLY, NULL); }
void ts_post_start_up(void)   { enqueue_cmd(TS_CMD_START_UP, NULL); }
void ts_post_down(void)       { enqueue_cmd(TS_CMD_DOWN, NULL); }
void ts_post_stop(void)       { enqueue_cmd(TS_CMD_STOP, NULL); }
void ts_post_logout(void)     { enqueue_cmd(TS_CMD_LOGOUT, NULL); }
void ts_post_ping(const char *ip) { enqueue_cmd(TS_CMD_PING, ip); }

int ts_get_boot_autostart(void)
{
    return access(TS_BOOT_FILE, F_OK) == 0 ? 1 : 0;
}

int ts_set_boot_autostart(int enable)
{
    if (!enable) {
        if (unlink(TS_BOOT_FILE) != 0 && errno != ENOENT) return -1;
        pthread_mutex_lock(&g_ts_mu);
        g_snap.boot_autostart = 0;
        pthread_mutex_unlock(&g_ts_mu);
        return 0;
    }

    mkdir(TS_BOOT_DIR, 0755);
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s.tmp", TS_BOOT_FILE);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return -1;
    fprintf(fp, "#!/bin/sh\n/mnt/SDCARD/Apps/Tailscale/scripts/daemon.sh boot >/tmp/tailscale_boot.log 2>&1 &\n");
    if (fflush(fp) != 0 || fclose(fp) != 0) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, TS_BOOT_FILE) != 0) {
        unlink(tmp);
        return -1;
    }
    chmod(TS_BOOT_FILE, 0755);

    pthread_mutex_lock(&g_ts_mu);
    g_snap.boot_autostart = 1;
    pthread_mutex_unlock(&g_ts_mu);
    return 0;
}

int ts_read_log_lines(char lines[][160], int max_lines)
{
    FILE *fp;
    char buf[512];
    int total = 0;
    int cap = max_lines > 0 ? max_lines : 1;
    int write_idx = 0;

    if (!lines || max_lines <= 0) return 0;
    fp = fopen(TS_LOG_FILE, "r");
    if (!fp) return 0;

    while (fgets(buf, sizeof(buf), fp) != NULL) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            buf[--len] = '\0';
        }
        safe_copy(lines[write_idx], 160, buf);
        write_idx = (write_idx + 1) % cap;
        total++;
    }
    fclose(fp);

    if (total <= cap) {
        return total;
    }

    /* Rotate ring buffer in-place so lines[0] is oldest of the last `cap` lines */
    char (*ordered)[160] = calloc((size_t)cap, 160);
    if (!ordered) return cap;
    for (int i = 0; i < cap; i++) {
        int src = (write_idx + i) % cap;
        memcpy(ordered[i], lines[src], 160);
    }
    memcpy(lines, ordered, (size_t)cap * 160);
    free(ordered);
    return cap;
}

void ts_clear_log(void)
{
    FILE *fp = fopen(TS_LOG_FILE, "w");
    if (fp) fclose(fp);
}
