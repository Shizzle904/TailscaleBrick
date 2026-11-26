#!/bin/sh
# Tailscale daemon controller for Trimui Brick (TG3040 / Stock OS)

APPDIR="$(cd "$(dirname "$0")/.." && pwd)"
SOCK="/tmp/tailscaled.sock"
PIDF="/tmp/tailscaled.pid"
LOGF="/tmp/tailscaled.log"
MODEF="/tmp/tailscaled.mode"
STATEDIR="/mnt/UDISK/tailscale"
STATEFILE="$STATEDIR/tailscaled.state"

export SSL_CERT_FILE="$APPDIR/certs/cacert.pem"
export SSL_CERT_DIR="$APPDIR/certs"
export GOMEMLIMIT="48MiB"
export GOGC="25"
export HOME="$STATEDIR"
export PATH="$APPDIR/bin:$PATH"

ACCEPT_ROUTES="1"
ACCEPT_DNS="0"
HOSTNAME_CFG="trimui-brick"
LOGIN_SERVER=""
AUTH_KEY=""
TUN_MODE="auto"

trim_val() {
    printf '%s' "$1" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//'
}

load_cfg() {
    if [ -r "$APPDIR/config.ini" ]; then
        while IFS= read -r line || [ -n "$line" ]; do
            line=$(printf '%s' "$line" | tr -d '\r')
            line=$(trim_val "$line")
            case "$line" in
                ""|"#"*|";"*) continue ;;
            esac
            key=$(trim_val "${line%%=*}")
            val=$(trim_val "${line#*=}")
            case "$key" in
                accept_routes) ACCEPT_ROUTES="$val" ;;
                accept_dns) ACCEPT_DNS="$val" ;;
                hostname) [ -n "$val" ] && HOSTNAME_CFG="$val" ;;
                login_server) LOGIN_SERVER="$val" ;;
                auth_key) AUTH_KEY="$val" ;;
                tun_mode) [ -n "$val" ] && TUN_MODE="$val" ;;
            esac
        done < "$APPDIR/config.ini"
    fi

    if [ -z "$AUTH_KEY" ] && [ -r "$APPDIR/authkey.txt" ]; then
        AUTH_KEY=$(head -n 1 "$APPDIR/authkey.txt" | tr -d '\r\n' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    fi
    if [ -z "$LOGIN_SERVER" ] && [ -r "$APPDIR/loginserver.txt" ]; then
        LOGIN_SERVER=$(head -n 1 "$APPDIR/loginserver.txt" | tr -d '\r\n' | sed 's/^[[:space:]]*//;s/[[:space:]]*$//')
    fi
}

find_daemon_pid() {
    if [ -r "$PIDF" ]; then
        p=$(cat "$PIDF" 2>/dev/null)
        if [ -n "$p" ] && kill -0 "$p" 2>/dev/null && [ -r "/proc/$p/cmdline" ]; then
            cmd=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null)
            case "$cmd" in
                *tailscaled*) printf '%s' "$p"; return 0 ;;
            esac
        fi
    fi
    for p in /proc/[0-9]*; do
        pid="${p#/proc/}"
        [ -r "$p/cmdline" ] || continue
        cmd=$(tr '\0' ' ' < "$p/cmdline" 2>/dev/null)
        case "$cmd" in
            *tailscaled*--socket=/tmp/tailscaled.sock*)
                echo "$pid" > "$PIDF"
                printf '%s' "$pid"
                return 0
                ;;
        esac
    done
    return 1
}

rotate_log() {
    if [ -f "$LOGF" ]; then
        sz=$(wc -c < "$LOGF" 2>/dev/null || echo 0)
        if [ "$sz" -gt 262144 ] 2>/dev/null; then
            tail -c 65536 "$LOGF" > "${LOGF}.tmp" 2>/dev/null && mv "${LOGF}.tmp" "$LOGF"
        fi
    fi
}

spawn_tailscaled() {
    mode="$1"
    mkdir -p "$STATEDIR" || return 1
    rm -f "$SOCK" "$PIDF"

    if [ "$mode" = "kernel" ]; then
        mkdir -p /dev/net
        [ -c /dev/net/tun ] || mknod /dev/net/tun c 10 200 2>/dev/null || true
        [ -c /dev/net/tun ] || return 1
        echo "kernel" > "$MODEF"
        ( trap '' HUP; "$APPDIR/bin/tailscaled" \
            --state="$STATEFILE" \
            --statedir="$STATEDIR" \
            --socket="$SOCK" \
            --tun=tailscale0 \
            >> "$LOGF" 2>&1 < /dev/null & echo $! > "$PIDF" )
    else
        echo "userspace" > "$MODEF"
        ( trap '' HUP; "$APPDIR/bin/tailscaled" \
            --state="$STATEFILE" \
            --statedir="$STATEDIR" \
            --socket="$SOCK" \
            --tun=userspace-networking \
            --socks5-server=127.0.0.1:1055 \
            --outbound-http-proxy-listen=127.0.0.1:1056 \
            >> "$LOGF" 2>&1 < /dev/null & echo $! > "$PIDF" )
    fi

    i=0
    while [ "$i" -lt 40 ]; do
        if [ -S "$SOCK" ]; then
            p=$(find_daemon_pid)
            [ -n "$p" ] && return 0
        fi
        p=$(cat "$PIDF" 2>/dev/null)
        if [ -n "$p" ] && ! kill -0 "$p" 2>/dev/null; then
            break
        fi
        i=$((i + 1))
        sleep 0.2 2>/dev/null || sleep 1
    done

    return 1
}

