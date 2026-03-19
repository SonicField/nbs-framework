/*
 * hub.c — NBS Hub implementation.
 *
 * Deterministic process enforcement for AI supervisors.
 * All state is file-based, human-readable, crash-recoverable.
 * All writes are atomic (write temp, rename).
 */

#include "hub.h"
#include "nbs_assert.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* ================================================================
 * Utility functions
 * ================================================================ */

void iso_timestamp(char *buf, int size)
{
    ASSERT_MSG(buf != NULL, "iso_timestamp: buf is NULL");
    ASSERT_MSG(size >= TIMESTAMP_SIZE, "iso_timestamp: buf too small: %d", size);

    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(buf, (size_t)size, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

int file_exists(const char *path)
{
    ASSERT_MSG(path != NULL, "file_exists: path is NULL");
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int file_nonempty(const char *path)
{
    ASSERT_MSG(path != NULL, "file_nonempty: path is NULL");
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

int file_contains(const char *path, const char *substr)
{
    ASSERT_MSG(path != NULL, "file_contains: path is NULL");
    ASSERT_MSG(substr != NULL, "file_contains: substr is NULL");

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[LINE_BUF_SIZE];
    while (fgets(line, (int)sizeof(line), f)) {
        if (strstr(line, substr)) {
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

int ensure_dir(const char *path)
{
    ASSERT_MSG(path != NULL, "ensure_dir: path is NULL");
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return 0;
    return mkdir(path, 0755);
}

int find_project_dir(const char *cwd, char *out_dir)
{
    ASSERT_MSG(cwd != NULL, "find_project_dir: cwd is NULL");
    ASSERT_MSG(out_dir != NULL, "find_project_dir: out_dir is NULL");

    char check[PATH_BUF_SIZE];
    snprintf(out_dir, PATH_BUF_SIZE, "%s", cwd);

    for (;;) {
        snprintf(check, sizeof(check), "%s/%s", out_dir, HUB_SUBDIR);
        struct stat st;
        if (stat(check, &st) == 0 && S_ISDIR(st.st_mode))
            return 0;

        /* Move up one directory */
        char *slash = strrchr(out_dir, '/');
        if (!slash || slash == out_dir)
            return -1;
        *slash = '\0';
    }
}

/* ================================================================
 * State I/O — key=value files
 * ================================================================ */

int state_read(const char *path, const char *key,
               char *out_value, int out_size)
{
    ASSERT_MSG(path != NULL, "state_read: path is NULL");
    ASSERT_MSG(key != NULL, "state_read: key is NULL");
    ASSERT_MSG(out_value != NULL, "state_read: out_value is NULL");
    ASSERT_MSG(out_size > 0, "state_read: out_size must be > 0");

    FILE *f = fopen(path, "r");
    if (!f) return 0;

    size_t klen = strlen(key);
    char line[LINE_BUF_SIZE];

    while (fgets(line, (int)sizeof(line), f)) {
        if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
            const char *val = line + klen + 1;
            /* Strip trailing newline */
            size_t vlen = strlen(val);
            if (vlen > 0 && val[vlen - 1] == '\n')
                vlen--;
            if ((int)vlen >= out_size)
                vlen = (size_t)(out_size - 1);
            memcpy(out_value, val, vlen);
            out_value[vlen] = '\0';
            fclose(f);
            return 1;
        }
    }
    fclose(f);
    return 0;
}

int state_write(const char *path, const char *key, const char *value)
{
    ASSERT_MSG(path != NULL, "state_write: path is NULL");
    ASSERT_MSG(key != NULL, "state_write: key is NULL");
    ASSERT_MSG(value != NULL, "state_write: value is NULL");

    char tmp[PATH_BUF_SIZE];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());

    /* Read existing file, write to temp replacing key if found */
    FILE *out = fopen(tmp, "w");
    if (!out) return -1;

    int found = 0;
    FILE *in = fopen(path, "r");
    if (in) {
        size_t klen = strlen(key);
        char line[LINE_BUF_SIZE];
        while (fgets(line, (int)sizeof(line), in)) {
            if (strncmp(line, key, klen) == 0 && line[klen] == '=') {
                fprintf(out, "%s=%s\n", key, value);
                found = 1;
            } else {
                fputs(line, out);
            }
        }
        fclose(in);
    }

    if (!found)
        fprintf(out, "%s=%s\n", key, value);

    fclose(out);
    return rename(tmp, path);
}

void hub_log(const char *project_dir, const char *message)
{
    ASSERT_MSG(project_dir != NULL, "hub_log: project_dir is NULL");
    ASSERT_MSG(message != NULL, "hub_log: message is NULL");

    char path[PATH_BUF_SIZE];
    snprintf(path, sizeof(path), "%s/%s", project_dir, HUB_LOG);

    char ts[TIMESTAMP_SIZE];
    iso_timestamp(ts, sizeof(ts));

    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "[%s] %s\n", ts, message);
        fclose(f);
    }
}

void hub_chat(const char *project_dir, const char *message)
{
    ASSERT_MSG(project_dir != NULL, "hub_chat: project_dir is NULL");
    ASSERT_MSG(message != NULL, "hub_chat: message is NULL");

    char chat_path[PATH_BUF_SIZE];
    snprintf(chat_path, sizeof(chat_path), "%s/%s", project_dir, HUB_CHAT);

    /* Use nbs-chat if chat file exists, otherwise skip silently */
    if (file_exists(chat_path)) {
        char cmd[PATH_BUF_SIZE * 2];
        /* Sanitise message for shell — replace single quotes */
        snprintf(cmd, sizeof(cmd),
                 "nbs-chat send '%s' hub '%s' 2>/dev/null",
                 chat_path, message);
        (void)system(cmd);
    }
}

/* ================================================================
 * Stall detection — check last spawn time
 * ================================================================ */

static void check_stall(const char *project_dir)
{
    char val[VALUE_BUF_SIZE];
    char state_path[PATH_BUF_SIZE];
    snprintf(state_path, sizeof(state_path), "%s/%s",
             project_dir, HUB_STATE);

    /* Check phase — don't warn if COMPLETE */
    if (state_read(state_path, "phase_name", val, sizeof(val)) &&
        strcmp(val, "COMPLETE") == 0)
        return;

    if (!state_read(state_path, "last_spawn_time", val, sizeof(val)))
        return;

    long last = strtol(val, NULL, 10);
    if (last <= 0) return;

    time_t now = time(NULL);
    long elapsed_min = (now - last) / 60;

    if (elapsed_min >= STALL_MINUTES) {
        fprintf(stderr,
                "[HUB-WARNING] No worker spawned for %ld minutes. "
                "Are you doing tactical work? Delegate or escalate.\n",
                elapsed_min);
    }
}

/* ================================================================
 * Commands
 * ================================================================ */

int cmd_init(const char *project_dir, const char *goal)
{
    ASSERT_MSG(project_dir != NULL, "cmd_init: project_dir is NULL");
    ASSERT_MSG(goal != NULL, "cmd_init: goal is NULL");

    char hub_dir[PATH_BUF_SIZE];
    snprintf(hub_dir, sizeof(hub_dir), "%s/%s", project_dir, HUB_SUBDIR);

    struct stat st;
    if (stat(hub_dir, &st) == 0) {
        fprintf(stderr, "Error: hub already initialised at %s\n", hub_dir);
        return EXIT_ERROR;
    }

    /* Create directories */
    ensure_dir(hub_dir);

    char audits[PATH_BUF_SIZE];
    snprintf(audits, sizeof(audits), "%s/%s", project_dir, HUB_AUDITS);
    ensure_dir(audits);

    char gates[PATH_BUF_SIZE];
    snprintf(gates, sizeof(gates), "%s/%s", project_dir, HUB_GATES);
    ensure_dir(gates);

    /* Ensure .nbs/chat/ exists */
    char chat_dir[PATH_BUF_SIZE];
    snprintf(chat_dir, sizeof(chat_dir), "%s/.nbs/chat", project_dir);
    ensure_dir(chat_dir);

    /* Write manifest */
    char manifest[PATH_BUF_SIZE];
    snprintf(manifest, sizeof(manifest), "%s/%s", project_dir, HUB_MANIFEST);
    state_write(manifest, "project_dir", project_dir);
    state_write(manifest, "goal", goal);

    /* Write initial state */
    char state_path[PATH_BUF_SIZE];
    snprintf(state_path, sizeof(state_path), "%s/%s",
             project_dir, HUB_STATE);
    state_write(state_path, "phase_num", "0");
    state_write(state_path, "phase_name", "PLANNING");
    state_write(state_path, "audit_counter", "0");
    state_write(state_path, "audit_required", "0");

    char ts_buf[32];
    snprintf(ts_buf, sizeof(ts_buf), "%ld", (long)time(NULL));
    state_write(state_path, "last_spawn_time", ts_buf);

    /* Create hub.chat */
    char chat_path[PATH_BUF_SIZE];
    snprintf(chat_path, sizeof(chat_path), "%s/%s", project_dir, HUB_CHAT);
    if (!file_exists(chat_path)) {
        char cmd[PATH_BUF_SIZE];
        snprintf(cmd, sizeof(cmd), "nbs-chat create '%s' 2>/dev/null",
                 chat_path);
        (void)system(cmd);
    }

    hub_log(project_dir, "Hub initialised");

    char log_msg[VALUE_BUF_SIZE];
    snprintf(log_msg, sizeof(log_msg), "Terminal goal: %s", goal);
    hub_log(project_dir, log_msg);
    hub_log(project_dir, "Phase 0: PLANNING");

    printf("Hub initialised at %s\n", hub_dir);
    printf("Terminal goal: %s\n", goal);
    printf("Phase: 0 -- PLANNING\n");

    return EXIT_SUCCESS_CODE;
}

int cmd_status(const char *project_dir)
{
    ASSERT_MSG(project_dir != NULL, "cmd_status: project_dir is NULL");

    char manifest[PATH_BUF_SIZE];
    snprintf(manifest, sizeof(manifest), "%s/%s",
             project_dir, HUB_MANIFEST);
    char state_path[PATH_BUF_SIZE];
    snprintf(state_path, sizeof(state_path), "%s/%s",
             project_dir, HUB_STATE);

    char val[VALUE_BUF_SIZE];

    /* Terminal goal */
    printf("=== NBS Hub Status ===\n\n");

    if (state_read(manifest, "goal", val, sizeof(val)))
        printf("Terminal goal: %s\n\n", val);

    /* Phase */
    char phase_num[32] = "?";
    char phase_name[256] = "?";
    state_read(state_path, "phase_num", phase_num, sizeof(phase_num));
    state_read(state_path, "phase_name", phase_name, sizeof(phase_name));
    printf("Phase: %s -- %s\n", phase_num, phase_name);

    /* Audit state */
    char counter[32] = "0";
    char required[32] = "0";
    state_read(state_path, "audit_counter", counter, sizeof(counter));
    state_read(state_path, "audit_required", required, sizeof(required));
    int cnt = atoi(counter);
    int req = atoi(required);

    printf("Audit: %d/%d results since last check", cnt, AUDIT_THRESHOLD);
    if (req)
        printf(" [AUDIT REQUIRED — spawns blocked]");
    printf("\n\n");

    /* Registered documents */
    printf("Documents:\n");
    char doc_key[128];
    int doc_found = 0;
    for (int i = 0; i < MAX_DOCS; i++) {
        snprintf(doc_key, sizeof(doc_key), "doc.%d.name", i);
        char dname[256];
        if (!state_read(manifest, doc_key, dname, sizeof(dname)))
            break;
        snprintf(doc_key, sizeof(doc_key), "doc.%d.path", i);
        char dpath[PATH_BUF_SIZE];
        if (!state_read(manifest, doc_key, dpath, sizeof(dpath)))
            break;
        printf("  %-20s %s%s\n", dname, dpath,
               file_exists(dpath) ? "" : " [MISSING]");
        doc_found = 1;
    }
    if (!doc_found)
        printf("  (none registered)\n");
    printf("\n");

    /* Workers — delegate to nbs-workers list */
    printf("Workers:\n");
    fflush(stdout);
    char cmd[PATH_BUF_SIZE];
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && nbs-workers list 2>/dev/null | head -30",
             project_dir);
    (void)system(cmd);
    printf("\n");

    /* Last 10 log entries */
    cmd_log(10, project_dir);

    check_stall(project_dir);

    return EXIT_SUCCESS_CODE;
}

int cmd_spawn(const char *slug, const char *task_desc,
              const char *project_dir)
{
    ASSERT_MSG(slug != NULL, "cmd_spawn: slug is NULL");
    ASSERT_MSG(task_desc != NULL, "cmd_spawn: task_desc is NULL");
    ASSERT_MSG(project_dir != NULL, "cmd_spawn: project_dir is NULL");

    char state_path[PATH_BUF_SIZE];
    snprintf(state_path, sizeof(state_path), "%s/%s",
             project_dir, HUB_STATE);

    /* Check audit gate */
    char required[32] = "0";
    state_read(state_path, "audit_required", required, sizeof(required));
    if (atoi(required)) {
        fprintf(stderr,
                "Error: spawn REFUSED — audit required.\n"
                "%d workers completed since last self-check.\n"
                "Submit: nbs-hub audit <file>\n",
                AUDIT_THRESHOLD);
        hub_log(project_dir, "SPAWN REFUSED: audit gate active");
        return EXIT_AUDIT_REQUIRED;
    }

    /* Delegate to nbs-workers spawn */
    char cmd[PATH_BUF_SIZE * 2];
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && nbs-workers spawn '%s' '%s' '%s'",
             project_dir, slug, project_dir, task_desc);

    FILE *p = popen(cmd, "r");
    if (!p) {
        fprintf(stderr, "Error: failed to execute nbs-workers spawn\n");
        return EXIT_ERROR;
    }

    char worker_name[256] = "";
    char line[LINE_BUF_SIZE];
    while (fgets(line, (int)sizeof(line), p)) {
        fputs(line, stdout);
        /* nbs-workers spawn prints worker name as last line */
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n')
            line[len - 1] = '\0';
        /* Worker name matches slug-XXXX pattern */
        if (strstr(line, slug) == line) {
            snprintf(worker_name, sizeof(worker_name), "%s", line);
        }
    }

    int rc = pclose(p);
    if (rc != 0)
        return EXIT_ERROR;

    /* Update last spawn time */
    char ts_buf[32];
    snprintf(ts_buf, sizeof(ts_buf), "%ld", (long)time(NULL));
    state_write(state_path, "last_spawn_time", ts_buf);

    /* Log */
    char log_msg[VALUE_BUF_SIZE];
    snprintf(log_msg, sizeof(log_msg), "SPAWN: %s — %s",
             worker_name[0] ? worker_name : slug, task_desc);
    hub_log(project_dir, log_msg);

    return EXIT_SUCCESS_CODE;
}

int cmd_check(const char *worker_name, const char *project_dir)
{
    ASSERT_MSG(worker_name != NULL, "cmd_check: worker_name is NULL");
    ASSERT_MSG(project_dir != NULL, "cmd_check: project_dir is NULL");

    char cmd[PATH_BUF_SIZE];
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && nbs-workers status '%s'",
             project_dir, worker_name);

    check_stall(project_dir);
    return system(cmd) == 0 ? EXIT_SUCCESS_CODE : EXIT_ERROR;
}

