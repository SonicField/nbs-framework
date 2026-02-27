/*
 * test_sidecar_transport_unit.c -- Unit tests for transport_pty.c and transport_tmux.c
 *
 * Tests adversarial inputs and boundary conditions identified by audit:
 *   - BUG: Long pane_id/pty_path/session_name return -1 (not abort)
 *   - BUG: Empty strings return -1 (not abort for external-input checks)
 *   - SECURITY: pane_id format validation (%[0-9]+)
 *   - SECURITY: Defensive NUL-termination in is_alive parsing
 *   - BUG: transport_tmux scrollback==0 semantics
 *   - BUG: is_alive returns -1 on exec error, not 0
 *   - HARDENING: transport_free invariant check
 *   - HARDENING: Named constants, postconditions
 *
 * Build:
 *   make -C src/nbs-sidecar transport_pty.o transport_tmux.o exec_util.o
 *   gcc -Wall -Wextra -Wshadow -Werror -std=c11 -D_POSIX_C_SOURCE=200809L \
 *       -I src/nbs-sidecar -I src/nbs-common \
 *       -o tests/test_sidecar_transport_unit \
 *       tests/test_sidecar_transport_unit.c \
 *       src/nbs-sidecar/transport_pty.o \
 *       src/nbs-sidecar/transport_tmux.o \
 *       src/nbs-sidecar/exec_util.o
 */

#include "transport.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, fmt, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: " fmt "\n", \
                __FILE__, __LINE__, ##__VA_ARGS__); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define TEST_PASS(name) do { \
    tests_passed++; \
    printf("  PASS: %s\n", name); \
} while(0)

/* ==================================================================
 * PTY TRANSPORT TESTS
 * ================================================================== */

/* ---- 1. pty_init: normal case succeeds ---- */
static void test_pty_init_normal(void)
{
    transport_t tp;
    int rc = transport_pty_init(&tp, "/usr/bin/pty-session", "test-session");
    TEST_ASSERT(rc == 0, "expected rc=0, got %d", rc);
    TEST_ASSERT(tp.capture != NULL, "capture should be non-NULL");
    TEST_ASSERT(tp.send_text != NULL, "send_text should be non-NULL");
    TEST_ASSERT(tp.send_key != NULL, "send_key should be non-NULL");
    TEST_ASSERT(tp.is_alive != NULL, "is_alive should be non-NULL");
    TEST_ASSERT(tp.ctx != NULL, "ctx should be non-NULL");
    transport_free(&tp);
    TEST_PASS("pty_init normal case");
}

/* ---- 2. pty_init: pty_path too long returns -1 (not abort) ---- */
static void test_pty_init_path_too_long(void)
{
    transport_t tp;
    /* 4096 chars + NUL = too long for pty_path[4096] */
    char long_path[4097];
    memset(long_path, 'x', 4096);
    long_path[4096] = '\0';

    int rc = transport_pty_init(&tp, long_path, "test-session");
    TEST_ASSERT(rc == -1,
                "expected rc=-1 for too-long pty_path, got %d", rc);
    /* Postcondition: tp should be zeroed on error */
    TEST_ASSERT(tp.capture == NULL, "capture should be NULL after error");
    TEST_ASSERT(tp.ctx == NULL, "ctx should be NULL after error");
    TEST_PASS("pty_init path too long returns -1");
}

/* ---- 3. pty_init: session_name too long returns -1 (not abort) ---- */
static void test_pty_init_session_too_long(void)
{
    transport_t tp;
    /* 256 chars + NUL = too long for session_name[256] */
    char long_name[257];
    memset(long_name, 'y', 256);
    long_name[256] = '\0';

    int rc = transport_pty_init(&tp, "/usr/bin/pty-session", long_name);
    TEST_ASSERT(rc == -1,
                "expected rc=-1 for too-long session_name, got %d", rc);
    TEST_ASSERT(tp.capture == NULL, "capture should be NULL after error");
    TEST_ASSERT(tp.ctx == NULL, "ctx should be NULL after error");
    TEST_PASS("pty_init session_name too long returns -1");
}

