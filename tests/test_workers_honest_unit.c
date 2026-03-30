/*
 * test_workers_honest_unit.c — Tests for honest-based session metadata parsing.
 *
 * Part 2 of the JSON→Honest migration. These tests verify that the honest
 * C API correctly reads SessionMeta documents as specified in
 * docs/chat-architecture/session-metadata.md.
 *
 * Tests are written BEFORE the production code changes (TDD). They exercise
 * the honest library directly against SessionMeta .honest files.
 *
 * Build (from src/nbs-workers/):
 *   gcc -Wall -Wextra -Wshadow -Werror -std=c11 \
 *       -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -O2 \
 *       -I. -I../nbs-common -I../../lib/honest/include \
 *       -o ../../tests/test_workers_honest_unit \
 *       ../../tests/test_workers_honest_unit.c \
 *       ../../lib/honest/build/libhonest.a
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <stdbool.h>

#include "honest.h"

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

/* ================================================================== */
/* Helper: write a string to a temp file, return the path              */
/* ================================================================== */

static char tmp_dir[128] = {0};

static void setup_tmp_dir(void)
{
    snprintf(tmp_dir, sizeof(tmp_dir), "/tmp/test_honest_unit_XXXXXX");
    if (mkdtemp(tmp_dir) == NULL) {
        perror("mkdtemp");
        exit(1);
    }
}

static void cleanup_tmp_dir(void)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmp_dir);
    (void)system(cmd);
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) { perror(path); exit(1); }
    fputs(content, f);
    fclose(f);
}

static void build_path(char *buf, size_t bufsz, const char *filename)
{
    int n = snprintf(buf, bufsz, "%s/%s", tmp_dir, filename);
    if (n < 0 || (size_t)n >= bufsz) {
        fprintf(stderr, "build_path: path too long\n");
        exit(1);
    }
}

/* ================================================================== */
/* Valid SessionMeta document — the canonical test fixture              */
/* ================================================================== */

static const char *VALID_SESSION_META =
    "type\n"
    "  Transport = (Auto, Ts);\n"
    "\n"
    "  SessionMeta = record\n"
    "    session_id : String;\n"
    "    handle : String;\n"
    "    model : String;\n"
    "    nbs_ts_handle : String;\n"
    "    transport : Transport;\n"
    "    started : String;\n"
    "    project_root : String;\n"
    "    pid : LongInt;\n"
    "    initial_prompt_set : Boolean;\n"
    "  end;\n"
    "\n"
    "var result : SessionMeta = (\n"
    "  session_id : 'a1b2c3d4-e5f6-7890-abcd-ef1234567890';\n"
    "  handle : 'supervisor';\n"
    "  model : 'opus[1m]';\n"
    "  nbs_ts_handle : 'nbs-supervisor-session';\n"
    "  transport : Auto;\n"
    "  started : '2026-03-30T14:52:17Z';\n"
    "  project_root : '/data/users/alex/project';\n"
    "  pid : 42;\n"
    "  initial_prompt_set : True;\n"
    ");\n";

/* Transport=Ts, initial_prompt_set=False, empty nbs_ts_handle */
static const char *VALID_SESSION_META_TS =
    "type\n"
    "  Transport = (Auto, Ts);\n"
    "\n"
    "  SessionMeta = record\n"
    "    session_id : String;\n"
    "    handle : String;\n"
    "    model : String;\n"
    "    nbs_ts_handle : String;\n"
    "    transport : Transport;\n"
    "    started : String;\n"
    "    project_root : String;\n"
    "    pid : LongInt;\n"
    "    initial_prompt_set : Boolean;\n"
    "  end;\n"
    "\n"
    "var result : SessionMeta = (\n"
    "  session_id : 'deadbeef-1234-5678-9abc-def012345678';\n"
    "  handle : 'testkeeper';\n"
    "  model : 'sonnet[1m]';\n"
    "  nbs_ts_handle : '';\n"
    "  transport : Ts;\n"
    "  started : '2026-03-30T15:00:00Z';\n"
    "  project_root : '/path/with spaces/here';\n"
    "  pid : 99999;\n"
    "  initial_prompt_set : False;\n"
    ");\n";