do_start() {
    load_cfg
    p=$(find_daemon_pid)
    if [ -n "$p" ] && [ -S "$SOCK" ]; then
        return 0
    fi
    if [ -n "$p" ]; then
        do_stop
    fi
    rotate_log
    echo "=== start $(date) mode=$TUN_MODE ===" >> "$LOGF"

    if [ "$TUN_MODE" = "userspace" ]; then
        spawn_tailscaled "userspace"
        return $?
    fi

    if spawn_tailscaled "kernel"; then
        return 0
    fi

    if [ "$TUN_MODE" = "auto" ]; then
        echo "=== kernel tun failed, falling back to userspace ===" >> "$LOGF"
        do_stop
        spawn_tailscaled "userspace"
        return $?
    fi
    return 1
}

do_up() {
    do_start || return 1
    load_cfg

    set -- --socket="$SOCK" up --netfilter-mode=off
    if [ "$ACCEPT_ROUTES" = "1" ]; then
        set -- "$@" --accept-routes=true
    else
        set -- "$@" --accept-routes=false
    fi
    if [ "$ACCEPT_DNS" = "1" ]; then
        set -- "$@" --accept-dns=true
    else
        set -- "$@" --accept-dns=false
    fi
    [ -n "$HOSTNAME_CFG" ] && set -- "$@" --hostname="$HOSTNAME_CFG"
    [ -n "$LOGIN_SERVER" ] && set -- "$@" --login-server="$LOGIN_SERVER"
    [ -n "$AUTH_KEY" ] && set -- "$@" --auth-key="$AUTH_KEY"

    if [ -n "$AUTH_KEY" ]; then
        "$APPDIR/bin/tailscale" "$@" --timeout=20s >> "$LOGF" 2>&1
        return $?
    fi

    ( trap '' HUP; "$APPDIR/bin/tailscale" "$@" >> "$LOGF" 2>&1 < /dev/null & )
    i=0
    while [ "$i" -lt 25 ]; do
        st=$("$APPDIR/bin/tailscale" --socket="$SOCK" status --json 2>/dev/null)
        case "$st" in
            *'"BackendState":"Running"'*|*'"AuthURL":"http'*)
                return 0
                ;;
        esac
        i=$((i + 1))
        sleep 0.2 2>/dev/null || sleep 1
    done
    return 0
}

do_down() {
    [ -S "$SOCK" ] || return 0
    "$APPDIR/bin/tailscale" --socket="$SOCK" down >> "$LOGF" 2>&1
}

do_stop() {
    if [ -S "$SOCK" ]; then
        "$APPDIR/bin/tailscale" --socket="$SOCK" down >/dev/null 2>&1 || true
    fi
    p=$(find_daemon_pid)
    if [ -n "$p" ]; then
        kill -TERM "$p" 2>/dev/null || true
        i=0
        while [ "$i" -lt 15 ]; do
            kill -0 "$p" 2>/dev/null || break
            i=$((i + 1))
            sleep 0.2 2>/dev/null || sleep 1
        done
        kill -0 "$p" 2>/dev/null && kill -KILL "$p" 2>/dev/null || true
    fi
    rm -f "$PIDF" "$SOCK" "$MODEF"
    return 0
}

do_logout() {
    [ -S "$SOCK" ] || return 1
    "$APPDIR/bin/tailscale" --socket="$SOCK" logout >> "$LOGF" 2>&1
}

do_status() {
    [ -S "$SOCK" ] || return 1
    "$APPDIR/bin/tailscale" --socket="$SOCK" status --json 2>/dev/null
}

do_ping() {
    target="$1"
    [ -S "$SOCK" ] || { echo "daemon not running"; return 1; }
    [ -n "$target" ] || { echo "missing target"; return 1; }
    "$APPDIR/bin/tailscale" --socket="$SOCK" ping -c 1 --timeout=5s "$target" 2>&1
}

do_boot() {
    i=0
    while [ "$i" -lt 45 ]; do
        up=0
        for wf in /sys/class/net/wl*/operstate; do
            if [ -r "$wf" ] && [ "$(cat "$wf" 2>/dev/null)" = "up" ]; then
                up=1
                break
            fi
        done
        [ "$up" = "1" ] && break
        i=$((i + 1))
        sleep 1
    done
    load_cfg
    do_start || exit 1
    if [ -s "$STATEFILE" ] || [ -n "$AUTH_KEY" ]; then
        do_up || true
    fi
}

case "$1" in
    start)  do_start ;;
    up)     do_up ;;
    down)   do_down ;;
    stop)   do_stop ;;
    logout) do_logout ;;
    status) do_status ;;
    ping)   do_ping "$2" ;;
    boot)   do_boot ;;
    *)      echo "Usage: $0 {start|up|down|stop|logout|status|ping <ip>|boot}"; exit 2 ;;
esac
