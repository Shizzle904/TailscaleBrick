#ifndef TS_CTRL_H
#define TS_CTRL_H

#include <stddef.h>
#include <stdint.h>

#define TS_MAX_PEERS 64

typedef struct {
    int  accept_routes;
    int  accept_dns;
    char hostname[64];
    char login_server[256];
    char auth_key[128];
    char tun_mode[24];
} TsConfig;

typedef struct {
    char     hostname[128];
    char     dns_name[160];
    char     ip[64];
    char     os[32];
    char     relay[32];
    char     cur_addr[96];
    int      online;
    int      active;
    int      exit_node;
    uint64_t rx_bytes;
    uint64_t tx_bytes;
    char     ping_result[96];
} TsPeer;

typedef struct {
    int      daemon_running;
    int      daemon_pid;
    uint64_t rss_kb;
    char     tun_mode[24];
    int      boot_autostart;
    char     backend_state[32];
    char     self_ip[64];
    char     self_host[128];
    char     self_dns[160];
    char     auth_url[512];
    char     magic_dns_suffix[128];
    char     health[256];
    char     version[64];
    TsPeer   peers[TS_MAX_PEERS];
    int      peer_count;
    int      online_peers;
    int      busy;
    char     busy_label[64];
    char     last_msg[160];
    uint32_t msg_seq;
} TsSnapshot;

int  ts_init(const char *config_path);
void ts_shutdown(void);

void ts_get_snapshot(TsSnapshot *out);
void ts_get_config(TsConfig *out);
int  ts_save_config(const TsConfig *cfg);

/* Async worker commands */
void ts_post_refresh(void);
void ts_post_start_only(void);
void ts_post_start_up(void);
void ts_post_down(void);
void ts_post_stop(void);
void ts_post_logout(void);
void ts_post_ping(const char *ip);

/* Boot autostart (/mnt/SDCARD/System/starts/tailscale_boot.sh) */
int  ts_get_boot_autostart(void);
int  ts_set_boot_autostart(int enable);

/* Log file helpers (/tmp/tailscaled.log) */
int  ts_read_log_lines(char lines[][160], int max_lines);
void ts_clear_log(void);

/* Pure parsing/config helpers (exposed for unit tests) */
int  ts_parse_status_json(const char *json_str, TsSnapshot *snap);
int  ts_config_load_file(const char *path, TsConfig *cfg);
int  ts_config_save_file(const char *path, const TsConfig *cfg);

#endif
