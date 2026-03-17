#!/bin/bash
# Test: Human message authentication (Ed25519 signing)
#
# Group A: Backward compatibility — wire format with sig field (6 tests)
# Group B: Terminal integration via pty-session (5 tests)
# Group C: Auth unit tests — compiled C (2 tests)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_CHAT="${NBS_CHAT_BIN:-$PROJECT_ROOT/bin/nbs-chat}"
NBS_TERMINAL="${NBS_TERMINAL_BIN:-$PROJECT_ROOT/bin/nbs-chat-terminal}"
PTY_SESSION="${PTY_SESSION_BIN:-$PROJECT_ROOT/bin/pty-session}"

export PATH="$PROJECT_ROOT/bin:$PATH"
export NBS_PTY_QUIET=1

TEST_DIR=$(mktemp -d)
ERRORS=0
PASS_COUNT=0
SKIP_COUNT=0

cleanup() {
    for s in $("$PTY_SESSION" list 2>/dev/null | grep "auth-test-$$" | awk '{print $1}'); do
        "$PTY_SESSION" kill "$s" 2>/dev/null || true
    done
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

check() {
    local label="$1" result="$2"
    if [[ "$result" == "pass" ]]; then
        printf "   PASS: %s\n" "$label"; PASS_COUNT=$((PASS_COUNT + 1))
    elif [[ "$result" == "skip" ]]; then
        printf "   SKIP: %s\n" "$label"; SKIP_COUNT=$((SKIP_COUNT + 1))
    else
        printf "   FAIL: %s\n" "$label"; ERRORS=$((ERRORS + 1))
    fi
}

craft_signed_line() {
    echo -n "${1}|${2}|${3}: ${4}" | base64
}

create_auth_project() {
    local proj="$TEST_DIR/${1}_proj"
    mkdir -p "$proj/.nbs/chat/trusted-keys" "$proj/.nbs/events/processed"
    "$NBS_CHAT" create "$proj/.nbs/chat/test.chat" >/dev/null
    echo "$proj/.nbs/chat/test.chat"
}

echo "=== Human Message Authentication Tests ==="
echo ""

# ================================================================
# Group A: Backward compatibility
# ================================================================
echo "--- Group A: Backward compatibility ---"
echo ""

CHAT="$TEST_DIR/a1.chat"
"$NBS_CHAT" create "$CHAT" >/dev/null
echo "$(craft_signed_line alex 1742248800 deadbeef0123456789abcdef "Hello with sig")" >> "$CHAT"
"$NBS_CHAT" send "$CHAT" agent "Normal message"
set +e; OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null); RC=$?; set -e
check "A1: read extended format no crash" "$([[ $RC -eq 0 ]] && echo pass || echo fail)"
check "A2: normal msg readable" "$(echo "$OUTPUT" | grep -qF 'Normal message' && echo pass || echo fail)"
check "A3: handle parsed" "$(echo "$OUTPUT" | grep -q 'alex' && echo pass || echo fail)"

CHAT="$TEST_DIR/a4.chat"; "$NBS_CHAT" create "$CHAT" >/dev/null
echo "$(craft_signed_line alex 1742248800 "s1|s2|s3" "Piped")" >> "$CHAT"
set +e; "$NBS_CHAT" read "$CHAT" >/dev/null 2>&1; RC=$?; set -e
check "A4: many pipes no crash" "$([[ $RC -eq 0 ]] && echo pass || echo fail)"

CHAT="$TEST_DIR/a5.chat"; "$NBS_CHAT" create "$CHAT" >/dev/null
echo "$(craft_signed_line alex 1742248800 "" "Empty sig")" >> "$CHAT"
set +e; "$NBS_CHAT" read "$CHAT" >/dev/null 2>&1; RC=$?; set -e
check "A5: empty sig no crash" "$([[ $RC -eq 0 ]] && echo pass || echo fail)"

CHAT="$TEST_DIR/a6.chat"; "$NBS_CHAT" create "$CHAT" >/dev/null
LSIG=$(python3 -c "print('a'*512)" 2>/dev/null || printf '%0512d' 0 | tr 0 a)
echo "$(craft_signed_line alex 1742248800 "$LSIG" "Long sig")" >> "$CHAT"
set +e; "$NBS_CHAT" read "$CHAT" >/dev/null 2>&1; RC=$?; set -e
check "A6: long sig no crash" "$([[ $RC -eq 0 ]] && echo pass || echo fail)"

echo ""

# ================================================================
# Group B: Terminal integration
# ================================================================
echo "--- Group B: Terminal integration ---"
echo ""

