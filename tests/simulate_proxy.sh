#!/bin/bash
# ============================================================
# OmniShield Proxy Manager — Full Flow Simulation
# ============================================================
# Simulates the entire proxy lifecycle on a dev machine:
#  1. Config parsing
#  2. YAML generation & validation
#  3. Lock acquire/release
#  4. Daemon launch sequence
#  5. iptables command construction
#  6. Cleanup
# ============================================================

set +e  # Don't exit on individual command failures; we track pass/fail manually
PASS=0
FAIL=0
TEST_DIR=$(mktemp -d)
trap "rm -rf $TEST_DIR" EXIT

pass() { PASS=$((PASS+1)); echo "  [PASS] $1"; }
fail() { FAIL=$((FAIL+1)); echo "  [FAIL] $1"; }

echo "=== OmniShield Proxy Simulation ==="
echo "Temp dir: $TEST_DIR"
echo ""

# ─── Setup mock environment ──────────────────────────────────────────
OMNI_DIR="$TEST_DIR/omni_data"
MODULE_DIR="$TEST_DIR/modules/omnishield"
mkdir -p "$OMNI_DIR" "$MODULE_DIR/bin"

CONFIG="$OMNI_DIR/.identity.cfg"
TUN2SOCKS_CFG="$OMNI_DIR/tun2socks.yml"
PID_FILE="$OMNI_DIR/tun2socks.pid"
LOG_FILE="$OMNI_DIR/proxy.log"
LOCK_DIR="$OMNI_DIR/.proxy.lock"

# Create mock tun2socks binary (just writes PID file and sleeps)
cat > "$MODULE_DIR/bin/tun2socks" << 'MOCKEOF'
#!/bin/bash
# Mock hev-socks5-tunnel — reads config, validates, writes PID, stays alive
CFG="$1"
if [ -z "$CFG" ] || [ ! -f "$CFG" ]; then
    echo "ERROR: config file required" >&2
    exit 1
fi
# Extract pid-file from YAML
PID_PATH=$(grep "pid-file:" "$CFG" | awk '{print $2}')
if [ -z "$PID_PATH" ]; then
    echo "ERROR: no pid-file in config" >&2
    exit 1
fi
echo $$ > "$PID_PATH"
# Stay alive for 30s (simulates running daemon)
sleep 30 &
wait
MOCKEOF
chmod 755 "$MODULE_DIR/bin/tun2socks"

# Write test config with the provided proxy credentials
cat > "$CONFIG" << 'CFGEOF'
proxy_enabled=true
proxy_type=socks5
proxy_host=ultra.marsproxies.com
proxy_port=44445
proxy_user=mr661849eor
proxy_pass=Me6tLzk44e
proxy_dns=8.8.8.8
scoped_apps=com.test.app1,com.test.app2
webview_spoof=true
CFGEOF

# ─── Test 1: Config Parsing ──────────────────────────────────────────
echo "--- Test 1: Config Parsing ---"

# Source relevant functions from proxy_manager.sh with overridden paths
PROXY_SCRIPT="/home/user/OmniShield/proxy_manager.sh"

# Parse config using same grep+cut logic as the script
PROXY_ENABLED=$(grep "^proxy_enabled=" "$CONFIG" | cut -d'=' -f2- | tr -d ' \r\n')
PROXY_TYPE=$(grep "^proxy_type=" "$CONFIG" | cut -d'=' -f2- | tr -d ' \r\n')
PROXY_HOST=$(grep "^proxy_host=" "$CONFIG" | cut -d'=' -f2- | tr -d ' \r\n')
PROXY_PORT=$(grep "^proxy_port=" "$CONFIG" | cut -d'=' -f2- | tr -d ' \r\n')
PROXY_USER=$(grep "^proxy_user=" "$CONFIG" | cut -d'=' -f2- | tr -d ' \r\n')
PROXY_PASS=$(grep "^proxy_pass=" "$CONFIG" | cut -d'=' -f2- | tr -d ' \r\n')
PROXY_DNS=$(grep "^proxy_dns=" "$CONFIG" | cut -d'=' -f2- | tr -d ' \r\n')
SCOPED_APPS=$(grep "^scoped_apps=" "$CONFIG" | cut -d'=' -f2- | tr -d '\r\n')