int cmd_result(const char *worker_name, const char *project_dir)
{
    ASSERT_MSG(worker_name != NULL, "cmd_result: worker_name is NULL");
    ASSERT_MSG(project_dir != NULL, "cmd_result: project_dir is NULL");

    /* Delegate to nbs-workers results */
    char cmd[PATH_BUF_SIZE];
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && nbs-workers results '%s'",
             project_dir, worker_name);
    int rc = system(cmd);
    if (rc != 0)
        return EXIT_ERROR;

    /* Increment audit counter */
    char state_path[PATH_BUF_SIZE];
    snprintf(state_path, sizeof(state_path), "%s/%s",
             project_dir, HUB_STATE);

    char counter_str[32] = "0";
    state_read(state_path, "audit_counter", counter_str, sizeof(counter_str));
    int counter = atoi(counter_str) + 1;

    snprintf(counter_str, sizeof(counter_str), "%d", counter);
    state_write(state_path, "audit_counter", counter_str);

    /* Check if audit gate should fire */
    if (counter >= AUDIT_THRESHOLD) {
        state_write(state_path, "audit_required", "1");
        fprintf(stderr,
                "\n[HUB] Audit gate activated: %d results without "
                "self-check.\nSubmit: nbs-hub audit <file>\n", counter);
        hub_log(project_dir, "AUDIT GATE: activated");
    }

    char log_msg[VALUE_BUF_SIZE];
    snprintf(log_msg, sizeof(log_msg), "RESULT: %s (audit %d/%d)",
             worker_name, counter, AUDIT_THRESHOLD);
    hub_log(project_dir, log_msg);

    return EXIT_SUCCESS_CODE;
}