/* ================================================================== */
/* Test: parse valid document and extract all string fields             */
/* ================================================================== */

static void test_parse_valid_strings(void)
{
    char path[256];
    build_path(path, sizeof(path), "valid.honest");
    write_file(path, VALID_SESSION_META);

    hon_diag_list diags = {0};
    hon_document *doc = hon_parse_file(path, &diags);
    TEST_ASSERT(doc != NULL, "hon_parse_file returned NULL for valid document");

    char buf[256];

    TEST_ASSERT(hon_get_string(doc, "result", "session_id", buf, sizeof(buf)) == 0,
                "failed to extract session_id");
    TEST_ASSERT(strcmp(buf, "a1b2c3d4-e5f6-7890-abcd-ef1234567890") == 0,
                "session_id mismatch: got '%s'", buf);

    TEST_ASSERT(hon_get_string(doc, "result", "handle", buf, sizeof(buf)) == 0,
                "failed to extract handle");
    TEST_ASSERT(strcmp(buf, "supervisor") == 0,
                "handle mismatch: got '%s'", buf);

    TEST_ASSERT(hon_get_string(doc, "result", "model", buf, sizeof(buf)) == 0,
                "failed to extract model");
    TEST_ASSERT(strcmp(buf, "opus[1m]") == 0,
                "model mismatch: got '%s'", buf);

    TEST_ASSERT(hon_get_string(doc, "result", "nbs_ts_handle", buf, sizeof(buf)) == 0,
                "failed to extract nbs_ts_handle");
    TEST_ASSERT(strcmp(buf, "nbs-supervisor-session") == 0,
                "nbs_ts_handle mismatch: got '%s'", buf);

    TEST_ASSERT(hon_get_string(doc, "result", "started", buf, sizeof(buf)) == 0,
                "failed to extract started");
    TEST_ASSERT(strcmp(buf, "2026-03-30T14:52:17Z") == 0,
                "started mismatch: got '%s'", buf);

    TEST_ASSERT(hon_get_string(doc, "result", "project_root", buf, sizeof(buf)) == 0,
                "failed to extract project_root");
    TEST_ASSERT(strcmp(buf, "/data/users/alex/project") == 0,
                "project_root mismatch: got '%s'", buf);

    /* transport as string (hon_get_string works on enums) */
    TEST_ASSERT(hon_get_string(doc, "result", "transport", buf, sizeof(buf)) == 0,
                "failed to extract transport as string");
    TEST_ASSERT(strcmp(buf, "Auto") == 0,
                "transport mismatch: got '%s'", buf);

    hon_doc_free(doc);
    TEST_PASS("parse valid document: all string fields extracted correctly");
}

/* ================================================================== */
/* Test: extract LongInt (pid) field                                   */
/* ================================================================== */

static void test_parse_valid_long(void)
{
    char path[256];
    build_path(path, sizeof(path), "valid_long.honest");
    write_file(path, VALID_SESSION_META);

    hon_diag_list diags = {0};
    hon_document *doc = hon_parse_file(path, &diags);
    TEST_ASSERT(doc != NULL, "hon_parse_file returned NULL");

    long pid_val;
    TEST_ASSERT(hon_get_long(doc, "result", "pid", &pid_val) == 0,
                "failed to extract pid");
    TEST_ASSERT(pid_val == 42, "pid mismatch: got %ld", pid_val);

    hon_doc_free(doc);
    TEST_PASS("parse valid document: pid (LongInt) extracted correctly");
}

/* ================================================================== */
/* Test: extract Boolean (initial_prompt_set) field                    */
/* ================================================================== */

