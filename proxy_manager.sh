#!/system/bin/sh
# ============================================================
# PR72: OmniShield Transparent Proxy Manager
# ============================================================
# Orchestrates a per-UID transparent SOCKS5 proxy using
# hev-socks5-tunnel, iptables marking, DNS hijacking, and
# IPv6 leak prevention.
#
# Usage: sh proxy_manager.sh {start|stop|restart|status}
# ============================================================

OMNI_DIR="/data/adb/.omni_data"
CONFIG="$OMNI_DIR/.identity.cfg"
MODULE_DIR="/data/adb/modules/omnishield"
TUN2SOCKS="$MODULE_DIR/bin/tun2socks"
TUN2SOCKS_CFG="$OMNI_DIR/tun2socks.yml"
PID_FILE="$OMNI_DIR/tun2socks.pid"
LOG_FILE="$OMNI_DIR/proxy.log"
LOCK_DIR="$OMNI_DIR/.proxy.lock"

# TUN interface config
TUN_NAME="tun_omni"
TUN_ADDR="172.19.0.1"
TUN_MTU="8500"

# iptables chain names (OMNI_ prefix — never collide with system)
CHAIN_MARK="OMNI_PROXY"
CHAIN_DNS="OMNI_DNS"
CHAIN_V6="OMNI_V6BLOCK"
FWMARK="0x1337"
ROUTE_TABLE="100"

# Default DNS when no DNS server is configured
DEFAULT_DNS="8.8.8.8"

# ─── Locking (mkdir is atomic, PID check detects stale locks) ─────────
acquire_lock() {
    if mkdir "$LOCK_DIR" 2>/dev/null; then
        echo $$ > "$LOCK_DIR/pid"
        return 0
    fi
    # Lock exists — check if the owning process is still alive
    local owner_pid
    owner_pid=$(cat "$LOCK_DIR/pid" 2>/dev/null)
    if [ -n "$owner_pid" ] && kill -0 "$owner_pid" 2>/dev/null; then
        log "ERROR: Another proxy_manager instance is running (PID $owner_pid)"
        return 1
    fi
    # Owner is dead — stale lock, reclaim it
    rm -rf "$LOCK_DIR"
    if mkdir "$LOCK_DIR" 2>/dev/null; then
        echo $$ > "$LOCK_DIR/pid"
        return 0
    fi
    log "ERROR: Could not acquire lock"
    return 1
}

release_lock() {
    rm -rf "$LOCK_DIR" 2>/dev/null
}

# ─── Logging ───────────────────────────────────────────────────────────
log() {
    echo "[$(date '+%H:%M:%S')] $1" >> "$LOG_FILE" 2>/dev/null
    echo "$1"
}

# ─── Read config from .identity.cfg ───────────────────────────────────
read_config() {
    if [ ! -f "$CONFIG" ]; then
        log "ERROR: Config not found: $CONFIG"
        return 1
    fi
    PROXY_ENABLED=$(grep "^proxy_enabled=" "$CONFIG" | cut -d'=' -f2- | tr -d ' \r\n')
    PROXY_TYPE=$(grep "^proxy_type=" "$CONFIG" | cut -d'=' -f2- | tr -d ' \r\n')
    PROXY_HOST=$(grep "^proxy_host=" "$CONFIG" | cut -d'=' -f2- | tr -d ' \r\n')
    PROXY_PORT=$(grep "^proxy_port=" "$CONFIG" | cut -d'=' -f2- | tr -d ' \r\n')
    PROXY_USER=$(grep "^proxy_user=" "$CONFIG" | cut -d'=' -f2- | tr -d ' \r\n')
    PROXY_PASS=$(grep "^proxy_pass=" "$CONFIG" | cut -d'=' -f2- | tr -d ' \r\n')
    PROXY_DNS=$(grep "^proxy_dns=" "$CONFIG" | cut -d'=' -f2- | tr -d ' \r\n')
    SCOPED_APPS=$(grep "^scoped_apps=" "$CONFIG" | cut -d'=' -f2- | tr -d '\r\n')

    [ -z "$PROXY_DNS" ] && PROXY_DNS="$DEFAULT_DNS"
}