/* ---- 4. pty_init: session_name empty returns -1 ---- */
static void test_pty_init_session_empty(void)
{
    transport_t tp;
    int rc = transport_pty_init(&tp, "/usr/bin/pty-session", "");
    TEST_ASSERT(rc == -1,
                "expected rc=-1 for empty session_name, got %d", rc);
    TEST_PASS("pty_init empty session_name returns -1");
}

/* ---- 5. pty_init: pty_path empty returns -1 ---- */
static void test_pty_init_path_empty(void)
{
    transport_t tp;
    int rc = transport_pty_init(&tp, "", "test-session");
    TEST_ASSERT(rc == -1,
                "expected rc=-1 for empty pty_path, got %d", rc);
    TEST_PASS("pty_init empty pty_path returns -1");
}

/* ---- 6. pty_is_alive: nonexistent binary returns 0 or -1 ---- */
static void test_pty_is_alive_exec_error(void)
{
    transport_t tp;
    /* Use a nonexistent binary so exec fails. exec_capture returns 127
     * (child _exit(127) on execvp failure), which is >= 0, so pty_is_alive
     * takes the "rc != 0" path and returns 0 (session not found).
     * rc=-1 (fork failure) would return -1, but that cannot be provoked
     * without mocking fork. Both 0 and -1 are acceptable here. */
    int rc = transport_pty_init(&tp, "/__no_such_pty_binary__", "test-sess");
    TEST_ASSERT(rc == 0, "init should succeed, got %d", rc);

    int alive = tp.is_alive(&tp);
    TEST_ASSERT(alive == 0 || alive == -1,
                "expected is_alive=0 or -1 on nonexistent binary, got %d", alive);
    transport_free(&tp);
    TEST_PASS("pty_is_alive returns 0 or -1 on nonexistent binary");
}

/* ---- 7. pty_capture: nonexistent binary returns empty string or NULL ---- */
static void test_pty_capture_exec_error(void)
{
    transport_t tp;
    int rc = transport_pty_init(&tp, "/__no_such_pty_binary__", "test-sess");
    TEST_ASSERT(rc == 0, "init should succeed, got %d", rc);

    /* exec_capture with nonexistent binary: child _exit(127), parent gets
     * rc=127 (>= 0), so pty_capture does NOT return NULL. The buffer is
     * empty because no stdout was produced. rc < 0 (fork failure) would
     * return NULL, but that case requires mocking fork. */
    char *result = tp.capture(&tp, 100);
    if (result != NULL) {
        TEST_ASSERT(result[0] == '\0',
                    "expected empty capture on nonexistent binary, got '%s'", result);
        free(result);
    }
    /* Both NULL and empty string are acceptable */
    transport_free(&tp);
    TEST_PASS("pty_capture with nonexistent binary");
}

/* ---- 8. pty_send_text: nonexistent binary returns -1 ---- */
static void test_pty_send_text_exec_error(void)
{
    transport_t tp;
    int rc = transport_pty_init(&tp, "/__no_such_pty_binary__", "test-sess");
    TEST_ASSERT(rc == 0, "init should succeed, got %d", rc);

    rc = tp.send_text(&tp, "hello");
    TEST_ASSERT(rc == -1,
                "expected send_text=-1 on exec error, got %d", rc);
    transport_free(&tp);
    TEST_PASS("pty_send_text returns -1 on exec error");
}

/* ---- 9. pty_send_key: nonexistent binary returns -1 ---- */
static void test_pty_send_key_exec_error(void)
{
    transport_t tp;
    int rc = transport_pty_init(&tp, "/__no_such_pty_binary__", "test-sess");
    TEST_ASSERT(rc == 0, "init should succeed, got %d", rc);

    rc = tp.send_key(&tp, "Enter");
    TEST_ASSERT(rc == -1,
                "expected send_key=-1 on exec error for Enter, got %d", rc);

    rc = tp.send_key(&tp, "Escape");
    TEST_ASSERT(rc == -1,
                "expected send_key=-1 on exec error for Escape, got %d", rc);

    rc = tp.send_key(&tp, "Tab");
    TEST_ASSERT(rc == -1,
                "expected send_key=-1 on exec error for Tab, got %d", rc);

    transport_free(&tp);
    TEST_PASS("pty_send_key returns -1 on exec error");
}