static void test_parse_valid_bool_true(void)
{
    char path[256];
    build_path(path, sizeof(path), "valid_bool_true.honest");
    write_file(path, VALID_SESSION_META);

    hon_diag_list diags = {0};
    hon_document *doc = hon_parse_file(path, &diags);
    TEST_ASSERT(doc != NULL, "hon_parse_file returned NULL");

    bool val;
    TEST_ASSERT(hon_get_bool(doc, "result", "initial_prompt_set", &val) == 0,
                "failed to extract initial_prompt_set");
    TEST_ASSERT(val == true, "initial_prompt_set should be true");

    hon_doc_free(doc);
    TEST_PASS("parse valid document: initial_prompt_set=True extracted correctly");
}

static void test_parse_valid_bool_false(void)
{
    char path[256];
    build_path(path, sizeof(path), "valid_bool_false.honest");
    write_file(path, VALID_SESSION_META_TS);

    hon_diag_list diags = {0};
    hon_document *doc = hon_parse_file(path, &diags);
    TEST_ASSERT(doc != NULL, "hon_parse_file returned NULL");

    bool val;
    TEST_ASSERT(hon_get_bool(doc, "result", "initial_prompt_set", &val) == 0,
                "failed to extract initial_prompt_set");
    TEST_ASSERT(val == false, "initial_prompt_set should be false");

    hon_doc_free(doc);
    TEST_PASS("parse valid document: initial_prompt_set=False extracted correctly");
}

/* ================================================================== */
/* Test: Transport enum variant Ts                                     */
/* ================================================================== */

static void test_parse_transport_ts(void)
{
    char path[256];
    build_path(path, sizeof(path), "valid_ts.honest");
    write_file(path, VALID_SESSION_META_TS);

    hon_diag_list diags = {0};
    hon_document *doc = hon_parse_file(path, &diags);
    TEST_ASSERT(doc != NULL, "hon_parse_file returned NULL");

    char buf[64];
    TEST_ASSERT(hon_get_string(doc, "result", "transport", buf, sizeof(buf)) == 0,
                "failed to extract transport");
    TEST_ASSERT(strcmp(buf, "Ts") == 0,
                "transport mismatch: expected 'Ts', got '%s'", buf);

    hon_doc_free(doc);
    TEST_PASS("parse valid document: transport=Ts extracted correctly");
}

/* ================================================================== */
/* Test: empty nbs_ts_handle (common before ts session is created)     */
/* ================================================================== */

static void test_parse_empty_ts_handle(void)
{
    char path[256];
    build_path(path, sizeof(path), "empty_ts.honest");
    write_file(path, VALID_SESSION_META_TS);

    hon_diag_list diags = {0};
    hon_document *doc = hon_parse_file(path, &diags);
    TEST_ASSERT(doc != NULL, "hon_parse_file returned NULL");

    char buf[64];
    TEST_ASSERT(hon_get_string(doc, "result", "nbs_ts_handle", buf, sizeof(buf)) == 0,
                "failed to extract nbs_ts_handle");
    TEST_ASSERT(strcmp(buf, "") == 0,
                "nbs_ts_handle should be empty, got '%s'", buf);

    hon_doc_free(doc);
    TEST_PASS("parse valid document: empty nbs_ts_handle handled correctly");
}

/* ================================================================== */
/* Test: path with spaces in project_root                              */
/* ================================================================== */

static void test_parse_path_with_spaces(void)
{
    char path[256];
    build_path(path, sizeof(path), "spaces.honest");
    write_file(path, VALID_SESSION_META_TS);

    hon_diag_list diags = {0};
    hon_document *doc = hon_parse_file(path, &diags);
    TEST_ASSERT(doc != NULL, "hon_parse_file returned NULL");

    char buf[256];
    TEST_ASSERT(hon_get_string(doc, "result", "project_root", buf, sizeof(buf)) == 0,
                "failed to extract project_root");
    TEST_ASSERT(strcmp(buf, "/path/with spaces/here") == 0,
                "project_root mismatch: got '%s'", buf);

    hon_doc_free(doc);
    TEST_PASS("parse valid document: path with spaces round-trips correctly");
}

/* ================================================================== */
/* Test: large PID value                                               */
/* ================================================================== */

