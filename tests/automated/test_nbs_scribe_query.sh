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

# Bug 2: preamble match doesn't crash — exits 1 (no decision matches)
next_test "14. Search term in preamble: no decision match, exits 1"
RC=0
"$QUERY" --chat="$CHAT" "Decision Log" 2>/dev/null || RC=$?
check "Exits with 1 (no decision matches)" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"

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

# Archive chat derivation
next_test "19. Archive chat file derives correct scribe log"
ARCHIVE_CHAT="$TEST_DIR/.nbs/chat/test-20260227-121804-archive.chat"
touch "$ARCHIVE_CHAT"
COUNT=$("$QUERY" --chat="$ARCHIVE_CHAT" --count)
check "Archive derives to same log (count=3)" "$( [[ "$COUNT" -eq 3 ]] && echo pass || echo fail )"
rm -f "$ARCHIVE_CHAT"

next_test "20. Second archive generation also derives correctly"
ARCHIVE_CHAT2="$TEST_DIR/.nbs/chat/test-20260226-095058-archive.chat"
touch "$ARCHIVE_CHAT2"
COUNT=$("$QUERY" --chat="$ARCHIVE_CHAT2" --count)
check "Older archive same log (count=3)" "$( [[ "$COUNT" -eq 3 ]] && echo pass || echo fail )"
rm -f "$ARCHIVE_CHAT2"

next_test "21. Archive chat queries return same results as main chat"
ARCHIVE_CHAT3="$TEST_DIR/.nbs/chat/test-20260227-143000-archive.chat"
touch "$ARCHIVE_CHAT3"
OUTPUT=$("$QUERY" --chat="$ARCHIVE_CHAT3" --id=D-1000000002)
check "Archive --id finds Pratt" "$( echo "$OUTPUT" | grep -qF 'Pratt parser' && echo pass || echo fail )"
rm -f "$ARCHIVE_CHAT3"

next_test "22. Non-archive hyphenated chat name not stripped"
# A chat named "my-project.chat" should derive "my-project-log.md", not "my-log.md"
mkdir -p "$TEST_DIR/.nbs/scribe"
HYPH_CHAT="$TEST_DIR/.nbs/chat/my-project.chat"
HYPH_LOG="$TEST_DIR/.nbs/scribe/my-project-log.md"
touch "$HYPH_CHAT"
cp "$LOG" "$HYPH_LOG"
COUNT=$("$QUERY" --chat="$HYPH_CHAT" --count)
check "Hyphenated name preserved (count=3)" "$( [[ "$COUNT" -eq 3 ]] && echo pass || echo fail )"
rm -f "$HYPH_CHAT" "$HYPH_LOG"

next_test "23. Path derivation: chat dir → scribe dir"
# Verify the ../scribe relative path works
OUTPUT=$("$QUERY" --chat="$CHAT" --last=1)
check "Derivation found log" "$( echo "$OUTPUT" | grep -qF 'file-based events' && echo pass || echo fail )"

# =====================================================================
# Violation fix tests: BUG, SECURITY, HARDENING
# =====================================================================

# --- BUG #1: --last=N must be validated as positive integer ---
next_test "24. --last=0 exits 4 (not positive)"
RC=0
"$QUERY" --chat="$CHAT" --last=0 2>/dev/null || RC=$?
check "Exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

next_test "25. --last=abc exits 4 (not integer)"
RC=0
"$QUERY" --chat="$CHAT" --last=abc 2>/dev/null || RC=$?
check "Exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

next_test "26. --last=-1 exits 4 (negative)"
RC=0
"$QUERY" --chat="$CHAT" --last=-1 2>/dev/null || RC=$?
check "Exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

next_test "27. --last= (empty) exits 4"
RC=0
"$QUERY" --chat="$CHAT" "--last=" 2>/dev/null || RC=$?
check "Exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

# --- BUG #5: empty search pattern must be rejected ---
next_test "28. Empty search pattern exits 4"
RC=0
"$QUERY" --chat="$CHAT" "" 2>/dev/null || RC=$?
check "Exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

# --- BUG #6: pattern starting with dash must not be interpreted as grep flag ---
next_test "29. Pattern starting with dash is caught as unknown option"
RC=0
"$QUERY" --chat="$CHAT" "-something" 2>/dev/null || RC=$?
# Starts with -, so case matches -* → unknown option → exit 4
check "Exit code is 4 (unknown option)" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

# --- BUG #8: pattern search exits 1 when no matches ---
next_test "30. Pattern search with no matches exits 1"
RC=0
"$QUERY" --chat="$CHAT" "xyzzy_nonexistent_string" 2>/dev/null || RC=$?
check "Exit code is 1" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"

next_test "31. Pattern search with match exits 0"
RC=0
"$QUERY" --chat="$CHAT" "Pratt" >/dev/null 2>&1 || RC=$?
check "Exit code is 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"

# --- BUG #9: --by exits 1 when no matches ---
next_test "32. --by with unknown handle exits 1"
RC=0
"$QUERY" --chat="$CHAT" --by=nonexistent_person 2>/dev/null || RC=$?
check "Exit code is 1" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"

