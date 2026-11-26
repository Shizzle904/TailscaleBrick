#include "ts_ctrl.h"
#include "qrcode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int check_qr_url(const char *url)
{
    QrCode qr;
    if (qr_encode_text(url, &qr) != 0) {
        fprintf(stderr, "[FAIL] qr_encode_text failed for len=%zu\n", strlen(url));
        return 1;
    }
    if (qr.size < 21 || qr.size > QR_MAX_SIZE || (qr.size - 21) % 4 != 0) {
        fprintf(stderr, "[FAIL] invalid QR size=%d\n", qr.size);
        return 1;
    }
    /* Verify top-left 7x7 finder pattern corners & center */
    if (!qr_get_module(&qr, 0, 0) || !qr_get_module(&qr, 6, 0) ||
        !qr_get_module(&qr, 0, 6) || !qr_get_module(&qr, 6, 6) ||
        !qr_get_module(&qr, 3, 3) || qr_get_module(&qr, 1, 1) ||
        qr_get_module(&qr, 7, 0) || qr_get_module(&qr, 0, 7)) {
        fprintf(stderr, "[FAIL] top-left finder pattern invalid (size=%d)\n", qr.size);
        return 1;
    }
    /* Verify dark module at (8, size - 8) */
    if (!qr_get_module(&qr, 8, qr.size - 8)) {
        fprintf(stderr, "[FAIL] dark module missing (size=%d)\n", qr.size);
        return 1;
    }
    printf("[PASS] QR(%zu B) -> size=%dx%d\n", strlen(url), qr.size, qr.size);
    return 0;
}

int main(void)
{
    int failures = 0;

    printf("=== Test 1: ts_parse_status_json (NeedsLogin) ===\n");
    {
        const char *sample =
            "{"
            "\"Version\":\"1.104.7\","
            "\"BackendState\":\"NeedsLogin\","
            "\"AuthURL\":\"https://login.tailscale.com/a/1234567890abcdef\","
            "\"Self\":{\"HostName\":\"trimui-brick\",\"DNSName\":\"trimui-brick.tailnet.ts.net.\",\"TailscaleIPs\":[\"100.64.0.10\"]}"
            "}";
        TsSnapshot snap;
        memset(&snap, 0, sizeof(snap));
        if (ts_parse_status_json(sample, &snap) != 0 ||
            strcmp(snap.backend_state, "NeedsLogin") != 0 ||
            strcmp(snap.auth_url, "https://login.tailscale.com/a/1234567890abcdef") != 0 ||
            strcmp(snap.self_ip, "100.64.0.10") != 0) {
            fprintf(stderr, "[FAIL] NeedsLogin parse failed\n");
            failures++;
        } else {
            printf("[PASS] NeedsLogin parsed: host=%s ip=%s url=%s\n",
                   snap.self_host, snap.self_ip, snap.auth_url);
        }
    }

    printf("\n=== Test 2: ts_parse_status_json (Running + Peers) ===\n");
    {
        const char *sample =
            "{"
            "\"Version\":\"1.104.7\","
            "\"BackendState\":\"Running\","
            "\"MagicDNSSuffix\":\"tail1234.ts.net\","
            "\"Self\":{\"HostName\":\"trimui-brick\",\"DNSName\":\"trimui-brick.tail1234.ts.net.\",\"TailscaleIPs\":[\"100.80.1.5\",\"fd7a::1\"]},"
            "\"Peer\":{"
            "  \"node1\":{\"HostName\":\"phone\",\"OS\":\"android\",\"Online\":false,\"TailscaleIPs\":[\"100.80.1.2\"]},"
            "  \"node2\":{\"HostName\":\"nas-server\",\"OS\":\"linux\",\"Online\":true,\"Active\":true,\"CurAddr\":\"192.0.2.1:41641\",\"RxBytes\":1024,\"TxBytes\":2048,\"TailscaleIPs\":[\"100.80.1.1\"]}"
            "}"
            "}";
        TsSnapshot snap;
        memset(&snap, 0, sizeof(snap));
        if (ts_parse_status_json(sample, &snap) != 0 ||
            strcmp(snap.backend_state, "Running") != 0 ||
            snap.peer_count != 2 || snap.online_peers != 1 ||
            strcmp(snap.peers[0].hostname, "nas-server") != 0 ||
            strcmp(snap.peers[0].ip, "100.80.1.1") != 0 ||
            snap.peers[0].tx_bytes != 2048) {
            fprintf(stderr, "[FAIL] Running parse failed\n");
            failures++;
        } else {
            printf("[PASS] Running parsed: self=%s peers=%d online=%d first=%s\n",
                   snap.self_ip, snap.peer_count, snap.online_peers, snap.peers[0].hostname);
        }
    }

    printf("\n=== Test 3: ts_config_load/save roundtrip ===\n");
    {
        const char *tmp_cfg = "/tmp/test_ts_cfg.ini";
        TsConfig a, b;
        memset(&a, 0, sizeof(a));
        a.accept_routes = 1;
        a.accept_dns = 1;
        snprintf(a.hostname, sizeof(a.hostname), "brick-test");
        snprintf(a.tun_mode, sizeof(a.tun_mode), "kernel");
        snprintf(a.login_server, sizeof(a.login_server), "https://hs.example.org");
        if (ts_config_save_file(tmp_cfg, &a) != 0 || ts_config_load_file(tmp_cfg, &b) != 0 ||
            b.accept_routes != 1 || b.accept_dns != 1 ||
            strcmp(b.hostname, "brick-test") != 0 ||
            strcmp(b.tun_mode, "kernel") != 0 ||
            strcmp(b.login_server, "https://hs.example.org") != 0) {
            fprintf(stderr, "[FAIL] config roundtrip failed\n");
            failures++;
        } else {
            printf("[PASS] Config roundtrip passed\n");
        }
        unlink(tmp_cfg);
    }

    printf("\n=== Test 4: QR Code Encoder ===\n");
    failures += check_qr_url("https://login.tailscale.com/a/1234567890abcdef");
    failures += check_qr_url("https://headscale.example.org/register/mkey:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");

    printf("\n=== Summary: %s (failures=%d) ===\n",
           failures == 0 ? "ALL PASSED" : "FAILED", failures);
    return failures == 0 ? 0 : 1;
}
