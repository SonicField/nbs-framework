#!/bin/bash
# test_nbs_chat_web.sh — Integration tests for nbs-chat-web
#
# Tests: HTML serving, JSON API, SSE live updates, error handling.
#
# Exit codes:
#   0 - All tests passed
#   1 - One or more tests failed

set -uo pipefail

# --- Setup ---

# Bypass corporate proxy for localhost connections
export no_proxy="localhost,127.0.0.1,::1,[::1]"
export NO_PROXY="$no_proxy"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
NBS_CHAT="$REPO_DIR/bin/nbs-chat"
NBS_WEB="$REPO_DIR/bin/nbs-chat-web"

PASS=0
FAIL=0
WEB_PID=0

# Kill a background process safely
killbg() {
    local pid="$1"
    if [ "$pid" -gt 0 ] && kill -0 "$pid" 2>/dev/null; then
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null || true
    fi
}

check() {
    local desc="$1"
    shift
    if "$@" >/dev/null 2>&1; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc"
        FAIL=$((FAIL + 1))
    fi
}

check_output() {
    local desc="$1"
    local expected="$2"
    local actual="$3"
    if echo "$actual" | grep -qF "$expected"; then
        echo "  PASS: $desc"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $desc (expected '$expected', got '$actual')"
        FAIL=$((FAIL + 1))
    fi
}

# Create temp directory
TMPDIR=$(mktemp -d)
cleanup() {
    killbg "$WEB_PID"
    rm -rf "$TMPDIR"
}
trap cleanup EXIT

CHAT_FILE="$TMPDIR/test.chat"

echo "=== nbs-chat-web integration tests ==="

# --- Test 1: Missing chat file exits with code 2 ---

echo ""
echo "--- Test 1: Exit code for missing file ---"
"$NBS_WEB" "$TMPDIR/nonexistent.chat" 2>/dev/null
EXIT_CODE=$?
check "Missing file exits with code 2" test "$EXIT_CODE" -eq 2

# --- Test 2: Create chat and start server ---

echo ""
echo "--- Test 2: Server starts and serves HTML ---"
"$NBS_CHAT" create "$CHAT_FILE"
"$NBS_CHAT" send "$CHAT_FILE" alice "Hello from Alice"
"$NBS_CHAT" send "$CHAT_FILE" bob "Hello from Bob"
"$NBS_CHAT" send "$CHAT_FILE" alice "Testing \"quotes\" and \\backslash and <tags>"

# Start server on random port
"$NBS_WEB" "$CHAT_FILE" --port=0 > "$TMPDIR/web.out" 2>&1 &
WEB_PID=$!
sleep 1

# Extract port and base URL (handles both IPv4 and IPv6 output)
PORT=$(grep -oP ':\K[0-9]+(?=/)' "$TMPDIR/web.out")
if [ -z "$PORT" ]; then
    echo "  FAIL: Could not extract port from server output"
    cat "$TMPDIR/web.out"
    exit 1
fi
# Determine base URL from server output (IPv6 uses [::1], IPv4 uses 127.0.0.1)
BASE_URL=$(grep -oP 'http://\K[^\s]+(?=/)' "$TMPDIR/web.out")
BASE_URL="http://${BASE_URL}/"
# For curl: strip trailing slash, we add paths below
BASE_URL="${BASE_URL%/}"
echo "  Server started on port $PORT (PID $WEB_PID)"

# Test HTML response
HTTP_CODE=$(curl -s -o /dev/null -w '%{http_code}' "$BASE_URL/")
check "GET / returns 200" test "$HTTP_CODE" = "200"

# Check content-type
CT=$(curl -s -D - -o /dev/null "$BASE_URL/" 2>/dev/null | grep -i "content-type:" | tr -d '\r')
check_output "Content-Type is text/html" "text/html" "$CT"

# Check HTML contains expected elements
HTML=$(curl -s "$BASE_URL/")
check_output "HTML contains <html>" "<html" "$HTML"
check_output "HTML contains nbs-chat title" "nbs-chat" "$HTML"
check_output "HTML contains EventSource" "EventSource" "$HTML"

# --- Test 3: 404 for unknown paths ---

echo ""
echo "--- Test 3: Unknown path returns 404 ---"
HTTP_404=$(curl -s -o /dev/null -w '%{http_code}' "$BASE_URL/bogus")
check "GET /bogus returns 404" test "$HTTP_404" = "404"

# --- Test 4: JSON API ---

echo ""
echo "--- Test 4: JSON API endpoint ---"
JSON_CODE=$(curl -s -o /dev/null -w '%{http_code}' "$BASE_URL/api/messages")
check "GET /api/messages returns 200" test "$JSON_CODE" = "200"