int cmd_dismiss(const char *worker_name, const char *project_dir)
{
    ASSERT_MSG(worker_name != NULL, "cmd_dismiss: worker_name is NULL");
    ASSERT_MSG(project_dir != NULL, "cmd_dismiss: project_dir is NULL");

    char cmd[PATH_BUF_SIZE];
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && nbs-workers dismiss '%s'",
             project_dir, worker_name);

    int rc = system(cmd);

    char log_msg[VALUE_BUF_SIZE];
    snprintf(log_msg, sizeof(log_msg), "DISMISS: %s", worker_name);
    hub_log(project_dir, log_msg);

    return rc == 0 ? EXIT_SUCCESS_CODE : EXIT_ERROR;
}

int cmd_list(const char *project_dir)
{
    ASSERT_MSG(project_dir != NULL, "cmd_list: project_dir is NULL");

    char cmd[PATH_BUF_SIZE];
    snprintf(cmd, sizeof(cmd),
             "cd '%s' && nbs-workers list", project_dir);

    check_stall(project_dir);
    return system(cmd) == 0 ? EXIT_SUCCESS_CODE : EXIT_ERROR;
}

int cmd_audit(const char *audit_file, const char *project_dir)
{
    ASSERT_MSG(audit_file != NULL, "cmd_audit: audit_file is NULL");
    ASSERT_MSG(project_dir != NULL, "cmd_audit: project_dir is NULL");

    if (!file_nonempty(audit_file)) {
        fprintf(stderr,
                "Error: audit file does not exist or is empty: %s\n",
                audit_file);
        return EXIT_ERROR;
    }

    /* Check required sections */
    int has_goal = file_contains(audit_file, "goal") ||
                   file_contains(audit_file, "Goal") ||
                   file_contains(audit_file, "terminal");
    int has_delegation = file_contains(audit_file, "delegat") ||
                         file_contains(audit_file, "Delegat");
    int has_learnings = file_contains(audit_file, "3W") ||
                        file_contains(audit_file, "learn") ||
                        file_contains(audit_file, "Learn") ||
                        file_contains(audit_file, "well") ||
                        file_contains(audit_file, "Well");

    if (!has_goal) {
        fprintf(stderr,
                "Error: audit missing terminal goal alignment section.\n"
                "Include a section addressing: are you still pursuing "
                "the terminal goal?\n");
        return EXIT_ERROR;
    }
    if (!has_delegation) {
        fprintf(stderr,
                "Error: audit missing delegation assessment.\n"
                "Include a section addressing: are you delegating, "
                "not doing tactical work?\n");
        return EXIT_ERROR;
    }
    if (!has_learnings) {
        fprintf(stderr,
                "Error: audit missing learnings / 3Ws section.\n"
                "Include a section addressing: what went well, "
                "what did not, what to improve?\n");
        return EXIT_ERROR;
    }

    /* Archive the audit */
    char ts[TIMESTAMP_SIZE];
    iso_timestamp(ts, sizeof(ts));

    char archive[PATH_BUF_SIZE];
    snprintf(archive, sizeof(archive), "%s/%s/audit-%s.md",
             project_dir, HUB_AUDITS, ts);

    char cmd[PATH_BUF_SIZE * 2];
    snprintf(cmd, sizeof(cmd), "cp '%s' '%s'", audit_file, archive);
    (void)system(cmd);

    /* Reset audit state */
    char state_path[PATH_BUF_SIZE];
    snprintf(state_path, sizeof(state_path), "%s/%s",
             project_dir, HUB_STATE);
    state_write(state_path, "audit_counter", "0");
    state_write(state_path, "audit_required", "0");

    hub_log(project_dir, "AUDIT: accepted — gate cleared");
    hub_chat(project_dir, "[HUB] Audit accepted. Spawns unblocked.");

    printf("Audit accepted. Spawn gate cleared.\n");

    return EXIT_SUCCESS_CODE;
}