# --- BUG #10: --tag exits 1 when no matches ---
next_test "33. --tag with unknown tag exits 1"
RC=0
"$QUERY" --chat="$CHAT" --tag=nonexistent_tag 2>/dev/null || RC=$?
check "Exit code is 1" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"

# --- BUG #11: --superseded exits 1 when no superseded entries ---
next_test "34. --superseded exits 1 on log with no superseded entries"
# Create a log with no SUPERSEDES entries
NO_SUPER_LOG="$TEST_DIR/.nbs/scribe/nosup-log.md"
NO_SUPER_CHAT="$TEST_DIR/.nbs/chat/nosup.chat"
touch "$NO_SUPER_CHAT"
cat > "$NO_SUPER_LOG" << 'EOF2'
# Decision Log

---

### D-1000000001 A simple decision
- **Status:** decided

---
EOF2
RC=0
"$QUERY" --chat="$NO_SUPER_CHAT" --superseded 2>/dev/null || RC=$?
check "Exit code is 1" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"
rm -f "$NO_SUPER_LOG" "$NO_SUPER_CHAT"

# --- SECURITY #12: path canonicalisation ---
next_test "35. Scribe dir is canonicalised (no path traversal)"
# The scribe directory must exist as a real directory for the script to work.
# If it doesn't exist, the script should fail with exit 4.
RC=0
NOWHERE_CHAT="$TEST_DIR/nonexistent_dir/chat/test.chat"
mkdir -p "$(dirname "$NOWHERE_CHAT")"
touch "$NOWHERE_CHAT"
"$QUERY" --chat="$NOWHERE_CHAT" --count 2>/dev/null || RC=$?
check "Exit code is 4 (scribe dir not found)" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"
rm -rf "$TEST_DIR/nonexistent_dir"

# --- HARDENING #2: --id= format validation ---
next_test "36. --id= with empty value exits 4"
RC=0
"$QUERY" --chat="$CHAT" "--id=" 2>/dev/null || RC=$?
check "Exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

next_test "37. --id= with regex metacharacters exits 4"
RC=0
"$QUERY" --chat="$CHAT" "--id=.*" 2>/dev/null || RC=$?
check "Exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

next_test "38. --id= with invalid format exits 4"
RC=0
"$QUERY" --chat="$CHAT" "--id=notvalid" 2>/dev/null || RC=$?
check "Exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

# --- HARDENING #3: --by= empty handle validation ---
next_test "39. --by= with empty value exits 4"
RC=0
"$QUERY" --chat="$CHAT" "--by=" 2>/dev/null || RC=$?
check "Exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

# --- HARDENING #4: --tag= empty tag validation ---
next_test "40. --tag= with empty value exits 4"
RC=0
"$QUERY" --chat="$CHAT" "--tag=" 2>/dev/null || RC=$?
check "Exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

# --- HARDENING #13: --chat= with no subcommand exits 4, not silent help ---
next_test "41. --chat= with no subcommand exits 4"
RC=0
"$QUERY" --chat="$CHAT" 2>/dev/null || RC=$?
check "Exit code is 4 (not silent help)" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

next_test "42. --chat= with --regex but no pattern exits 4"
RC=0
"$QUERY" --chat="$CHAT" --regex 2>/dev/null || RC=$?
check "Exit code is 4" "$( [[ $RC -eq 4 ]] && echo pass || echo fail )"

# --- Verify existing functionality still works with fixes ---
next_test "43. --by=alex still finds decisions (regression)"
OUTPUT=$("$QUERY" --chat="$CHAT" --by=alex 2>/dev/null) || true
check "Found alex decisions" "$( echo "$OUTPUT" | grep -qF 'recursive descent' && echo pass || echo fail )"

next_test "44. --tag=breaking-change still finds decisions (regression)"
OUTPUT=$("$QUERY" --chat="$CHAT" --tag=breaking-change 2>/dev/null) || true
check "Found breaking-change decision" "$( echo "$OUTPUT" | grep -qF 'Pratt parser' && echo pass || echo fail )"

next_test "45. --superseded still finds entries (regression)"
RC=0
OUTPUT=$("$QUERY" --chat="$CHAT" --superseded 2>/dev/null) || RC=$?
check "Exit code is 0" "$( [[ $RC -eq 0 ]] && echo pass || echo fail )"
check "Contains SUPERSEDES" "$( echo "$OUTPUT" | grep -qF 'SUPERSEDES' && echo pass || echo fail )"

next_test "46. Regex search with no match exits 1"
RC=0
"$QUERY" --chat="$CHAT" 'zzzzz_nomatch' --regex 2>/dev/null || RC=$?
check "Exit code is 1" "$( [[ $RC -eq 1 ]] && echo pass || echo fail )"

echo ""
echo "=== Result: $PASS passed, $FAIL failed ==="
if [[ $FAIL -eq 0 ]]; then
    echo "PASS: All tests passed"
    exit 0
else
    echo "FAIL: $FAIL test(s) failed"
    exit 1
fi