# Validate JSON with jq
JSON=$(curl -s "$BASE_URL/api/messages")
TOTAL=$(echo "$JSON" | jq '.total_count' 2>/dev/null)
check "JSON is valid (jq can parse)" test "$TOTAL" = "3"

# Check message content
FIRST_HANDLE=$(echo "$JSON" | jq -r '.messages[0].handle' 2>/dev/null)
check "First message handle is alice" test "$FIRST_HANDLE" = "alice"

FIRST_CONTENT=$(echo "$JSON" | jq -r '.messages[0].content' 2>/dev/null)
check "First message content correct" test "$FIRST_CONTENT" = "Hello from Alice"

# Check participants
ALICE_COUNT=$(echo "$JSON" | jq -r '.participants[] | select(.handle=="alice") | .count' 2>/dev/null)
check "Alice has 2 messages" test "$ALICE_COUNT" = "2"

# --- Test 5: JSON API with filters ---

echo ""
echo "--- Test 5: JSON API filters ---"

# since=0 should return messages 1 and 2
SINCE_JSON=$(curl -s "$BASE_URL/api/messages?since=0")
SINCE_COUNT=$(echo "$SINCE_JSON" | jq '.messages | length' 2>/dev/null)
check "since=0 returns 2 messages" test "$SINCE_COUNT" = "2"

# last=1 should return only the last message
LAST_JSON=$(curl -s "$BASE_URL/api/messages?last=1")
LAST_COUNT=$(echo "$LAST_JSON" | jq '.messages | length' 2>/dev/null)
check "last=1 returns 1 message" test "$LAST_COUNT" = "1"

LAST_HANDLE=$(echo "$LAST_JSON" | jq -r '.messages[0].handle' 2>/dev/null)
check "last=1 returns alice (last sender)" test "$LAST_HANDLE" = "alice"

# --- Test 6: Special characters in JSON ---

echo ""
echo "--- Test 6: Special character escaping ---"

SPECIAL_CONTENT=$(echo "$JSON" | jq -r '.messages[2].content' 2>/dev/null)
check_output "Quotes escaped correctly" 'quotes' "$SPECIAL_CONTENT"
check_output "Backslash escaped correctly" 'backslash' "$SPECIAL_CONTENT"
check_output "Tags escaped correctly" 'tags' "$SPECIAL_CONTENT"

# --- Test 7: SSE connection ---

echo ""
echo "--- Test 7: SSE live updates ---"

# Connect SSE and capture initial messages
timeout 5 curl -s -N "$BASE_URL/events" > "$TMPDIR/sse.out" 2>&1 &
SSE_PID=$!
sleep 2

# Check initial messages arrived
SSE_LINES=$(wc -l < "$TMPDIR/sse.out")
check "SSE sends initial messages" test "$SSE_LINES" -gt 3

# Check first event format
check_output "SSE has id field" "id:" "$(head -1 "$TMPDIR/sse.out")"
check_output "SSE has event field" "event: message" "$(head -2 "$TMPDIR/sse.out" | tail -1)"
check_output "SSE has data field" "data:" "$(head -3 "$TMPDIR/sse.out" | tail -1)"

# Send a new message and check SSE receives it
"$NBS_CHAT" send "$CHAT_FILE" charlie "New live message"
sleep 3

SSE_OUT=$(cat "$TMPDIR/sse.out")
check_output "SSE receives new message" "New live message" "$SSE_OUT"

killbg $SSE_PID

# --- Test 8: SSE reconnection with Last-Event-ID ---

echo ""
echo "--- Test 8: SSE reconnection ---"

# Connect with Last-Event-ID=2 (skip first 3 messages)
timeout 5 curl -s -N -H "Last-Event-ID: 2" "$BASE_URL/events" > "$TMPDIR/sse2.out" 2>&1 &
SSE_PID=$!
sleep 2

SSE2_OUT=$(cat "$TMPDIR/sse2.out")
# Should contain message index 3 (the new one) but not index 0
check_output "Reconnection gets message after Last-Event-ID" "charlie" "$SSE2_OUT"

killbg $SSE_PID

# --- Test 9: Method not allowed ---

echo ""
echo "--- Test 9: Method handling ---"
POST_CODE=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$BASE_URL/")
check "POST returns 405" test "$POST_CODE" = "405"

# --- Cleanup ---

killbg $WEB_PID

# --- Summary ---

echo ""
echo "=== Results: $PASS passed, $FAIL failed ==="

if [ "$FAIL" -gt 0 ]; then
    exit 1
fi
exit 0
