#!/bin/bash
# Test: nbs-scribe-log deterministic test suite
#
# 28 tests across 5 categories:
#   Happy path (1-6):    Core functionality, defaults, ordering
#   Options (7-12):      All optional flags, individually and combined
#   Adversarial (13-24): Bad args, permissions, injection, concurrency
#   Initialisation (25-26): File creation and empty-file handling
#   Help (27-28):        --help and -h
#
# Exit codes:
#   0 - All tests passed
#   1 - One or more tests failed

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(dirname "$(dirname "$SCRIPT_DIR")")"
NBS_SCRIBE="${NBS_SCRIBE_BIN:-$PROJECT_ROOT/bin/nbs-scribe-log}"

# nbs-bus must be on PATH for bus event publishing
export PATH="$PROJECT_ROOT/bin:$PATH"

TEST_DIR=$(mktemp -d)
PASS=0
FAIL=0

cleanup() {
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

check() {
    local label="$1"
    local result="$2"
    if [[ "$result" == "pass" ]]; then
        echo "   PASS: $label"
        PASS=$((PASS + 1))
    else
        echo "   FAIL: $label"
        FAIL=$((FAIL + 1))
    fi
}

TEST_NUM=0
next_test() {
    TEST_NUM=$((TEST_NUM + 1))
    echo ""
    echo "$TEST_NUM. $1"
}

echo "=== nbs-scribe-log Test Suite (28 tests) ==="
echo "Test dir: $TEST_DIR"
echo "Binary:   $NBS_SCRIBE"

# Precondition: binary exists and is executable
if [[ ! -x "$NBS_SCRIBE" ]]; then
    echo "FATAL: $NBS_SCRIBE not found or not executable"
    exit 1
fi

# =====================================================================
# Happy Path (1-6)
# =====================================================================

next_test "Append decision, verify D-<timestamp> format in heading"
LOG="$TEST_DIR/hp1.md"
set +e
STDOUT=$("$NBS_SCRIBE" "$LOG" "Use recursive descent parser" \
    --participants=alex,claude \
    --rationale="Grammar is LL(1), Pratt adds unnecessary complexity." \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null)
RC=$?
set -e
check "Exit code is 0" "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"
# Verify heading matches ### D-<digits> <summary>
check "Log contains ### D-<timestamp> heading" \
    "$( grep -qE '^### D-[0-9]+ Use recursive descent parser$' "$LOG" && echo pass || echo fail )"

next_test "Verify all 5 fields present"
check "Chat ref field present" \
    "$( grep -qF -- '- **Chat ref:**' "$LOG" && echo pass || echo fail )"
check "Participants field present" \
    "$( grep -qF -- '- **Participants:**' "$LOG" && echo pass || echo fail )"
check "Artefacts field present" \
    "$( grep -qF -- '- **Artefacts:**' "$LOG" && echo pass || echo fail )"
check "Risk tags field present" \
    "$( grep -qF -- '- **Risk tags:**' "$LOG" && echo pass || echo fail )"
check "Rationale field present" \
    "$( grep -qF -- '- **Rationale:**' "$LOG" && echo pass || echo fail )"

next_test "Verify bus event published"
BUS_DIR="$TEST_DIR/bus3"
mkdir -p "$BUS_DIR"
LOG="$TEST_DIR/hp3.md"
"$NBS_SCRIBE" "$LOG" "Bus event test" \
    --participants=alex \
    --rationale="Verifying bus publish" \
    --bus-dir="$BUS_DIR" 2>/dev/null
# Look for a decision-logged event file
EVENT_COUNT=$(find "$BUS_DIR" -name '*decision-logged*' -type f | wc -l)
check "Bus event file exists with decision-logged in name" \
    "$( [[ "$EVENT_COUNT" -ge 1 ]] && echo pass || echo fail )"

next_test "Verify decision ID printed to stdout"
LOG="$TEST_DIR/hp4.md"
set +e
STDOUT=$("$NBS_SCRIBE" "$LOG" "Stdout ID test" \
    --participants=alex \
    --rationale="Check stdout" \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null)
set -e
# stdout should be D-<digits> with optional trailing newline
check "Stdout matches D-<digits>" \
    "$( echo "$STDOUT" | grep -qE '^D-[0-9]+$' && echo pass || echo fail )"

next_test "Append two decisions, verify both present and ordered"
LOG="$TEST_DIR/hp5.md"
set +e
ID1=$("$NBS_SCRIBE" "$LOG" "First decision" \
    --participants=alex --rationale="Reason one" \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null)
sleep 1
ID2=$("$NBS_SCRIBE" "$LOG" "Second decision" \
    --participants=alex --rationale="Reason two" \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null)
set -e
check "First decision present in log" \
    "$( grep -qF 'First decision' "$LOG" && echo pass || echo fail )"
check "Second decision present in log" \
    "$( grep -qF 'Second decision' "$LOG" && echo pass || echo fail )"
# Verify ordering: first appears before second (by line number)
LINE1=$(grep -nF 'First decision' "$LOG" | head -1 | cut -d: -f1)
LINE2=$(grep -nF 'Second decision' "$LOG" | head -1 | cut -d: -f1)
check "First decision appears before second" \
    "$( [[ "$LINE1" -lt "$LINE2" ]] && echo pass || echo fail )"
# Verify IDs differ
check "Two different decision IDs" \
    "$( [[ "$ID1" != "$ID2" ]] && echo pass || echo fail )"

next_test "Verify status=decided is default"
LOG="$TEST_DIR/hp6.md"
"$NBS_SCRIBE" "$LOG" "Default status test" \
    --participants=alex --rationale="Check default" \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null
check "Status line shows 'decided'" \
    "$( grep -qF -- '- **Status:** decided' "$LOG" && echo pass || echo fail )"

# =====================================================================
# Options (7-12)
# =====================================================================

next_test "--status=superseded"
LOG="$TEST_DIR/opt7.md"
"$NBS_SCRIBE" "$LOG" "Superseded test" \
    --participants=alex --rationale="Testing status" \
    --status=superseded \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null
check "Status line shows 'superseded'" \
    "$( grep -qF -- '- **Status:** superseded' "$LOG" && echo pass || echo fail )"

next_test "--supersedes=D-1234567890 adds [SUPERSEDES ...] in heading"
LOG="$TEST_DIR/opt8.md"
"$NBS_SCRIBE" "$LOG" "Replacement plan" \
    --participants=alex --rationale="Better approach" \
    --supersedes=D-1234567890 \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null
check "Heading contains [SUPERSEDES D-1234567890]" \
    "$( grep -qF '[SUPERSEDES D-1234567890]' "$LOG" && echo pass || echo fail )"
check "Summary still present after SUPERSEDES tag" \
    "$( grep -qF 'Replacement plan' "$LOG" && echo pass || echo fail )"

next_test "--risk-tags=scope-creep,tech-debt"
LOG="$TEST_DIR/opt9.md"
"$NBS_SCRIBE" "$LOG" "Risk tags test" \
    --participants=alex --rationale="Risky" \
    --risk-tags=scope-creep,tech-debt \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null
check "Risk tags line shows 'scope-creep,tech-debt'" \
    "$( grep -qF -- '- **Risk tags:** scope-creep,tech-debt' "$LOG" && echo pass || echo fail )"

next_test "--artefacts=commit-abc123"
LOG="$TEST_DIR/opt10.md"
"$NBS_SCRIBE" "$LOG" "Artefacts test" \
    --participants=alex --rationale="Has artefact" \
    --artefacts=commit-abc123 \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null
check "Artefacts line shows 'commit-abc123'" \
    "$( grep -qF -- '- **Artefacts:** commit-abc123' "$LOG" && echo pass || echo fail )"

next_test "--bus-dir=<custom temp dir>"
BUS_DIR="$TEST_DIR/custom-bus-11"
mkdir -p "$BUS_DIR"
LOG="$TEST_DIR/opt11.md"
"$NBS_SCRIBE" "$LOG" "Custom bus dir" \
    --participants=alex --rationale="Custom bus" \
    --bus-dir="$BUS_DIR" 2>/dev/null
CUSTOM_EVENT=$(find "$BUS_DIR" -name '*decision-logged*' -type f | wc -l)
check "Event published to custom bus dir" \
    "$( [[ "$CUSTOM_EVENT" -ge 1 ]] && echo pass || echo fail )"

next_test "All options combined"
BUS_DIR="$TEST_DIR/all-opts-bus"
mkdir -p "$BUS_DIR"
LOG="$TEST_DIR/opt12.md"
set +e
STDOUT=$("$NBS_SCRIBE" "$LOG" "All options combined" \
    --participants=alex,claude,bob \
    --rationale="Comprehensive option test" \
    --chat-ref=live.chat:~L42 \
    --artefacts=commit-abc123,file.py \
    --risk-tags=scope-creep,tech-debt \
    --status=superseded \
    --supersedes=D-9999999999 \
    --bus-dir="$BUS_DIR" 2>/dev/null)
RC=$?
set -e
check "All-options exits 0" "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"
check "Chat ref correct" \
    "$( grep -qF -- '- **Chat ref:** live.chat:~L42' "$LOG" && echo pass || echo fail )"
check "Participants correct" \
    "$( grep -qF -- '- **Participants:** alex,claude,bob' "$LOG" && echo pass || echo fail )"
check "Artefacts correct" \
    "$( grep -qF -- '- **Artefacts:** commit-abc123,file.py' "$LOG" && echo pass || echo fail )"
check "Risk tags correct" \
    "$( grep -qF -- '- **Risk tags:** scope-creep,tech-debt' "$LOG" && echo pass || echo fail )"
check "Status correct" \
    "$( grep -qF -- '- **Status:** superseded' "$LOG" && echo pass || echo fail )"
check "SUPERSEDES in heading" \
    "$( grep -qF '[SUPERSEDES D-9999999999]' "$LOG" && echo pass || echo fail )"
check "Bus event published" \
    "$( [[ $(find "$BUS_DIR" -name '*decision-logged*' -type f | wc -l) -ge 1 ]] && echo pass || echo fail )"

# =====================================================================
# Adversarial (13-24)
# =====================================================================

next_test "No arguments exits 4"
set +e
"$NBS_SCRIBE" >/dev/null 2>&1
RC=$?
set -e
check "No-args exit code is 4" "$( [[ "$RC" -eq 4 ]] && echo pass || echo fail )"

next_test "Only log file, no summary exits 4"
set +e
"$NBS_SCRIBE" "$TEST_DIR/adv14.md" >/dev/null 2>&1
RC=$?
set -e
check "One-arg exit code is 4" "$( [[ "$RC" -eq 4 ]] && echo pass || echo fail )"

next_test "Missing --participants exits 4"
set +e
"$NBS_SCRIBE" "$TEST_DIR/adv15.md" "Summary" \
    --rationale="Has rationale but no participants" >/dev/null 2>&1
RC=$?
set -e
check "Missing --participants exit code is 4" "$( [[ "$RC" -eq 4 ]] && echo pass || echo fail )"

next_test "Missing --rationale exits 4"
set +e
"$NBS_SCRIBE" "$TEST_DIR/adv16.md" "Summary" \
    --participants=alex >/dev/null 2>&1
RC=$?
set -e
check "Missing --rationale exit code is 4" "$( [[ "$RC" -eq 4 ]] && echo pass || echo fail )"

next_test "Empty summary exits 4"
set +e
"$NBS_SCRIBE" "$TEST_DIR/adv17.md" "" \
    --participants=alex --rationale="Reason" >/dev/null 2>&1
RC=$?
set -e
check "Empty summary exit code is 4" "$( [[ "$RC" -eq 4 ]] && echo pass || echo fail )"

next_test "Log file in non-writable directory exits 1"
RO_DIR="$TEST_DIR/readonly-dir"
mkdir -p "$RO_DIR"
chmod 555 "$RO_DIR"
set +e
"$NBS_SCRIBE" "$RO_DIR/test.md" "Should fail" \
    --participants=alex --rationale="Reason" \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
chmod 755 "$RO_DIR"
check "Non-writable directory exit code is 1" "$( [[ "$RC" -eq 1 ]] && echo pass || echo fail )"

next_test "Read-only log file exits 1"
LOG="$TEST_DIR/adv19.md"
touch "$LOG"
chmod 444 "$LOG"
set +e
"$NBS_SCRIBE" "$LOG" "Should fail" \
    --participants=alex --rationale="Reason" \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1
RC=$?
set -e
chmod 644 "$LOG"
check "Read-only log file exit code is 1" "$( [[ "$RC" -eq 1 ]] && echo pass || echo fail )"

next_test "Summary with shell metacharacters safely handled"
LOG="$TEST_DIR/adv20.md"
# These characters should appear literally in the log, not be interpreted
NASTY_SUMMARY='Test $(whoami) `id` & ; | > < "quotes" ${HOME}'
set +e
STDOUT=$("$NBS_SCRIBE" "$LOG" "$NASTY_SUMMARY" \
    --participants=alex --rationale="Injection test" \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null)
RC=$?
set -e
check "Metacharacter summary exits 0" "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"
# Verify the literal text appears, not expanded values
check "Dollar-whoami not expanded" \
    "$( grep -qF '$(whoami)' "$LOG" && echo pass || echo fail )"
check "Backtick-id not expanded" \
    "$( grep -qF '`id`' "$LOG" && echo pass || echo fail )"

next_test "Participants with special chars safely handled"
LOG="$TEST_DIR/adv21.md"
set +e
STDOUT=$("$NBS_SCRIBE" "$LOG" "Participant test" \
    --participants='alice;bob|charlie&dave' \
    --rationale="Special participant names" \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null)
RC=$?
set -e
check "Special-char participants exits 0" "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"
check "Participants appear literally in log" \
    "$( grep -qF 'alice;bob|charlie&dave' "$LOG" && echo pass || echo fail )"

next_test "Concurrent appends both succeed, no corruption"
LOG="$TEST_DIR/adv22.md"
# Launch two instances in background
"$NBS_SCRIBE" "$LOG" "Concurrent-A" \
    --participants=alex --rationale="First concurrent" \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1 &
PID1=$!
"$NBS_SCRIBE" "$LOG" "Concurrent-B" \
    --participants=bob --rationale="Second concurrent" \
    --bus-dir="$TEST_DIR/nobus" >/dev/null 2>&1 &
PID2=$!
set +e
wait "$PID1"
RC1=$?
wait "$PID2"
RC2=$?
set -e
check "First concurrent instance exits 0" "$( [[ "$RC1" -eq 0 ]] && echo pass || echo fail )"
check "Second concurrent instance exits 0" "$( [[ "$RC2" -eq 0 ]] && echo pass || echo fail )"
check "Concurrent-A present in log" \
    "$( grep -qF 'Concurrent-A' "$LOG" && echo pass || echo fail )"
check "Concurrent-B present in log" \
    "$( grep -qF 'Concurrent-B' "$LOG" && echo pass || echo fail )"
# Count heading lines — should be exactly 2
HEADING_COUNT=$(grep -c '^### D-' "$LOG")
check "Exactly 2 decision headings (no corruption)" \
    "$( [[ "$HEADING_COUNT" -eq 2 ]] && echo pass || echo fail )"

next_test "Bus directory missing — decision still logged, warning on stderr"
LOG="$TEST_DIR/adv23.md"
set +e
STDERR=$("$NBS_SCRIBE" "$LOG" "Missing bus dir" \
    --participants=alex --rationale="Bus dir absent" \
    --bus-dir="$TEST_DIR/no-such-bus-dir" 2>&1 1>/dev/null)
RC=$?
set -e
check "Decision logged despite missing bus dir (exit 0)" \
    "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"
check "Log file contains the decision" \
    "$( grep -qF 'Missing bus dir' "$LOG" && echo pass || echo fail )"
check "Warning on stderr about bus directory" \
    "$( echo "$STDERR" | grep -qi 'warning.*bus' && echo pass || echo fail )"

next_test "Unknown option exits 4"
set +e
"$NBS_SCRIBE" "$TEST_DIR/adv24.md" "Summary" \
    --participants=alex --rationale="Reason" \
    --bogus-option=value >/dev/null 2>&1
RC=$?
set -e
check "Unknown option exit code is 4" "$( [[ "$RC" -eq 4 ]] && echo pass || echo fail )"

# =====================================================================
# Initialisation (25-26)
# =====================================================================

next_test "Log file does not exist — created with header"
LOG="$TEST_DIR/init-subdir/new-log.md"
# Subdirectory does not exist either; tool should create it
"$NBS_SCRIBE" "$LOG" "Init test" \
    --participants=alex --rationale="Auto-creation" \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null
check "Log file created" "$( [[ -f "$LOG" ]] && echo pass || echo fail )"
check "Log contains '# Decision Log' header" \
    "$( grep -qF '# Decision Log' "$LOG" && echo pass || echo fail )"
check "Log contains the appended decision" \
    "$( grep -qF 'Init test' "$LOG" && echo pass || echo fail )"

next_test "Log file exists but empty — handled gracefully"
LOG="$TEST_DIR/init26.md"
touch "$LOG"
# File exists but is zero bytes
set +e
STDOUT=$("$NBS_SCRIBE" "$LOG" "Into empty file" \
    --participants=alex --rationale="Empty file test" \
    --bus-dir="$TEST_DIR/nobus" 2>/dev/null)
RC=$?
set -e
check "Append to empty file exits 0" "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"
check "Decision present in previously-empty file" \
    "$( grep -qF 'Into empty file' "$LOG" && echo pass || echo fail )"
# File should have valid content (no crash, no garbled output)
check "File has a D-<digits> heading" \
    "$( grep -qE '^### D-[0-9]+' "$LOG" && echo pass || echo fail )"

# =====================================================================
# Help (27-28)
# =====================================================================

next_test "--help exits 0 with usage text"
set +e
HELP_OUT=$("$NBS_SCRIBE" --help 2>&1)
RC=$?
set -e
check "--help exit code is 0" "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"
check "--help output contains 'Usage:'" \
    "$( echo "$HELP_OUT" | grep -qF 'Usage:' && echo pass || echo fail )"

next_test "-h exits 0"
set +e
"$NBS_SCRIBE" -h >/dev/null 2>&1
RC=$?
set -e
check "-h exit code is 0" "$( [[ "$RC" -eq 0 ]] && echo pass || echo fail )"

# =====================================================================
# Summary
# =====================================================================

echo ""
echo "=== Result: $PASS passed, $FAIL failed ==="
if [[ $FAIL -eq 0 ]]; then
    echo "PASS: All tests passed"
    exit 0
else
    echo "FAIL: $FAIL test(s) failed"
    exit 1
fi