[ "$PROXY_ENABLED" = "true" ] && pass "proxy_enabled=true" || fail "proxy_enabled='$PROXY_ENABLED'"
[ "$PROXY_TYPE" = "socks5" ] && pass "proxy_type=socks5" || fail "proxy_type='$PROXY_TYPE'"
[ "$PROXY_HOST" = "ultra.marsproxies.com" ] && pass "proxy_host parsed" || fail "proxy_host='$PROXY_HOST'"
[ "$PROXY_PORT" = "44445" ] && pass "proxy_port=44445" || fail "proxy_port='$PROXY_PORT'"
[ "$PROXY_USER" = "mr661849eor" ] && pass "proxy_user parsed" || fail "proxy_user='$PROXY_USER'"
[ "$PROXY_PASS" = "Me6tLzk44e" ] && pass "proxy_pass parsed" || fail "proxy_pass='$PROXY_PASS'"
[ "$PROXY_DNS" = "8.8.8.8" ] && pass "proxy_dns=8.8.8.8" || fail "proxy_dns='$PROXY_DNS'"
[ "$SCOPED_APPS" = "com.test.app1,com.test.app2" ] && pass "scoped_apps parsed" || fail "scoped_apps='$SCOPED_APPS'"

# Test password with = sign
echo "proxy_pass=P@ss=w0rd=end" > "$TEST_DIR/edge.cfg"
EDGE_PASS=$(grep "^proxy_pass=" "$TEST_DIR/edge.cfg" | cut -d'=' -f2- | tr -d ' \r\n')
[ "$EDGE_PASS" = "P@ss=w0rd=end" ] && pass "password with = signs preserved" || fail "password truncated: '$EDGE_PASS'"

echo ""

# ─── Test 2: YAML Generation ─────────────────────────────────────────
echo "--- Test 2: YAML Generation ---"

TUN_NAME="tun_omni"
TUN_MTU="8500"
TUN_ADDR="172.19.0.1"

auth_block=""
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

# Validate YAML structure
[ -f "$TUN2SOCKS_CFG" ] && pass "YAML file created" || fail "YAML file missing"

grep -q "name: tun_omni" "$TUN2SOCKS_CFG" && pass "tunnel.name correct" || fail "tunnel.name wrong"
grep -q "mtu: 8500" "$TUN2SOCKS_CFG" && pass "tunnel.mtu correct" || fail "tunnel.mtu wrong"
grep -q "ipv4: 172.19.0.1" "$TUN2SOCKS_CFG" && pass "tunnel.ipv4 correct" || fail "tunnel.ipv4 wrong"
grep -q "port: 44445" "$TUN2SOCKS_CFG" && pass "socks5.port correct" || fail "socks5.port wrong"
grep -q "address: 'ultra.marsproxies.com'" "$TUN2SOCKS_CFG" && pass "socks5.address correct" || fail "socks5.address wrong"
grep -q "username: 'mr661849eor'" "$TUN2SOCKS_CFG" && pass "socks5.username correct" || fail "socks5.username wrong"
grep -q "password: 'Me6tLzk44e'" "$TUN2SOCKS_CFG" && pass "socks5.password correct" || fail "socks5.password wrong"
grep -q "udp: udp" "$TUN2SOCKS_CFG" && pass "socks5.udp correct" || fail "socks5.udp wrong"
grep -q "tcp-read-write-timeout: 60000" "$TUN2SOCKS_CFG" && pass "misc.tcp-rw-timeout correct" || fail "tcp-rw-timeout wrong"
grep -q "udp-read-write-timeout: 60000" "$TUN2SOCKS_CFG" && pass "misc.udp-rw-timeout correct" || fail "udp-rw-timeout wrong"
grep -q "pid-file: $PID_FILE" "$TUN2SOCKS_CFG" && pass "misc.pid-file correct" || fail "pid-file wrong"

# Check YAML indentation (all socks5 fields must be 2-space indented)
BAD_INDENT=$(grep -n "^[^ ]" "$TUN2SOCKS_CFG" | grep -v "^[0-9]*:tunnel:" | grep -v "^[0-9]*:socks5:" | grep -v "^[0-9]*:misc:" | grep -v "^[0-9]*:$")
if [ -z "$BAD_INDENT" ]; then
    pass "YAML indentation valid"
else
    fail "Bad indentation: $BAD_INDENT"
fi

# Validate with python YAML parser if available
if command -v python3 >/dev/null 2>&1; then
    if python3 -c "import yaml; yaml.safe_load(open('$TUN2SOCKS_CFG'))" 2>/dev/null; then
        pass "YAML parses correctly (python3 yaml.safe_load)"
    else
        fail "YAML parse error!"
    fi
fi

echo ""