static void test_parse_large_pid(void)
{
    char path[256];
    build_path(path, sizeof(path), "large_pid.honest");
    write_file(path, VALID_SESSION_META_TS);

    hon_diag_list diags = {0};
    hon_document *doc = hon_parse_file(path, &diags);
    TEST_ASSERT(doc != NULL, "hon_parse_file returned NULL");

    long pid_val;
    TEST_ASSERT(hon_get_long(doc, "result", "pid", &pid_val) == 0,
                "failed to extract pid");
    TEST_ASSERT(pid_val == 99999, "pid mismatch: expected 99999, got %ld", pid_val);

    hon_doc_free(doc);
    TEST_PASS("parse valid document: large pid value handled correctly");
}

/* ================================================================== */
/* Test: empty file returns parse error                                */
/* ================================================================== */

static void test_parse_empty_file(void)
{
    char path[256];
    build_path(path, sizeof(path), "empty.honest");
    write_file(path, "");

    hon_diag_list diags = {0};
    hon_document *doc = hon_parse_file(path, &diags);
    /* Empty file should either return NULL or produce a document with no vars */
    if (doc != NULL) {
        /* If it parses, extracting fields should fail */
        char buf[64];
        int rc = hon_get_string(doc, "result", "session_id", buf, sizeof(buf));
        TEST_ASSERT(rc != 0,
                    "extracting session_id from empty document should fail");
        hon_doc_free(doc);
    }
    /* Either way, we handled the empty file without crashing */
    TEST_PASS("parse empty file: no crash, extraction fails gracefully");
}

/* ================================================================== */
/* Test: nonexistent file returns NULL                                 */
/* ================================================================== */

static void test_parse_nonexistent_file(void)
{
    char path[256];
    build_path(path, sizeof(path), "does_not_exist.honest");

    hon_diag_list diags = {0};
    hon_document *doc = hon_parse_file(path, &diags);
    TEST_ASSERT(doc == NULL,
                "hon_parse_file should return NULL for nonexistent file");
    TEST_PASS("parse nonexistent file: returns NULL");
}

/* ================================================================== */
/* Test: malformed document (truncated)                                */
/* ================================================================== */

static void test_parse_malformed_truncated(void)
{
    char path[256];
    build_path(path, sizeof(path), "truncated.honest");
    /* Cut off mid-record */
    write_file(path,
        "type\n"
        "  Transport = (Auto, Ts);\n"
        "\n"
        "  SessionMeta = record\n"
        "    session_id : String;\n"
    );

    hon_diag_list diags = {0};
    hon_document *doc = hon_parse_file(path, &diags);
    /* Truncated doc should either fail to parse or have no extractable var */
    if (doc != NULL) {
        char buf[64];
        int rc = hon_get_string(doc, "result", "session_id", buf, sizeof(buf));
        TEST_ASSERT(rc != 0,
                    "extracting from truncated document should fail");
        hon_doc_free(doc);
    }
    TEST_PASS("parse truncated document: no crash, handled gracefully");
}

/* ================================================================== */
/* Test: wrong variable name returns error                             */
/* ================================================================== */

static void test_extract_wrong_var_name(void)
{
    char path[256];
    build_path(path, sizeof(path), "wrong_var.honest");
    write_file(path, VALID_SESSION_META);

    hon_diag_list diags = {0};
    hon_document *doc = hon_parse_file(path, &diags);
    TEST_ASSERT(doc != NULL, "hon_parse_file returned NULL");

    char buf[64];
    int rc = hon_get_string(doc, "nonexistent", "session_id", buf, sizeof(buf));
    TEST_ASSERT(rc != 0,
                "extracting with wrong var name should fail");

    hon_doc_free(doc);
    TEST_PASS("extract wrong var name: returns error");
}

/* ================================================================== */
/* Test: wrong field name returns error                                */
/* ================================================================== */

static void test_extract_wrong_field_name(void)
{
    char path[256];
    build_path(path, sizeof(path), "wrong_field.honest");
    write_file(path, VALID_SESSION_META);

    hon_diag_list diags = {0};
    hon_document *doc = hon_parse_file(path, &diags);
    TEST_ASSERT(doc != NULL, "hon_parse_file returned NULL");

    char buf[64];
    int rc = hon_get_string(doc, "result", "nonexistent_field", buf, sizeof(buf));
    TEST_ASSERT(rc != 0,
                "extracting nonexistent field should fail");

    hon_doc_free(doc);
    TEST_PASS("extract wrong field name: returns error");
}

