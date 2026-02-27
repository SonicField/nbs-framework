#!/bin/bash
# Test: nbs-scribe-query
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
QUERY="$PROJECT_ROOT/bin/nbs-scribe-query"

TEST_DIR=$(mktemp -d)
trap "rm -rf '$TEST_DIR'" EXIT

PASS=0
FAIL=0

check() {
    if [[ "$2" == "pass" ]]; then
        echo "   PASS: $1"
        PASS=$((PASS + 1))
    else
        echo "   FAIL: $1"
        FAIL=$((FAIL + 1))
    fi
}

next_test() { echo ""; echo "$1"; }

# Create test directory structure matching .nbs layout
mkdir -p "$TEST_DIR/.nbs/chat"
mkdir -p "$TEST_DIR/.nbs/scribe"
CHAT="$TEST_DIR/.nbs/chat/test.chat"
LOG="$TEST_DIR/.nbs/scribe/test-log.md"
touch "$CHAT"  # chat file just needs to exist for derivation

cat > "$LOG" << 'LOGEOF'
# Decision Log

Created: 2026-02-27
Project: test

---

### D-1000000001 Use recursive descent parser
- **Chat ref:** live.chat:~L5
- **Participants:** alex,claude
- **Artefacts:** src/parser.c
- **Risk tags:** none
- **Status:** decided
- **Rationale:** Grammar is LL(1).

---

### D-1000000002 [SUPERSEDES D-1000000001] Switch to Pratt parser
- **Chat ref:** live.chat:~L42
- **Participants:** alex,theologian
- **Artefacts:** src/parser.c
- **Risk tags:** breaking-change
- **Status:** decided
- **Rationale:** Grammar has operator precedence. Recursive descent cannot handle it.

---

### D-1000000003 Use file-based events
- **Chat ref:** live.chat:~L80
- **Participants:** alex,claude,generalist
- **Artefacts:** src/bus.c
- **Risk tags:** perf-risk
- **Status:** decided
- **Rationale:** Crash recovery is free with files. Sockets need a daemon.

---
LOGEOF

echo "=== nbs-scribe-query tests ==="

# Bug 1: --help without log file
next_test "1. --help without log file exits 0"
RC=0
"$QUERY" --help >/dev/null 2>&1 || RC=$?
check "Exit code is 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"

next_test "2. -h without log file exits 0"
RC=0
"$QUERY" -h >/dev/null 2>&1 || RC=$?
check "Exit code is 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"

next_test "3. --help with log file also works"
RC=0
"$QUERY" --chat="$CHAT" --help >/dev/null 2>&1 || RC=$?
check "Exit code is 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"

# Count
next_test "4. --count returns 3"
COUNT=$("$QUERY" --chat="$CHAT" --count)
check "Count is 3" "$( [[ "$COUNT" -eq 3 ]] && echo pass || echo fail )"

# --last
next_test "5. --last=1 shows only the last decision"
OUTPUT=$("$QUERY" --chat="$CHAT" --last=1)
check "Contains file-based" "$( echo "$OUTPUT" | grep -qF 'file-based events' && echo pass || echo fail )"
check "Does not contain recursive" "$( echo "$OUTPUT" | grep -qF 'recursive descent' && echo fail || echo pass )"

next_test "6. --last=2 shows two decisions"
OUTPUT=$("$QUERY" --chat="$CHAT" --last=2)
check "Contains Pratt" "$( echo "$OUTPUT" | grep -qF 'Pratt parser' && echo pass || echo fail )"
check "Contains file-based" "$( echo "$OUTPUT" | grep -qF 'file-based events' && echo pass || echo fail )"

# --id
next_test "7. --id lookup"
OUTPUT=$("$QUERY" --chat="$CHAT" --id=D-1000000002)
check "Found Pratt decision" "$( echo "$OUTPUT" | grep -qF 'Pratt parser' && echo pass || echo fail )"
check "Shows rationale" "$( echo "$OUTPUT" | grep -qF 'operator precedence' && echo pass || echo fail )"

next_test "8. --id not found exits 1"
RC=0
"$QUERY" --chat="$CHAT" --id=D-9999999999 2>/dev/null || RC=$?
check "Exit code is 1" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"

# --by
next_test "9. --by=theologian"
OUTPUT=$("$QUERY" --chat="$CHAT" --by=theologian 2>/dev/null) || true
check "Contains theologian's decision" "$( echo "$OUTPUT" | grep -qF 'Pratt parser' && echo pass || echo fail )"

# --tag
next_test "10. --tag=perf-risk"
OUTPUT=$("$QUERY" --chat="$CHAT" --tag=perf-risk 2>/dev/null) || true
check "Contains perf-risk decision" "$( echo "$OUTPUT" | grep -qF 'file-based events' && echo pass || echo fail )"

# --superseded
next_test "11. --superseded shows corrections"
OUTPUT=$("$QUERY" --chat="$CHAT" --superseded 2>/dev/null) || true
check "Contains SUPERSEDES" "$( echo "$OUTPUT" | grep -qF 'SUPERSEDES D-1000000001' && echo pass || echo fail )"

# Free-text literal search
next_test "12. Literal search for 'LL(1)'"
OUTPUT=$("$QUERY" --chat="$CHAT" "LL(1)" 2>/dev/null) || true
check "Found LL(1) match" "$( echo "$OUTPUT" | grep -qF 'recursive descent' && echo pass || echo fail )"

next_test "13. Literal search — dot not treated as regex"
OUTPUT=$("$QUERY" --chat="$CHAT" "bus.c" 2>/dev/null) || true
check "Found bus.c" "$( echo "$OUTPUT" | grep -qF 'file-based events' && echo pass || echo fail )"

# Bug 2: preamble match doesn't crash
next_test "14. Search term in preamble doesn't crash"
OUTPUT=$("$QUERY" --chat="$CHAT" "Decision Log" 2>/dev/null) || true
RC=$?
check "Exits without error" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"

# Regex mode
next_test "15. --regex search"
OUTPUT=$("$QUERY" --chat="$CHAT" 'Pratt.*parser' --regex 2>/dev/null) || true
check "Regex found match" "$( echo "$OUTPUT" | grep -qF 'Pratt parser' && echo pass || echo fail )"

# No args
next_test "16. No arguments exits 4"
RC=0
"$QUERY" 2>/dev/null || RC=$?
check "Exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

# Missing log file (chat exists but derived log doesn't)
next_test "17. Missing scribe log exits 4"
RC=0
FAKE_CHAT="$TEST_DIR/.nbs/chat/nonexistent.chat"
touch "$FAKE_CHAT"
"$QUERY" --chat="$FAKE_CHAT" --count 2>/dev/null || RC=$?
check "Exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"
rm -f "$FAKE_CHAT"

# Unknown option
next_test "18. Unknown option exits 4"
RC=0
"$QUERY" --chat="$CHAT" --bogus 2>/dev/null || RC=$?
check "Exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

echo ""
echo "=== Result: $PASS passed, $FAIL failed ==="
if [[ $FAIL -eq 0 ]]; then
    echo "PASS: All tests passed"
    exit 0
else
    echo "FAIL: $FAIL test(s) failed"
    exit 1
fi