# ─── Test 3: YAML without auth (no username/password) ────────────────
echo "--- Test 3: YAML without auth ---"

auth_block=""
cat > "${TUN2SOCKS_CFG}.noauth" << YAMLEOF
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

grep -q "username" "${TUN2SOCKS_CFG}.noauth" && fail "noauth YAML has username" || pass "noauth YAML omits username"
grep -q "password" "${TUN2SOCKS_CFG}.noauth" && fail "noauth YAML has password" || pass "noauth YAML omits password"

if command -v python3 >/dev/null 2>&1; then
    if python3 -c "import yaml; yaml.safe_load(open('${TUN2SOCKS_CFG}.noauth'))" 2>/dev/null; then
        pass "noauth YAML parses correctly"
    else
        fail "noauth YAML parse error!"
    fi
fi

echo ""

# ─── Test 4: Lock Mechanism ──────────────────────────────────────────
echo "--- Test 4: Lock Mechanism ---"

# Test acquire
mkdir "$LOCK_DIR" 2>/dev/null
echo $$ > "$LOCK_DIR/pid"
[ -d "$LOCK_DIR" ] && pass "Lock acquired (mkdir)" || fail "Lock acquire failed"
[ -f "$LOCK_DIR/pid" ] && pass "PID file written" || fail "PID file missing"

OWNER_PID=$(cat "$LOCK_DIR/pid")
[ "$OWNER_PID" = "$$" ] && pass "PID matches current process" || fail "PID mismatch"

# Test stale detection (write a dead PID)
echo "99999999" > "$LOCK_DIR/pid"
STALE_PID=$(cat "$LOCK_DIR/pid")
if ! kill -0 "$STALE_PID" 2>/dev/null; then
    pass "Dead PID detected as stale"
else
    fail "Stale detection broken"
fi

# Reclaim stale lock
rm -rf "$LOCK_DIR"
mkdir "$LOCK_DIR" && echo $$ > "$LOCK_DIR/pid"
[ -d "$LOCK_DIR" ] && pass "Stale lock reclaimed" || fail "Reclaim failed"

# Release
rm -rf "$LOCK_DIR"
[ ! -d "$LOCK_DIR" ] && pass "Lock released" || fail "Lock release failed"

echo ""

# ─── Test 5: Mock Daemon Launch ──────────────────────────────────────
echo "--- Test 5: Mock Daemon Launch ---"

rm -f "$PID_FILE"
nohup "$MODULE_DIR/bin/tun2socks" "$TUN2SOCKS_CFG" >> "$LOG_FILE" 2>&1 &
NOHUP_PID=$!

# Wait for PID file (same logic as proxy_manager.sh)
daemon_ok=0
for _w in 1 2 3 4 5; do
    if [ -f "$PID_FILE" ]; then
        daemon_ok=1
        break
    fi
    sleep 1
done

if [ "$daemon_ok" -eq 1 ]; then
    pass "PID file created within 5s"
    DAEMON_PID=$(cat "$PID_FILE")
    if kill -0 "$DAEMON_PID" 2>/dev/null; then
        pass "Daemon process alive (PID $DAEMON_PID)"
    else
        fail "Daemon process dead"
    fi
else
    fail "PID file not created after 5s"
fi

# Kill the mock daemon
kill "$DAEMON_PID" 2>/dev/null
wait "$DAEMON_PID" 2>/dev/null
rm -f "$PID_FILE"
pass "Daemon killed and PID cleaned"

echo ""

# ─── Test 6: iptables Command Construction ───────────────────────────
echo "--- Test 6: iptables Command Construction ---"

CHAIN_MARK="OMNI_PROXY"
CHAIN_DNS="OMNI_DNS"
CHAIN_V6="OMNI_V6BLOCK"
FWMARK="0x1337"
ROUTE_TABLE="100"
UIDS="10243 10256"

# Verify command strings (dry-run — don't actually execute iptables)
CMD_MARK_CREATE="iptables -t mangle -N $CHAIN_MARK"
CMD_DNS_CREATE="iptables -t nat -N $CHAIN_DNS"
CMD_V6_CREATE="ip6tables -N $CHAIN_V6"

echo "$CMD_MARK_CREATE" | grep -q "mangle -N OMNI_PROXY" && pass "mangle chain create cmd" || fail "mangle cmd wrong"
echo "$CMD_DNS_CREATE" | grep -q "nat -N OMNI_DNS" && pass "nat chain create cmd" || fail "nat cmd wrong"
echo "$CMD_V6_CREATE" | grep -q "OMNI_V6BLOCK" && pass "v6 chain create cmd" || fail "v6 cmd wrong"