int cmd_gate(const char *phase_name, const char *test_file,
             const char *audit_file, const char *project_dir)
{
    ASSERT_MSG(phase_name != NULL, "cmd_gate: phase_name is NULL");
    ASSERT_MSG(test_file != NULL, "cmd_gate: test_file is NULL");
    ASSERT_MSG(audit_file != NULL, "cmd_gate: audit_file is NULL");
    ASSERT_MSG(project_dir != NULL, "cmd_gate: project_dir is NULL");

    char state_path[PATH_BUF_SIZE];
    snprintf(state_path, sizeof(state_path), "%s/%s",
             project_dir, HUB_STATE);

    /* Verify phase name matches current */
    char current_phase[256] = "";
    state_read(state_path, "phase_name", current_phase,
               sizeof(current_phase));

    if (strcmp(phase_name, current_phase) != 0) {
        fprintf(stderr,
                "Error: phase mismatch. Current is '%s', "
                "you submitted '%s'.\n"
                "No skipping phases.\n",
                current_phase, phase_name);
        return EXIT_ERROR;
    }

    /* Verify files exist and are non-empty */
    if (!file_nonempty(test_file)) {
        fprintf(stderr,
                "Error: test results file missing or empty: %s\n",
                test_file);
        return EXIT_ERROR;
    }
    if (!file_nonempty(audit_file)) {
        fprintf(stderr,
                "Error: audit file missing or empty: %s\n",
                audit_file);
        return EXIT_ERROR;
    }

    /* Advance phase */
    char phase_num_str[32] = "0";
    state_read(state_path, "phase_num", phase_num_str,
               sizeof(phase_num_str));
    int phase_num = atoi(phase_num_str) + 1;
    snprintf(phase_num_str, sizeof(phase_num_str), "%d", phase_num);
    state_write(state_path, "phase_num", phase_num_str);
    state_write(state_path, "phase_name", "UNNAMED");

    /* Archive gate materials */
    char ts[TIMESTAMP_SIZE];
    iso_timestamp(ts, sizeof(ts));

    char archive_base[PATH_BUF_SIZE];
    snprintf(archive_base, sizeof(archive_base), "%s/%s/gate-%d-%s",
             project_dir, HUB_GATES, phase_num - 1, ts);

    char cmd[PATH_BUF_SIZE * 2];
    snprintf(cmd, sizeof(cmd), "cp '%s' '%s-tests.md'",
             test_file, archive_base);
    (void)system(cmd);
    snprintf(cmd, sizeof(cmd), "cp '%s' '%s-audit.md'",
             audit_file, archive_base);
    (void)system(cmd);

    char log_msg[VALUE_BUF_SIZE];
    snprintf(log_msg, sizeof(log_msg),
             "GATE: phase %d (%s) passed. Now phase %d.",
             phase_num - 1, phase_name, phase_num);
    hub_log(project_dir, log_msg);
    hub_chat(project_dir, log_msg);

    printf("Gate passed. Phase %d complete.\n", phase_num - 1);
    printf("Now at phase %d — set name with: "
           "nbs-hub phase-name <name>\n", phase_num);

    return EXIT_SUCCESS_CODE;
}