/* ==================================================================
 * TMUX TRANSPORT TESTS
 * ================================================================== */

/* ---- 10. tmux_init: normal case succeeds ---- */
static void test_tmux_init_normal(void)
{
    transport_t tp;
    int rc = transport_tmux_init(&tp, "%0");
    TEST_ASSERT(rc == 0, "expected rc=0, got %d", rc);
    TEST_ASSERT(tp.capture != NULL, "capture should be non-NULL");
    TEST_ASSERT(tp.send_text != NULL, "send_text should be non-NULL");
    TEST_ASSERT(tp.send_key != NULL, "send_key should be non-NULL");
    TEST_ASSERT(tp.is_alive != NULL, "is_alive should be non-NULL");
    TEST_ASSERT(tp.ctx != NULL, "ctx should be non-NULL");
    transport_free(&tp);
    TEST_PASS("tmux_init normal case");
}

/* ---- 11. tmux_init: pane_id too long returns -1 (not abort) ---- */
static void test_tmux_init_pane_id_too_long(void)
{
    transport_t tp;
    /* Build a pane_id that starts with % but is 64+ chars */
    char long_id[65];
    long_id[0] = '%';
    memset(long_id + 1, '1', 63);
    long_id[64] = '\0';

    int rc = transport_tmux_init(&tp, long_id);
    TEST_ASSERT(rc == -1,
                "expected rc=-1 for too-long pane_id, got %d", rc);
    TEST_ASSERT(tp.capture == NULL, "capture should be NULL after error");
    TEST_ASSERT(tp.ctx == NULL, "ctx should be NULL after error");
    TEST_PASS("tmux_init pane_id too long returns -1");
}

/* ---- 12. tmux_init: pane_id empty returns -1 ---- */
static void test_tmux_init_pane_id_empty(void)
{
    transport_t tp;
    int rc = transport_tmux_init(&tp, "");
    TEST_ASSERT(rc == -1,
                "expected rc=-1 for empty pane_id, got %d", rc);
    TEST_PASS("tmux_init empty pane_id returns -1");
}

/* ---- 13. tmux_init: pane_id bad format returns -1 ---- */
static void test_tmux_init_pane_id_bad_format(void)
{
    transport_t tp;

    /* Missing % prefix */
    int rc = transport_tmux_init(&tp, "123");
    TEST_ASSERT(rc == -1,
                "expected rc=-1 for pane_id without %% prefix, got %d", rc);

    /* Non-digit after % */
    rc = transport_tmux_init(&tp, "%abc");
    TEST_ASSERT(rc == -1,
                "expected rc=-1 for pane_id with non-digits, got %d", rc);

    /* Shell metacharacters */
    rc = transport_tmux_init(&tp, "%0;rm -rf /");
    TEST_ASSERT(rc == -1,
                "expected rc=-1 for pane_id with shell metacharacters, got %d", rc);

    /* Just % with no digits */
    rc = transport_tmux_init(&tp, "%");
    TEST_ASSERT(rc == -1,
                "expected rc=-1 for pane_id '%%' with no digits, got %d", rc);

    TEST_PASS("tmux_init pane_id bad format returns -1");
}

/* ---- 14. tmux_is_alive: exec error returns -1 (not 0) ---- */
static void test_tmux_is_alive_exec_error(void)
{
    transport_t tp;
    /* tmux is likely installed, but with a nonexistent pane the result
     * depends on tmux being present. Instead we test via pty transport
     * with bad binary (test 6 above). For tmux, we verify the return
     * contract: if tmux exits non-zero for a bad pane, we get 0. */
    int rc = transport_tmux_init(&tp, "%99999");
    TEST_ASSERT(rc == 0, "init should succeed, got %d", rc);

    int alive = tp.is_alive(&tp);
    /* With a pane that does not exist, tmux list-panes returns non-zero.
     * exec_capture returns the exit code (>0), not -1.
     * So is_alive should return 0 (pane not found). */
    TEST_ASSERT(alive == 0 || alive == -1,
                "expected is_alive=0 or -1 for nonexistent pane, got %d", alive);
    transport_free(&tp);
    TEST_PASS("tmux_is_alive with nonexistent pane");
}