/* ================================================================== */
/* Test: buffer too small for string value                             */
/* ================================================================== */

static void test_extract_buffer_too_small(void)
{
    char path[256];
    build_path(path, sizeof(path), "small_buf.honest");
    write_file(path, VALID_SESSION_META);

    hon_diag_list diags = {0};
    hon_document *doc = hon_parse_file(path, &diags);
    TEST_ASSERT(doc != NULL, "hon_parse_file returned NULL");

    /* session_id is 36 chars; buffer of 4 is too small */
    char tiny_buf[4];
    int rc = hon_get_string(doc, "result", "session_id", tiny_buf, sizeof(tiny_buf));
    TEST_ASSERT(rc != 0,
                "extracting into too-small buffer should fail");

    hon_doc_free(doc);
    TEST_PASS("extract with small buffer: returns error (no overflow)");
}

/* ================================================================== */
/* Test: model string with brackets (opus[1m])                         */
/* ================================================================== */

static void test_parse_model_with_brackets(void)
{
    char path[256];
    build_path(path, sizeof(path), "brackets.honest");
    write_file(path, VALID_SESSION_META);

    hon_diag_list diags = {0};
    hon_document *doc = hon_parse_file(path, &diags);
    TEST_ASSERT(doc != NULL, "hon_parse_file returned NULL");

    char buf[64];
    TEST_ASSERT(hon_get_string(doc, "result", "model", buf, sizeof(buf)) == 0,
                "failed to extract model");
    TEST_ASSERT(strcmp(buf, "opus[1m]") == 0,
                "model with brackets mismatch: got '%s'", buf);

    hon_doc_free(doc);
    TEST_PASS("parse model with brackets: opus[1m] round-trips correctly");
}

/* ================================================================== */
/* Test: complete round-trip via honest-build CLI → hon_parse_file      */
/* ================================================================== */