int cmd_phase(const char *project_dir)
{
    ASSERT_MSG(project_dir != NULL, "cmd_phase: project_dir is NULL");

    char state_path[PATH_BUF_SIZE];
    snprintf(state_path, sizeof(state_path), "%s/%s",
             project_dir, HUB_STATE);

    char phase_num[32] = "?";
    char phase_name[256] = "?";
    char counter[32] = "0";
    char required[32] = "0";

    state_read(state_path, "phase_num", phase_num, sizeof(phase_num));
    state_read(state_path, "phase_name", phase_name, sizeof(phase_name));
    state_read(state_path, "audit_counter", counter, sizeof(counter));
    state_read(state_path, "audit_required", required, sizeof(required));

    printf("Phase: %s -- %s\n", phase_num, phase_name);
    printf("Audit: %s/%d results since last check", counter, AUDIT_THRESHOLD);
    if (atoi(required))
        printf(" [AUDIT REQUIRED]");
    printf("\n");

    check_stall(project_dir);

    return EXIT_SUCCESS_CODE;
}

int cmd_doc_register(const char *name, const char *doc_path,
                     const char *project_dir)
{
    ASSERT_MSG(name != NULL, "cmd_doc_register: name is NULL");
    ASSERT_MSG(doc_path != NULL, "cmd_doc_register: doc_path is NULL");
    ASSERT_MSG(project_dir != NULL, "cmd_doc_register: project_dir is NULL");

    /* Resolve to absolute path */
    char abs_path[PATH_BUF_SIZE];
    if (doc_path[0] == '/') {
        snprintf(abs_path, sizeof(abs_path), "%s", doc_path);
    } else {
        char cwd[PATH_BUF_SIZE];
        if (getcwd(cwd, sizeof(cwd)) == NULL) {
            fprintf(stderr, "Error: getcwd() failed\n");
            return EXIT_ERROR;
        }
        snprintf(abs_path, sizeof(abs_path), "%s/%s", cwd, doc_path);
    }

    char manifest[PATH_BUF_SIZE];
    snprintf(manifest, sizeof(manifest), "%s/%s",
             project_dir, HUB_MANIFEST);

    /* Find existing slot or next available */
    int slot = -1;
    for (int i = 0; i < MAX_DOCS; i++) {
        char key[128], val[256];
        snprintf(key, sizeof(key), "doc.%d.name", i);
        if (!state_read(manifest, key, val, sizeof(val))) {
            if (slot < 0) slot = i;
            break;
        }
        if (strcmp(val, name) == 0) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        fprintf(stderr, "Error: maximum documents (%d) reached\n", MAX_DOCS);
        return EXIT_ERROR;
    }

    char key[128];
    snprintf(key, sizeof(key), "doc.%d.name", slot);
    state_write(manifest, key, name);
    snprintf(key, sizeof(key), "doc.%d.path", slot);
    state_write(manifest, key, abs_path);

    char log_msg[VALUE_BUF_SIZE];
    snprintf(log_msg, sizeof(log_msg), "DOC REGISTER: %s -> %s",
             name, abs_path);
    hub_log(project_dir, log_msg);

    printf("Registered: %s -> %s\n", name, abs_path);

    return EXIT_SUCCESS_CODE;
}