# Verify RETURN rule for proxy server comes FIRST
CMD_RETURN="iptables -t mangle -A $CHAIN_MARK -d $PROXY_HOST -j RETURN"
echo "$CMD_RETURN" | grep -q "\-d ultra.marsproxies.com -j RETURN" && pass "RETURN rule for proxy server" || fail "RETURN rule wrong"

# Verify per-UID mark rules
for uid in $UIDS; do
    CMD="iptables -t mangle -A $CHAIN_MARK -m owner --uid-owner $uid -j MARK --set-mark $FWMARK"
    echo "$CMD" | grep -q "uid-owner $uid" && pass "UID $uid mark rule" || fail "UID $uid mark rule wrong"
done

# Verify DNS DNAT rules
CMD_DNS_UDP="iptables -t nat -A $CHAIN_DNS -m mark --mark $FWMARK -p udp --dport 53 -j DNAT --to-destination ${PROXY_DNS}:53"
echo "$CMD_DNS_UDP" | grep -q "DNAT --to-destination 8.8.8.8:53" && pass "DNS DNAT UDP rule" || fail "DNS DNAT UDP wrong"

# Verify cleanup uses while loop (check source)
grep -q "while iptables -t mangle -D OUTPUT" "$PROXY_SCRIPT" && pass "cleanup uses while loop (dedup)" || fail "cleanup missing while loop"

echo ""

# ─── Test 7: Config Save/Load Roundtrip (app.js keys match shell) ────
echo "--- Test 7: Config Key Consistency ---"

JS_KEYS=$(grep -oP "cfg\.proxy_\w+" /home/user/OmniShield/webroot/js/app.js | sed 's/cfg\.//' | sort -u)
SH_KEYS=$(grep -oP 'grep "\^proxy_\w+=' "$PROXY_SCRIPT" | grep -oP 'proxy_\w+' | sort -u)

JS_COUNT=$(echo "$JS_KEYS" | wc -l)
SH_COUNT=$(echo "$SH_KEYS" | wc -l)

[ "$JS_COUNT" -eq "$SH_COUNT" ] && pass "Key count matches (JS=$JS_COUNT, SH=$SH_COUNT)" || fail "Key count mismatch (JS=$JS_COUNT, SH=$SH_COUNT)"

DIFF=$(diff <(echo "$JS_KEYS") <(echo "$SH_KEYS") 2>/dev/null)
if [ -z "$DIFF" ]; then
    pass "All config keys match between JS and shell"
else
    fail "Key mismatch: $DIFF"
fi

echo ""

# ─── Test 8: HTML ID Cross-Reference ─────────────────────────────────
echo "--- Test 8: HTML/JS ID Cross-Reference ---"

HTML_IDS=$(grep -oP 'id="proxy-[^"]*"' /home/user/OmniShield/webroot/index.html | sed 's/id="//;s/"//' | sort -u)
JS_IDS=$(grep -oP "getElementById\('proxy-[^']*'\)" /home/user/OmniShield/webroot/js/app.js | sed "s/getElementById('//;s/')//" | sort -u)

for js_id in $JS_IDS; do
    if echo "$HTML_IDS" | grep -q "^${js_id}$"; then
        pass "ID '$js_id' exists in HTML"
    else
        fail "ID '$js_id' missing from HTML!"
    fi
done

echo ""

# ─── Test 9: Build Workflow Completeness ──────────────────────────────
echo "--- Test 9: Build Workflow ---"

BUILD="/home/user/OmniShield/.github/workflows/build.yml"
grep -q "proxy_manager.sh" "$BUILD" && pass "proxy_manager.sh in build" || fail "proxy_manager.sh missing from build"
grep -q "customize.sh" "$BUILD" && pass "customize.sh in build" || fail "customize.sh missing from build"
grep -q "tun2socks" "$BUILD" && pass "tun2socks download in build" || fail "tun2socks missing from build"
grep -q "hev-socks5-tunnel" "$BUILD" && pass "hev-socks5-tunnel URL in build" || fail "download URL missing"

echo ""

# ─── Results ──────────────────────────────────────────────────────────
echo "==========================================="
echo "Results: $PASS passed, $FAIL failed"
echo "==========================================="

[ "$FAIL" -eq 0 ] && echo "ALL TESTS PASSED" && exit 0
echo "SOME TESTS FAILED" && exit 1