# ─── Resolve package name → Linux UID ─────────────────────────────────
pkg_to_uid() {
    dumpsys package "$1" 2>/dev/null | grep "    userId=" | head -1 | sed 's/.*userId=\([0-9]*\).*/\1/'
}

# ─── Resolve hostname → IP (for iptables -d) ──────────────────────────
resolve_host() {
    # If already an IP, return as-is
    echo "$1" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$' && { echo "$1"; return; }
    # Try getent (available on some Android builds)
    local ip
    ip=$(getent hosts "$1" 2>/dev/null | head -1 | awk '{print $1}')
    [ -n "$ip" ] && { echo "$ip"; return; }
    # Try ping-based resolution
    ip=$(ping -c1 -W2 "$1" 2>/dev/null | head -1 | sed -n 's/.*(\([0-9.]*\)).*/\1/p')
    [ -n "$ip" ] && { echo "$ip"; return; }
    # Fallback: return original (iptables will attempt its own resolution)
    echo "$1"
}

# ─── Resolve all scoped apps to UIDs ──────────────────────────────────
resolve_uids() {
    UIDS=""
    if [ -z "$SCOPED_APPS" ]; then
        log "WARNING: No scoped apps — proxy will have no effect"
        return
    fi
    OLDIFS="$IFS"
    IFS=','
    for pkg in $SCOPED_APPS; do
        uid=$(pkg_to_uid "$pkg")
        if [ -n "$uid" ]; then
            UIDS="$UIDS $uid"
            log "  $pkg → UID $uid"
        else
            log "  WARNING: Could not resolve UID for $pkg"
        fi
    done
    IFS="$OLDIFS"
}

# ─── Generate hev-socks5-tunnel YAML config ───────────────────────────
# Built as a single heredoc to avoid fragile multi-append YAML issues.
generate_config() {
    local auth_block=""
    if [ -n "$PROXY_USER" ] && [ -n "$PROXY_PASS" ]; then
        auth_block="  username: '${PROXY_USER}'
  password: '${PROXY_PASS}'"
    fi

    cat > "$TUN2SOCKS_CFG" << YAMLEOF
tunnel:
  name: ${TUN_NAME}
  mtu: ${TUN_MTU}
  ipv4: ${TUN_ADDR}

socks5:
  port: ${PROXY_PORT}
  address: '${PROXY_HOST}'
${auth_block}
  udp: udp

misc:
  task-stack-size: 81920
  connect-timeout: 5000
  tcp-read-write-timeout: 60000
  udp-read-write-timeout: 60000
  log-file: ${LOG_FILE}
  log-level: warn
  pid-file: ${PID_FILE}
  limit-nofile: 65535
YAMLEOF

    chmod 600 "$TUN2SOCKS_CFG"
    log "Config written to $TUN2SOCKS_CFG"
}

# ─── Setup routing after TUN is up ────────────────────────────────────
# hev-socks5-tunnel creates the TUN interface internally.
# We only need to add policy routing after it's up.
setup_routing() {
    ip route add default dev "$TUN_NAME" table "$ROUTE_TABLE" 2>/dev/null
    log "Routing table $ROUTE_TABLE → $TUN_NAME"
}