int cmd_doc_list(const char *project_dir)
{
    ASSERT_MSG(project_dir != NULL, "cmd_doc_list: project_dir is NULL");

    char manifest[PATH_BUF_SIZE];
    snprintf(manifest, sizeof(manifest), "%s/%s",
             project_dir, HUB_MANIFEST);

    int found = 0;
    for (int i = 0; i < MAX_DOCS; i++) {
        char key[128], dname[256], dpath[PATH_BUF_SIZE];
        snprintf(key, sizeof(key), "doc.%d.name", i);
        if (!state_read(manifest, key, dname, sizeof(dname)))
            break;
        snprintf(key, sizeof(key), "doc.%d.path", i);
        if (!state_read(manifest, key, dpath, sizeof(dpath)))
            break;
        printf("  %-20s %s%s\n", dname, dpath,
               file_exists(dpath) ? "" : " [MISSING]");
        found = 1;
    }

    if (!found)
        printf("No documents registered.\n"
               "Use: nbs-hub doc register <name> <path>\n");

    return EXIT_SUCCESS_CODE;
}

int cmd_doc_read(const char *name, const char *project_dir)
{
    ASSERT_MSG(name != NULL, "cmd_doc_read: name is NULL");
    ASSERT_MSG(project_dir != NULL, "cmd_doc_read: project_dir is NULL");

    char manifest[PATH_BUF_SIZE];
    snprintf(manifest, sizeof(manifest), "%s/%s",
             project_dir, HUB_MANIFEST);

    for (int i = 0; i < MAX_DOCS; i++) {
        char key[128], dname[256], dpath[PATH_BUF_SIZE];
        snprintf(key, sizeof(key), "doc.%d.name", i);
        if (!state_read(manifest, key, dname, sizeof(dname)))
            break;
        if (strcmp(dname, name) != 0)
            continue;

        snprintf(key, sizeof(key), "doc.%d.path", i);
        if (!state_read(manifest, key, dpath, sizeof(dpath)))
            break;

        if (!file_exists(dpath)) {
            fprintf(stderr, "Error: document '%s' registered at %s "
                    "but file is MISSING\n", name, dpath);
            return EXIT_NOT_FOUND;
        }

        /* Cat the file */
        FILE *f = fopen(dpath, "r");
        if (!f) {
            fprintf(stderr, "Error: cannot open %s: %s\n",
                    dpath, strerror(errno));
            return EXIT_ERROR;
        }
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
            fwrite(buf, 1, n, stdout);
        fclose(f);
        return EXIT_SUCCESS_CODE;
    }

    fprintf(stderr, "Error: document '%s' not registered.\n"
            "Use: nbs-hub doc list  — to see registered documents\n"
            "Use: nbs-hub doc register <name> <path>  — to register\n",
            name);
    return EXIT_NOT_FOUND;
}