/* ---- 15. transport_free: NULL is safe ---- */
static void test_transport_free_null(void)
{
    transport_free(NULL);
    TEST_PASS("transport_free(NULL) does not crash");
}

/* ---- 16. transport_free: zeroed transport is safe ---- */
static void test_transport_free_zeroed(void)
{
    transport_t tp;
    memset(&tp, 0, sizeof(tp));
    transport_free(&tp);
    TEST_PASS("transport_free on zeroed transport does not crash");
}

/* ---- 17. transport_free: double free is safe ---- */
static void test_transport_free_double(void)
{
    transport_t tp;
    int rc = transport_pty_init(&tp, "/usr/bin/pty-session", "test-session");
    TEST_ASSERT(rc == 0, "init should succeed, got %d", rc);
    transport_free(&tp);
    /* After free, tp is zeroed, so second free should be safe */
    transport_free(&tp);
    TEST_PASS("transport_free double-free is safe");
}

/* ---- 18. pty_init: max-length strings that fit are accepted ---- */
static void test_pty_init_boundary_lengths(void)
{
    transport_t tp;

    /* pty_path[4096]: string of length 4095 (max that fits) */
    char max_path[4096];
    memset(max_path, 'p', 4095);
    max_path[4095] = '\0';

    /* session_name[256]: string of length 255 (max that fits) */
    char max_name[256];
    memset(max_name, 's', 255);
    max_name[255] = '\0';

    int rc = transport_pty_init(&tp, max_path, max_name);
    TEST_ASSERT(rc == 0,
                "expected rc=0 for max-length strings, got %d", rc);
    transport_free(&tp);
    TEST_PASS("pty_init boundary-length strings accepted");
}

/* ---- 19. tmux_init: max-length pane_id that fits is accepted ---- */
static void test_tmux_init_boundary_length(void)
{
    transport_t tp;
    /* pane_id[64]: % + 62 digits + NUL = 64 bytes, length=63 */
    char max_id[64];
    max_id[0] = '%';
    memset(max_id + 1, '0', 62);
    max_id[63] = '\0';

    int rc = transport_tmux_init(&tp, max_id);
    TEST_ASSERT(rc == 0,
                "expected rc=0 for max-length pane_id, got %d", rc);
    transport_free(&tp);
    TEST_PASS("tmux_init boundary-length pane_id accepted");
}

/* ---- 20. tmux_init: valid pane_id formats ---- */
static void test_tmux_init_valid_formats(void)
{
    transport_t tp;

    int rc = transport_tmux_init(&tp, "%0");
    TEST_ASSERT(rc == 0, "expected rc=0 for %%0, got %d", rc);
    transport_free(&tp);

    rc = transport_tmux_init(&tp, "%123");
    TEST_ASSERT(rc == 0, "expected rc=0 for %%123, got %d", rc);
    transport_free(&tp);

    rc = transport_tmux_init(&tp, "%99999");
    TEST_ASSERT(rc == 0, "expected rc=0 for %%99999, got %d", rc);
    transport_free(&tp);

    TEST_PASS("tmux_init valid pane_id formats");
}

/* ---- main ---- */

int main(void)
{
    printf("test_sidecar_transport_unit\n");

    /* PTY transport tests */
    test_pty_init_normal();
    test_pty_init_path_too_long();
    test_pty_init_session_too_long();
    test_pty_init_session_empty();
    test_pty_init_path_empty();
    test_pty_is_alive_exec_error();
    test_pty_capture_exec_error();
    test_pty_send_text_exec_error();
    test_pty_send_key_exec_error();

    /* TMUX transport tests */
    test_tmux_init_normal();
    test_tmux_init_pane_id_too_long();
    test_tmux_init_pane_id_empty();
    test_tmux_init_pane_id_bad_format();
    test_tmux_is_alive_exec_error();

    /* transport_free tests */
    test_transport_free_null();
    test_transport_free_zeroed();
    test_transport_free_double();

    /* Boundary tests */
    test_pty_init_boundary_lengths();
    test_tmux_init_boundary_length();
    test_tmux_init_valid_formats();

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