static void test_cli_roundtrip(void)
{
    char honest_path[256];
    char rulebook_path[256];
    build_path(honest_path, sizeof(honest_path), "cli_roundtrip.honest");
    build_path(rulebook_path, sizeof(rulebook_path), "session.honest-rulebook");

    /* Write rulebook */
    write_file(rulebook_path,
        "type Transport = (Auto, Ts); "
        "SessionMeta = record "
        "session_id : String; "
        "handle : String; "
        "model : String; "
        "nbs_ts_handle : String; "
        "transport : Transport; "
        "started : String; "
        "project_root : String; "
        "pid : LongInt; "
        "initial_prompt_set : Boolean; "
        "end;\n"
    );

    /* Resolve honest-build path from this source file's known location.
     * This file lives at tests/test_workers_honest_unit.c, so the
     * honest-build binary is at lib/honest/build/honest-build relative
     * to the repo root. Use __FILE__ to derive the path. */
    char honest_build_path[512];
    {
        /* __FILE__ is typically a relative or absolute path to this .c file.
         * We know the Makefile compiles from src/nbs-workers/, so the path
         * ../../lib/honest/build/honest-build works from there. But to also
         * work from the repo root, try both the Makefile-relative path and
         * a path relative to the tests/ directory. */
        const char *candidates[] = {
            "../../lib/honest/build/honest-build",   /* from src/nbs-workers/ */
            "lib/honest/build/honest-build",          /* from repo root */
            NULL
        };
        honest_build_path[0] = '\0';
        for (int i = 0; candidates[i]; i++) {
            if (access(candidates[i], X_OK) == 0) {
                snprintf(honest_build_path, sizeof(honest_build_path),
                         "%s", candidates[i]);
                break;
            }
        }
        if (honest_build_path[0] == '\0') {
            fprintf(stderr, "SKIP: honest-build not found (tried from cwd)\n");
            tests_passed++;
            printf("  SKIP: CLI round-trip (honest-build not in expected location)\n");
            return;
        }
    }

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
        "%s --type SessionMeta "
        "--rulebook %s "
        "session_id=\"cli-test-uuid\" "
        "handle=\"worker\" "
        "model=\"haiku\" "
        "nbs_ts_handle=\"\" "
        "transport=Auto "
        "started=\"2026-01-01T00:00:00Z\" "
        "project_root=\"/tmp/test\" "
        "pid=1234 "
        "initial_prompt_set=False "
        "> %s",
        honest_build_path, rulebook_path, honest_path);
    int rc = system(cmd);
    TEST_ASSERT(rc == 0, "honest-build CLI failed with exit code %d", rc);

    /* Now parse the output with the C API */
    hon_diag_list diags = {0};
    hon_document *doc = hon_parse_file(honest_path, &diags);
    TEST_ASSERT(doc != NULL, "hon_parse_file returned NULL for CLI output");

    char buf[256];
    TEST_ASSERT(hon_get_string(doc, "result", "session_id", buf, sizeof(buf)) == 0,
                "failed to extract session_id from CLI output");
    TEST_ASSERT(strcmp(buf, "cli-test-uuid") == 0,
                "session_id mismatch: got '%s'", buf);

    TEST_ASSERT(hon_get_string(doc, "result", "handle", buf, sizeof(buf)) == 0,
                "failed to extract handle from CLI output");
    TEST_ASSERT(strcmp(buf, "worker") == 0,
                "handle mismatch: got '%s'", buf);

    TEST_ASSERT(hon_get_string(doc, "result", "transport", buf, sizeof(buf)) == 0,
                "failed to extract transport from CLI output");
    TEST_ASSERT(strcmp(buf, "Auto") == 0,
                "transport mismatch: got '%s'", buf);

    long pid_val;
    TEST_ASSERT(hon_get_long(doc, "result", "pid", &pid_val) == 0,
                "failed to extract pid from CLI output");
    TEST_ASSERT(pid_val == 1234, "pid mismatch: got %ld", pid_val);

    bool prompt_set;
    TEST_ASSERT(hon_get_bool(doc, "result", "initial_prompt_set", &prompt_set) == 0,
                "failed to extract initial_prompt_set from CLI output");
    TEST_ASSERT(prompt_set == false,
                "initial_prompt_set should be false");

    TEST_ASSERT(hon_get_string(doc, "result", "nbs_ts_handle", buf, sizeof(buf)) == 0,
                "failed to extract nbs_ts_handle from CLI output");
    TEST_ASSERT(strcmp(buf, "") == 0,
                "nbs_ts_handle should be empty, got '%s'", buf);

    hon_doc_free(doc);
    TEST_PASS("CLI round-trip: honest-build output parsed correctly by C API");
}

/* ================================================================== */
/* Main                                                                */
/* ================================================================== */

int main(void)
{
    setup_tmp_dir();

    printf("=== honest session metadata tests ===\n\n");

    printf("[STRING EXTRACTION]\n");
    test_parse_valid_strings();

    printf("\n[LONG EXTRACTION]\n");
    test_parse_valid_long();
    test_parse_large_pid();

    printf("\n[BOOLEAN EXTRACTION]\n");
    test_parse_valid_bool_true();
    test_parse_valid_bool_false();

    printf("\n[ENUM EXTRACTION]\n");
    test_parse_transport_ts();

    printf("\n[EDGE CASES]\n");
    test_parse_empty_ts_handle();
    test_parse_path_with_spaces();
    test_parse_model_with_brackets();

    printf("\n[ERROR HANDLING]\n");
    test_parse_empty_file();
    test_parse_nonexistent_file();
    test_parse_malformed_truncated();
    test_extract_wrong_var_name();
    test_extract_wrong_field_name();
    test_extract_buffer_too_small();

    printf("\n[CLI ROUND-TRIP]\n");
    test_cli_roundtrip();

    cleanup_tmp_dir();

    printf("\n=== Results: %d passed, %d failed ===\n",
           tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