int cmd_decision(const char *text, const char *project_dir)
{
    ASSERT_MSG(text != NULL, "cmd_decision: text is NULL");
    ASSERT_MSG(project_dir != NULL, "cmd_decision: project_dir is NULL");

    char log_msg[VALUE_BUF_SIZE];
    snprintf(log_msg, sizeof(log_msg), "DECISION: %s", text);
    hub_log(project_dir, log_msg);
    hub_chat(project_dir, log_msg);

    printf("Decision recorded.\n");

    return EXIT_SUCCESS_CODE;
}

int cmd_log(int n, const char *project_dir)
{
    ASSERT_MSG(project_dir != NULL, "cmd_log: project_dir is NULL");
    ASSERT_MSG(n > 0, "cmd_log: n must be > 0, got %d", n);

    char log_path[PATH_BUF_SIZE];
    snprintf(log_path, sizeof(log_path), "%s/%s", project_dir, HUB_LOG);

    if (!file_exists(log_path)) {
        printf("(no log entries)\n");
        return EXIT_SUCCESS_CODE;
    }

    printf("Log (last %d):\n", n);

    char cmd[PATH_BUF_SIZE];
    snprintf(cmd, sizeof(cmd), "tail -n %d '%s'", n, log_path);
    (void)system(cmd);

    return EXIT_SUCCESS_CODE;
}