if [[ -x "$PTY_SESSION" ]]; then

    # B1: Passphrase prompt appears
    CHAT=$(create_auth_project b1)
    S="auth-test-$$-b1"
    "$PTY_SESSION" create "$S" "$NBS_TERMINAL $CHAT viewer" >/dev/null 2>&1
    set +e; "$PTY_SESSION" wait "$S" 'Passphrase' --timeout=10 >/dev/null 2>&1; WRC=$?; set -e
    "$PTY_SESSION" kill "$S" >/dev/null 2>&1 || true
    check "B1: passphrase prompt appears" "$([[ $WRC -eq 0 ]] && echo pass || echo fail)"

    # B2: Passphrase enables signing (check output after send + sleep)
    CHAT=$(create_auth_project b2)
    S="auth-test-$$-b2"
    "$PTY_SESSION" create "$S" "$NBS_TERMINAL $CHAT sender" >/dev/null 2>&1
    "$PTY_SESSION" wait "$S" 'Passphrase' --timeout=10 >/dev/null 2>&1
    sleep 1
    "$PTY_SESSION" send "$S" 'test-pass' >/dev/null 2>&1
    sleep 3
    OUTPUT=$("$PTY_SESSION" read "$S" 2>/dev/null)
    "$PTY_SESSION" kill "$S" >/dev/null 2>&1 || true
    check "B2: passphrase enables signing" "$(echo "$OUTPUT" | grep -qF 'signing enabled' && echo pass || echo fail)"

    # B3: Trusted key created
    CHAT=$(create_auth_project b3)
    PROJ=$(dirname "$(dirname "$(dirname "$CHAT")")")
    S="auth-test-$$-b3"
    "$PTY_SESSION" create "$S" "$NBS_TERMINAL $CHAT sender" >/dev/null 2>&1
    "$PTY_SESSION" wait "$S" 'Passphrase' --timeout=10 >/dev/null 2>&1
    sleep 1
    "$PTY_SESSION" send "$S" 'key-pass' >/dev/null 2>&1
    sleep 3
    "$PTY_SESSION" kill "$S" >/dev/null 2>&1 || true
    check "B3: pubkey file created" "$([[ -f "$PROJ/.nbs/chat/trusted-keys/sender.pub" ]] && echo pass || echo fail)"

    # B4: UNVERIFIED marker for unsigned known-handle message
    CHAT=$(create_auth_project b4)
    PROJ=$(dirname "$(dirname "$(dirname "$CHAT")")")
    "$NBS_CHAT" send "$CHAT" alex "Unsigned message"
    echo "0000000000000000000000000000000000000000000000000000000000000000" > "$PROJ/.nbs/chat/trusted-keys/alex.pub"
    S="auth-test-$$-b4"
    "$PTY_SESSION" create "$S" "$NBS_TERMINAL $CHAT viewer" >/dev/null 2>&1
    "$PTY_SESSION" wait "$S" 'Passphrase' --timeout=10 >/dev/null 2>&1
    sleep 1
    "$PTY_SESSION" send "$S" 'viewer-pass' >/dev/null 2>&1
    sleep 3
    OUTPUT=$("$PTY_SESSION" read "$S" 2>/dev/null)
    "$PTY_SESSION" kill "$S" >/dev/null 2>&1 || true
    check "B4: unsigned known-handle shows UNVERIFIED" "$(echo "$OUTPUT" | grep -qF 'UNVERIFIED' && echo pass || echo fail)"

    # B5: Signed message content visible
    CHAT=$(create_auth_project b5)
    PROJ=$(dirname "$(dirname "$(dirname "$CHAT")")")
    S="auth-test-$$-b5"
    "$PTY_SESSION" create "$S" "$NBS_TERMINAL $CHAT alex" >/dev/null 2>&1
    "$PTY_SESSION" wait "$S" 'Passphrase' --timeout=10 >/dev/null 2>&1
    sleep 1
    "$PTY_SESSION" send "$S" 'sign-pass' >/dev/null 2>&1
    sleep 3
    "$PTY_SESSION" send "$S" 'Hello signed world' >/dev/null 2>&1
    sleep 2
    # Verify message appears in chat file
    OUTPUT=$("$NBS_CHAT" read "$CHAT" 2>/dev/null)
    "$PTY_SESSION" kill "$S" >/dev/null 2>&1 || true
    check "B5: signed message in chat" "$(echo "$OUTPUT" | grep -qF 'Hello signed world' && echo pass || echo fail)"

    echo ""

else
    echo "(pty-session not available)"
    for t in "B1: prompt" "B2: enable" "B3: pubkey" "B4: UNVERIFIED" "B5: signed msg"; do
        check "$t" "skip"
    done
    echo ""
fi

# ================================================================
# Group C: Auth unit tests (compiled)
# ================================================================
echo "--- Group C: Auth unit tests ---"
echo ""

AUTH_BIN="$TEST_DIR/test_auth_unit"
set +e
gcc -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L \
    -D_DEFAULT_SOURCE -O2 \
    -I "$PROJECT_ROOT/src/nbs-chat" -I "$PROJECT_ROOT/src/nbs-common" \
    -o "$AUTH_BIN" "$PROJECT_ROOT/tests/test_auth_unit.c" \
    "$PROJECT_ROOT/src/nbs-chat/auth.c" -lcrypto 2>/dev/null
CRC=$?
set -e

if [[ $CRC -eq 0 ]]; then
    check "C1: auth unit tests compile" "pass"
    set +e; AUTH_OUT=$("$AUTH_BIN" 2>&1); ARC=$?; set -e
    AP=$(echo "$AUTH_OUT" | grep -oP '\d+ passed' | grep -oP '\d+' || echo 0)
    AF=$(echo "$AUTH_OUT" | grep -oP '\d+ failed' | grep -oP '\d+' || echo 0)
    check "C2: auth unit tests $AP/$((AP+AF)) pass" "$([[ $ARC -eq 0 ]] && echo pass || echo fail)"
else
    check "C1: compile" "fail"
    check "C2: pass" "skip"
fi

echo ""
echo "=== Results: $PASS_COUNT passed, $ERRORS failed, $SKIP_COUNT skipped ==="
[[ $ERRORS -eq 0 ]]
