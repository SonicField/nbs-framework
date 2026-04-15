/*
 * test_file_browser_unit.c — Unit tests for nbs-file-browser long mode.
 *
 * Tests the formatting functions for permissions and modification time
 * that are used in long mode display.
 *
 * Build:
 *   gcc -Wall -Wextra -Werror -std=c11 -D_POSIX_C_SOURCE=200809L \
 *       -D_DEFAULT_SOURCE -O2 -o test_file_browser_unit test_file_browser_unit.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(label, cond) do { \
    if (cond) { printf("   PASS: %s\n", label); g_pass++; } \
    else { printf("   FAIL: %s (line %d)\n", label, __LINE__); g_fail++; } \
} while(0)

/* --- Functions under test (copied from main.c) --- */

/*
 * format_perms — Render a mode_t as a 10-character rwx string.
 * Example: drwxr-xr-x, -rw-r--r--
 */
static void format_perms(mode_t mode, char *buf, size_t bufsz) {
    if (bufsz < 11) { buf[0] = '\0'; return; }

    buf[0] = S_ISDIR(mode) ? 'd' : S_ISLNK(mode) ? 'l' : '-';
    buf[1] = (mode & S_IRUSR) ? 'r' : '-';
    buf[2] = (mode & S_IWUSR) ? 'w' : '-';
    buf[3] = (mode & S_IXUSR) ? 'x' : '-';
    buf[4] = (mode & S_IRGRP) ? 'r' : '-';
    buf[5] = (mode & S_IWGRP) ? 'w' : '-';
    buf[6] = (mode & S_IXGRP) ? 'x' : '-';
    buf[7] = (mode & S_IROTH) ? 'r' : '-';
    buf[8] = (mode & S_IWOTH) ? 'w' : '-';
    buf[9] = (mode & S_IXOTH) ? 'x' : '-';
    buf[10] = '\0';
}

/*
 * format_mtime — Render a modification time as "YYYY-MM-DD HH:MM".
 */
static void format_mtime(time_t mtime, char *buf, size_t bufsz) {
    if (bufsz < 17) { buf[0] = '\0'; return; }
    struct tm tm_buf;
    struct tm *tm = localtime_r(&mtime, &tm_buf);
    if (tm) {
        strftime(buf, bufsz, "%Y-%m-%d %H:%M", tm);
    } else {
        snprintf(buf, bufsz, "----/--/-- --:--");
    }
}

/* --- Tests --- */

static void test_perms_regular_file(void) {
    printf("1. Regular file permissions...\n");
    char buf[16];
    format_perms(S_IFREG | 0644, buf, sizeof(buf));
    CHECK("rw-r--r--", strcmp(buf, "-rw-r--r--") == 0);
}

static void test_perms_directory(void) {
    printf("2. Directory permissions...\n");
    char buf[16];
    format_perms(S_IFDIR | 0755, buf, sizeof(buf));
    CHECK("drwxr-xr-x", strcmp(buf, "drwxr-xr-x") == 0);
}

static void test_perms_executable(void) {
    printf("3. Executable file permissions...\n");
    char buf[16];
    format_perms(S_IFREG | 0755, buf, sizeof(buf));
    CHECK("-rwxr-xr-x", strcmp(buf, "-rwxr-xr-x") == 0);
}

static void test_perms_no_access(void) {
    printf("4. No permissions...\n");
    char buf[16];
    format_perms(S_IFREG | 0000, buf, sizeof(buf));
    CHECK("----------", strcmp(buf, "----------") == 0);
}

static void test_perms_all_access(void) {
    printf("5. All permissions...\n");
    char buf[16];
    format_perms(S_IFREG | 0777, buf, sizeof(buf));
    CHECK("-rwxrwxrwx", strcmp(buf, "-rwxrwxrwx") == 0);
}

static void test_perms_small_buffer(void) {
    printf("6. Small buffer returns empty...\n");
    char buf[8];
    format_perms(S_IFREG | 0644, buf, sizeof(buf));
    CHECK("empty on small buf", buf[0] == '\0');
}

static void test_mtime_known_date(void) {
    printf("7. Known date formatting...\n");
    /* 2026-01-15 12:30:00 UTC */
    struct tm tm_val = {0};
    tm_val.tm_year = 126; /* 2026 */
    tm_val.tm_mon = 0;    /* January */
    tm_val.tm_mday = 15;
    tm_val.tm_hour = 12;
    tm_val.tm_min = 30;
    time_t t = mktime(&tm_val);
    char buf[32];
    format_mtime(t, buf, sizeof(buf));
    CHECK("contains 2026", strstr(buf, "2026") != NULL);
    CHECK("contains 01-15", strstr(buf, "01-15") != NULL);
    CHECK("length is 16", strlen(buf) == 16);
}

static void test_mtime_small_buffer(void) {
    printf("8. Small mtime buffer returns empty...\n");
    char buf[8];
    format_mtime(time(NULL), buf, sizeof(buf));
    CHECK("empty on small buf", buf[0] == '\0');
}

static void test_mtime_zero(void) {
    printf("9. Epoch time formats without crash...\n");
    char buf[32];
    format_mtime(0, buf, sizeof(buf));
    CHECK("epoch formats", strlen(buf) > 0);
}

int main(void) {
    printf("=== nbs-file-browser Long Mode Unit Tests ===\n\n");

    test_perms_regular_file();
    test_perms_directory();
    test_perms_executable();
    test_perms_no_access();
    test_perms_all_access();
    test_perms_small_buffer();
    test_mtime_known_date();
    test_mtime_small_buffer();
    test_mtime_zero();

    printf("\n=== Result ===\n");
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