void cmd_help(void)
{
    printf(
        "nbs-hub — Deterministic process enforcement for AI supervisors\n"
        "\n"
        "Usage: nbs-hub [--project <path>] <command> [args...]\n"
        "\n"
        "Commands:\n"
        "  init <project-dir> <goal>         Initialise hub\n"
        "  status                            Full project state dump\n"
        "  spawn <slug> <task-desc>          Spawn worker (audit-gated)\n"
        "  check <worker-name>               Check worker status\n"
        "  result <worker-name>              Read results (increments audit)\n"
        "  dismiss <worker-name>             Kill and dismiss worker\n"
        "  list                              List all workers\n"
        "  audit <file>                      Submit self-check audit\n"
        "  gate <phase> <tests> <audit>      Submit phase gate\n"
        "  phase                             Show current phase\n"
        "  phase-name <name>                 Set current phase name\n"
        "  doc register <name> <path>        Register a document\n"
        "  doc list                          List registered documents\n"
        "  doc read <name>                   Read a registered document\n"
        "  decision <text>                   Record a decision\n"
        "  log [n]                           Show last n log entries\n"
        "  help                              Show this help\n"
        "\n"
        "Exit codes:\n"
        "  0  Success\n"
        "  1  Validation error\n"
        "  2  Hub not found / document not registered\n"
        "  3  Spawn refused — audit required\n"
        "  4  Usage error\n"
    );
}