# ─── Setup iptables rules (per-UID marking + routing) ─────────────────
setup_iptables() {
    # Resolve proxy host to IP for iptables (hostnames cause DNS lookups in iptables)
    local proxy_ip
    proxy_ip=$(resolve_host "$PROXY_HOST")

    # Create mangle chain for packet marking
    iptables -t mangle -N "$CHAIN_MARK" 2>/dev/null
    iptables -t mangle -F "$CHAIN_MARK"

    # Create nat chain for DNS hijacking
    iptables -t nat -N "$CHAIN_DNS" 2>/dev/null
    iptables -t nat -F "$CHAIN_DNS"

    # Create ip6tables chain for IPv6 blocking
    ip6tables -N "$CHAIN_V6" 2>/dev/null
    ip6tables -F "$CHAIN_V6"

    # Exclude proxy server itself from being marked (avoid routing loop)
    # Inserted FIRST so it takes priority over per-UID rules
    iptables -t mangle -A "$CHAIN_MARK" -d "$proxy_ip" -j RETURN

    # Add per-UID rules
    for uid in $UIDS; do
        # Mark packets from scoped UIDs
        iptables -t mangle -A "$CHAIN_MARK" -m owner --uid-owner "$uid" -j MARK --set-mark "$FWMARK"
        # Block IPv6 for scoped UIDs (force IPv4-only → all traffic through tunnel)
        ip6tables -A "$CHAIN_V6" -m owner --uid-owner "$uid" -j DROP
        log "  UID $uid: marked + IPv6 blocked"
    done

    # DNS hijack: redirect port 53 from marked packets to configured DNS
    iptables -t nat -A "$CHAIN_DNS" -m mark --mark "$FWMARK" -p udp --dport 53 -j DNAT --to-destination "${PROXY_DNS}:53"
    iptables -t nat -A "$CHAIN_DNS" -m mark --mark "$FWMARK" -p tcp --dport 53 -j DNAT --to-destination "${PROXY_DNS}:53"

    # Hook chains into OUTPUT
    iptables -t mangle -A OUTPUT -j "$CHAIN_MARK"
    iptables -t nat -A OUTPUT -j "$CHAIN_DNS"
    ip6tables -I OUTPUT -j "$CHAIN_V6"

    # Policy routing: marked packets use our route table
    ip rule add fwmark "$FWMARK" table "$ROUTE_TABLE" prio 100 2>/dev/null

    log "iptables rules applied (mark=$FWMARK, dns=$PROXY_DNS, proxy=$proxy_ip)"
}

# ─── Cleanup: remove all OMNI rules (idempotent) ──────────────────────
cleanup_iptables() {
    # Unhook from OUTPUT chains — loop to remove ALL instances (handles duplicates)
    while iptables -t mangle -D OUTPUT -j "$CHAIN_MARK" 2>/dev/null; do :; done
    while iptables -t nat -D OUTPUT -j "$CHAIN_DNS" 2>/dev/null; do :; done
    while ip6tables -D OUTPUT -j "$CHAIN_V6" 2>/dev/null; do :; done

    # Flush and delete our chains
    iptables -t mangle -F "$CHAIN_MARK" 2>/dev/null
    iptables -t mangle -X "$CHAIN_MARK" 2>/dev/null
    iptables -t nat -F "$CHAIN_DNS" 2>/dev/null
    iptables -t nat -X "$CHAIN_DNS" 2>/dev/null
    ip6tables -F "$CHAIN_V6" 2>/dev/null
    ip6tables -X "$CHAIN_V6" 2>/dev/null

    # Remove ALL policy routing rules (handles duplicates)
    while ip rule del fwmark "$FWMARK" table "$ROUTE_TABLE" 2>/dev/null; do :; done

    # Flush routing table
    ip route flush table "$ROUTE_TABLE" 2>/dev/null
}

# ─── Cleanup: destroy TUN interface ───────────────────────────────────
# Note: hev-socks5-tunnel creates the TUN internally via ioctl, so
# `ip tuntap del` may not work. We also try `ip link delete` as fallback.
cleanup_tun() {
    ip link set "$TUN_NAME" down 2>/dev/null
    ip link delete "$TUN_NAME" 2>/dev/null
    ip tuntap del mode tun "$TUN_NAME" 2>/dev/null
}

# ─── Kill tun2socks daemon ─────────────────────────────────────────────
kill_daemon() {
    if [ -f "$PID_FILE" ]; then
        PID=$(cat "$PID_FILE" 2>/dev/null)
        if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
            kill "$PID" 2>/dev/null
            # Wait up to 3 seconds for graceful shutdown
            for i in 1 2 3; do
                kill -0 "$PID" 2>/dev/null || break
                sleep 1
            done
            # Force kill if still alive
            kill -0 "$PID" 2>/dev/null && kill -9 "$PID" 2>/dev/null
        fi
        rm -f "$PID_FILE"
    fi
    # Also kill by name as safety net
    pkill -f "hev-socks5-tunnel.*$TUN2SOCKS_CFG" 2>/dev/null
}

# ═══════════════════════════════════════════════════════════════════════
# COMMANDS
# ═══════════════════════════════════════════════════════════════════════

do_start() {
    log "=== PROXY START ==="

    acquire_lock || return 1

    read_config || { release_lock; return 1; }

    # Validate
    if [ "$PROXY_ENABLED" != "true" ]; then
        log "Proxy is disabled in config"
        release_lock; return 1
    fi
    if [ "$PROXY_TYPE" != "socks5" ]; then
        log "ERROR: Only SOCKS5 proxies are supported (got: $PROXY_TYPE)"
        release_lock; return 1
    fi
    if [ -z "$PROXY_HOST" ]; then
        log "ERROR: No proxy host configured"
        release_lock; return 1
    fi
    if [ -z "$PROXY_PORT" ]; then
        log "ERROR: No proxy port configured"
        release_lock; return 1
    fi
    if [ ! -x "$TUN2SOCKS" ]; then
        log "ERROR: tun2socks binary not found or not executable at $TUN2SOCKS"
        log "Download from: https://github.com/heiher/hev-socks5-tunnel/releases"
        log "Place ARM64 static binary at: $TUN2SOCKS"
        release_lock; return 1
    fi

    # Clean previous state
    do_stop_quiet

    # Resolve UIDs
    log "Resolving scoped apps..."
    resolve_uids
    if [ -z "$UIDS" ]; then
        log "ERROR: No valid UIDs resolved — add apps to scope first"
        return 1
    fi

    # Generate tun2socks config
    generate_config

    # Launch hev-socks5-tunnel daemon (it creates the TUN interface internally)
    log "Launching hev-socks5-tunnel → $PROXY_HOST:$PROXY_PORT"
    nohup "$TUN2SOCKS" "$TUN2SOCKS_CFG" > /dev/null 2>&1 &

    # Wait for daemon to start and create PID file (retry up to 5s)
    local daemon_ok=0
    for _w in 1 2 3 4 5; do
        if [ -f "$PID_FILE" ]; then
            daemon_ok=1
            break
        fi
        sleep 1
    done

    if [ "$daemon_ok" -eq 1 ]; then
        DAEMON_PID=$(cat "$PID_FILE")
        if kill -0 "$DAEMON_PID" 2>/dev/null; then
            log "Daemon running (PID $DAEMON_PID)"
        else
            log "ERROR: Daemon started but died immediately — check $LOG_FILE"
            cleanup_tun
            release_lock; return 1
        fi
    else
        log "ERROR: Daemon failed to create PID file after 5s — check $LOG_FILE"
        cleanup_tun
        release_lock; return 1
    fi

    # Setup routing (TUN was created by hev-socks5-tunnel)
    setup_routing

    # Setup iptables rules
    setup_iptables

    log "=== PROXY ACTIVE ==="
    log "  Tunnel: $TUN_NAME ($TUN_ADDR)"
    log "  Server: $PROXY_HOST:$PROXY_PORT (SOCKS5)"
    log "  DNS:    $PROXY_DNS"
    log "  Apps:   $(echo $UIDS | wc -w) UIDs"
    release_lock
    return 0
}

do_stop() {
    log "=== PROXY STOP ==="
    acquire_lock || return 1
    do_stop_quiet
    release_lock
    log "=== PROXY STOPPED ==="
}

# Quiet stop (no log headers) — used internally by start for cleanup
do_stop_quiet() {
    kill_daemon
    cleanup_iptables
    cleanup_tun
    rm -f "$TUN2SOCKS_CFG"
}

do_status() {
    if [ -f "$PID_FILE" ]; then
        PID=$(cat "$PID_FILE" 2>/dev/null)
        if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
            echo "running"
            return 0
        fi
    fi
    echo "stopped"
    return 1
}

# ═══════════════════════════════════════════════════════════════════════
# ENTRY POINT
# ═══════════════════════════════════════════════════════════════════════

case "$1" in
    start)   do_start  ;;
    stop)    do_stop   ;;
    restart) do_stop; do_start ;;
    status)  do_status ;;
    *)
        echo "OmniShield Proxy Manager"
        echo "Usage: $0 {start|stop|restart|status}"
        exit 1
        ;;
esac
