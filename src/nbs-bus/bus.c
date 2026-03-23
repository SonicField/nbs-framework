/*
 * bus.c — NBS Bus event queue operations
 *
 * All operations are on a directory (.nbs/events/). Events are individual
 * YAML files. Publishing is atomic (write-temp, rename). Acknowledging
 * is atomic (rename to processed/). No locking needed for publish because
 * each event creates a new unique file.
 */

#include "bus.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Priority helpers                                                    */
/* ------------------------------------------------------------------ */

static const char *priority_names[] = {"critical", "high", "normal", "low"};
#define BUS_NUM_PRIORITIES (sizeof(priority_names) / sizeof(priority_names[0]))

int bus_priority_from_str(const char *s)
{
    ASSERT_MSG(s != NULL, "bus_priority_from_str: s is NULL");
    for (size_t i = 0; i < BUS_NUM_PRIORITIES; i++) {
        if (strcmp(s, priority_names[i]) == 0)
            return (int)i;
    }
    return -1;
}

const char *bus_priority_to_str(int p)
{
    ASSERT_MSG(p >= 0 && (size_t)p < BUS_NUM_PRIORITIES,
               "bus_priority_to_str: invalid priority %d (max %zu)",
               p, BUS_NUM_PRIORITIES - 1);
    return priority_names[p];
}

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

/* Get current time as microseconds since epoch */
static long long now_us(void)
{
    struct timeval tv;
    int rc = gettimeofday(&tv, NULL);
    ASSERT_MSG(rc == 0, "now_us: gettimeofday failed: %s", strerror(errno));
    return (long long)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* Format current time as ISO 8601 UTC */
static void format_iso8601(char *buf, size_t len)
{
    ASSERT_MSG(buf != NULL, "format_iso8601: buf is NULL");
    ASSERT_MSG(len >= 24, "format_iso8601: buf too small: %zu", len);

    time_t now = time(NULL);
    ASSERT_MSG(now != (time_t)-1, "format_iso8601: time() failed: %s", strerror(errno));
    struct tm tm;
    struct tm *gmrc = gmtime_r(&now, &tm);
    ASSERT_MSG(gmrc != NULL, "format_iso8601: gmtime_r failed for time_t %lld",
               (long long)now);
    size_t rc = strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
    ASSERT_MSG(rc != 0, "format_iso8601: strftime failed (buffer size %zu)", len);
}

/* Check if a string contains whitespace */
static int has_whitespace(const char *s)
{
    ASSERT_MSG(s != NULL, "has_whitespace: s is NULL");
    for (const char *p = s; *p; p++) {
        if (isspace((unsigned char)*p))
            return 1;
    }
    return 0;
}

/* Parse an event filename into bus_event_t.
 * Format: <timestamp-us>-<source>-<type>-<pid>.event
 * The timestamp is digits only. Source and type are separated by '-'.
 * Since source and type can contain '-', we parse as:
 *   - timestamp: leading digits up to first '-'
 *   - rest: everything after timestamp and '-' up to '.event'
 *   - source: first token of rest (up to first '-')
 *   - type: everything after source-'-' up to '.event'
 *
 * This means source cannot contain '-' but type can.
 * Actually, looking at doc examples: "parser-worker" is a source with '-'.
 * So we need a different split strategy.
 *
 * The filename format is: <timestamp>-<source>-<type>.event
 * where source can contain '-' (e.g. parser-worker) and type can
 * contain '-' (e.g. task-complete).
 *
 * Resolution: parse timestamp (digits up to first '-'), then parse
 * from the event file content for source/type. The filename is for
 * sorting; the content is authoritative.
 *
 * For check (listing), we only need priority from the file content
 * and timestamp from the filename. So we parse filename for timestamp
 * and read the priority line from the file.
 */
static int parse_event_filename_timestamp(const char *filename, long long *ts_us)
{
    ASSERT_MSG(filename != NULL, "parse_event_filename_timestamp: filename is NULL");
    ASSERT_MSG(ts_us != NULL, "parse_event_filename_timestamp: ts_us is NULL");

    /* Timestamp is the leading digits before first '-' */
    const char *dash = strchr(filename, '-');
    if (!dash || dash == filename)
        return -1;

    char ts_buf[32];
    size_t ts_len = (size_t)(dash - filename);
    if (ts_len >= sizeof(ts_buf))
        return -1;

    memcpy(ts_buf, filename, ts_len);
    ts_buf[ts_len] = '\0';

    /* Verify all digits */
    for (size_t i = 0; i < ts_len; i++) {
        if (!isdigit((unsigned char)ts_buf[i]))
            return -1;
    }

    char *endp;
    errno = 0;
    *ts_us = strtoll(ts_buf, &endp, 10);
    if (errno != 0 || *endp != '\0')
        return -1;

    ASSERT_MSG(*ts_us > 0, "parse_event_filename_timestamp: parsed non-positive timestamp: %lld", *ts_us);
    return 0;
}

/* Read the dedup-key from an event file's content (the "dedup-key: X" line).
 * Returns 0 on success, -1 if not found. key_buf is NUL-terminated. */
static int read_event_dedup_key(const char *filepath, char *key_buf, size_t key_len)
{
    ASSERT_MSG(filepath != NULL, "read_event_dedup_key: filepath is NULL");
    ASSERT_MSG(key_buf != NULL, "read_event_dedup_key: key_buf is NULL");
    ASSERT_MSG(key_len > 0, "read_event_dedup_key: key_len is 0");

    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        fprintf(stderr, "Warning: cannot read event file %s: %s\n",
                filepath, strerror(errno));
        return -1;
    }

    char line[512];
    int found = -1;

    while (fgets(line, sizeof(line), fp)) {
        /* H11: detect truncated lines — if no newline and not at EOF,
         * the line was longer than our buffer. Skip the remainder. */
        if (strchr(line, '\n') == NULL && !feof(fp)) {
            /* Consume the rest of this truncated line */
            int ch;
            while ((ch = fgetc(fp)) != EOF && ch != '\n')
                ;
            continue;
        }
        if (strncmp(line, "dedup-key: ", 11) == 0) {
            /* Trim trailing newline */
            char *nl = strchr(line + 11, '\n');
            if (nl) *nl = '\0';
            size_t klen = strlen(line + 11);
            if (klen >= key_len) klen = key_len - 1;
            memcpy(key_buf, line + 11, klen);
            key_buf[klen] = '\0';
            found = 0;
            break;
        }
    }

    if (ferror(fp)) {
        fprintf(stderr, "Warning: read error on event file: %s\n", filepath);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return found;
}

/* Read priority, source, and type from an event file in a single pass.
 * Populates the corresponding fields of the event struct.
 * source_buf and type_buf must be at least source_len / type_len bytes. */
static int read_event_fields(const char *filepath, int *priority,
                              char *source_buf, size_t source_len,
                              char *type_buf, size_t type_len)
{
    ASSERT_MSG(filepath != NULL, "read_event_fields: filepath is NULL");
    ASSERT_MSG(priority != NULL, "read_event_fields: priority is NULL");
    ASSERT_MSG(source_buf != NULL, "read_event_fields: source_buf is NULL");
    ASSERT_MSG(source_len > 0, "read_event_fields: source_len is 0");
    ASSERT_MSG(type_buf != NULL, "read_event_fields: type_buf is NULL");
    ASSERT_MSG(type_len > 0, "read_event_fields: type_len is 0");

    *priority = BUS_PRIORITY_NORMAL;
    source_buf[0] = '\0';
    type_buf[0] = '\0';

    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        fprintf(stderr, "Warning: cannot read event file %s: %s\n",
                filepath, strerror(errno));
        return -1;
    }

    char line[512];
    int found = 0; /* bitmask: 1=priority, 2=source, 4=type */

    while (fgets(line, sizeof(line), fp) && found != 7) {
        if (!(found & 1) && strncmp(line, "priority: ", 10) == 0) {
            char *nl = strchr(line + 10, '\n');
            if (nl) *nl = '\0';
            int p = bus_priority_from_str(line + 10);
            if (p >= 0) *priority = p;
            found |= 1;
        } else if (!(found & 2) && strncmp(line, "source: ", 8) == 0) {
            char *nl = strchr(line + 8, '\n');
            if (nl) *nl = '\0';
            size_t len = strlen(line + 8);
            if (len >= source_len) len = source_len - 1;
            memcpy(source_buf, line + 8, len);
            source_buf[len] = '\0';
            found |= 2;
        } else if (!(found & 4) && strncmp(line, "type: ", 6) == 0) {
            char *nl = strchr(line + 6, '\n');
            if (nl) *nl = '\0';
            size_t len = strlen(line + 6);
            if (len >= type_len) len = type_len - 1;
            memcpy(type_buf, line + 6, len);
            type_buf[len] = '\0';
            found |= 4;
        }
    }

    if (ferror(fp)) {
        fprintf(stderr, "Warning: read error on event file: %s\n", filepath);
        fclose(fp);
        return -1;
    }

    if (found != 7) {
        fprintf(stderr, "Warning: incomplete event file %s (found mask=%d)\n",
                filepath, found);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

/* Format an age string from microsecond delta into buf.
 * Produces: "0s ago", "45s ago", "3m ago", "2h ago", "5d ago". */
static void format_age(long long delta_us, char *buf, size_t len)
{
    ASSERT_MSG(buf != NULL, "format_age: buf is NULL");
    ASSERT_MSG(len >= 16, "format_age: buf too small: %zu", len);

    long long seconds = delta_us / 1000000;
    if (seconds < 0) seconds = 0;

    int n;
    if (seconds < 60)
        n = snprintf(buf, len, "%llds ago", seconds);
    else if (seconds < 3600)
        n = snprintf(buf, len, "%lldm ago", seconds / 60);
    else if (seconds < 86400)
        n = snprintf(buf, len, "%lldh ago", seconds / 3600);
    else
        n = snprintf(buf, len, "%lldd ago", seconds / 86400);

    ASSERT_MSG(n >= 0 && (size_t)n < len,
               "format_age: output truncated (wrote %d, buf size %zu)", n, len);
}

/* Comparison function for sorting events: by priority (asc), then timestamp (asc) */
static int event_compare(const void *a, const void *b)
{
    ASSERT_MSG(a != NULL, "event_compare: a is NULL");
    ASSERT_MSG(b != NULL, "event_compare: b is NULL");
    const bus_event_t *ea = (const bus_event_t *)a;
    const bus_event_t *eb = (const bus_event_t *)b;

    if (ea->priority != eb->priority)
        return ea->priority - eb->priority;

    if (ea->timestamp_us < eb->timestamp_us) return -1;
    if (ea->timestamp_us > eb->timestamp_us) return  1;
    return 0;
}

/* Scan events directory and populate array. Returns count, -1 on error. */
static int scan_events(const char *events_dir, bus_event_t *events, int max_events)
{
    ASSERT_MSG(events_dir != NULL, "scan_events: events_dir is NULL");
    ASSERT_MSG(events != NULL, "scan_events: events is NULL");
    ASSERT_MSG(max_events > 0, "scan_events: max_events <= 0: %d", max_events);

    DIR *dir = opendir(events_dir);
    if (!dir) {
        fprintf(stderr, "Error: cannot open events directory %s: %s\n",
                events_dir, strerror(errno));
        return -1;
    }

    int count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL && count < max_events) {
        /* Skip non-.event files */
        const char *name = entry->d_name;
        size_t nlen = strlen(name);
        if (nlen < 7 || strcmp(name + nlen - 6, ".event") != 0)
            continue;

        /* Skip directories (e.g. processed/) */
        char fullpath[BUS_MAX_FULLPATH];
        int n_fullpath = snprintf(fullpath, sizeof(fullpath), "%s/%s", events_dir, name);
        ASSERT_MSG(n_fullpath >= 0 && (size_t)n_fullpath < sizeof(fullpath),
                   "bus_list_events: path truncated for %s/%s", events_dir, name);
        struct stat st;
        if (stat(fullpath, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        /* Parse timestamp from filename */
        long long ts_us;
        if (parse_event_filename_timestamp(name, &ts_us) != 0) {
            fprintf(stderr, "Warning: skipping malformed event filename: %s\n", name);
            continue;
        }

        /* Read priority, source, and type from file content */
        bus_event_t *ev = &events[count];
        if (read_event_fields(fullpath, &ev->priority,
                              ev->source, sizeof(ev->source),
                              ev->type, sizeof(ev->type)) != 0) {
            fprintf(stderr, "Warning: skipping unreadable event file: %s\n", fullpath);
            continue;
        }

        int n_fn = snprintf(ev->filename, sizeof(ev->filename), "%s", name);
        ASSERT_MSG(n_fn >= 0 && (size_t)n_fn < sizeof(ev->filename),
                   "scan_events: filename truncated for %s", name);
        ev->timestamp_us = ts_us;
        count++;
    }

    /* B4: warn if we hit the event limit — events beyond max_events are silently dropped */
    if (count == max_events) {
        /* Check if there are more .event files we couldn't store */
        while ((entry = readdir(dir)) != NULL) {
            const char *extra_name = entry->d_name;
            size_t extra_nlen = strlen(extra_name);
            if (extra_nlen >= 7 && strcmp(extra_name + extra_nlen - 6, ".event") == 0) {
                fprintf(stderr, "Warning: event limit reached (%d), some events were not listed\n",
                        max_events);
                break;
            }
        }
    }

    closedir(dir);
    return count;
}

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

int bus_load_config(const char *events_dir, bus_config_t *cfg)
{
    ASSERT_MSG(events_dir != NULL, "bus_load_config: events_dir is NULL");
    ASSERT_MSG(cfg != NULL, "bus_load_config: cfg is NULL");

    /* Set defaults */
    cfg->retention_max_bytes = BUS_DEFAULT_MAX_BYTES;
    cfg->dedup_window_s = BUS_DEFAULT_DEDUP_WINDOW;
    cfg->ack_timeout_s = BUS_DEFAULT_ACK_TIMEOUT;

    /* Try to open config.yaml */
    char config_path[BUS_MAX_FULLPATH];
    int n_config = snprintf(config_path, sizeof(config_path), "%s/config.yaml", events_dir);
    ASSERT_MSG(n_config >= 0 && (size_t)n_config < sizeof(config_path),
               "bus_init: path truncated for %s/config.yaml", events_dir);

    FILE *fp = fopen(config_path, "r");
    if (!fp) return 0; /* missing config is fine — use defaults */

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        /* Skip comments and empty lines */
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;

        /* Find the colon separator */
        char *colon = strchr(line, ':');
        if (!colon) continue;

        /* Extract key (trim trailing whitespace) */
        size_t key_len = (size_t)(colon - line);
        while (key_len > 0 && isspace((unsigned char)line[key_len - 1]))
            key_len--;

        /* Extract value (skip leading whitespace, trim trailing) */
        char *val = colon + 1;
        while (*val && isspace((unsigned char)*val))
            val++;
        /* B5/H8: cache strlen, trim trailing whitespace, THEN check empty */
        size_t val_len = strlen(val);
        if (val_len > 0) {
            char *end = val + val_len - 1;
            while (end > val && isspace((unsigned char)*end))
                *end-- = '\0';
        }
        if (val[0] == '\0') continue;

        /* Match keys */
        if (key_len == 19 && strncmp(line, "retention-max-bytes", 19) == 0) {
            char *endp;
            errno = 0;
            long long v = strtoll(val, &endp, 10);
            if (errno == 0 && *endp == '\0' && v > 0)
                cfg->retention_max_bytes = v;
        } else if (key_len == 12 && strncmp(line, "dedup-window", 12) == 0) {
            char *endp;
            errno = 0;
            long long v = strtoll(val, &endp, 10);
            if (errno == 0 && *endp == '\0' && v >= 0 &&
                v <= LLONG_MAX / 1000000LL)
                cfg->dedup_window_s = v;
        } else if (key_len == 11 && strncmp(line, "ack-timeout", 11) == 0) {
            char *endp;
            errno = 0;
            long long v = strtoll(val, &endp, 10);
            if (errno == 0 && *endp == '\0' && v >= 0 &&
                v <= LLONG_MAX / 1000000LL)
                cfg->ack_timeout_s = v;
        } else {
            /* HARDENING #17: warn on unknown config keys */
            char key_str[64];
            size_t kl = key_len < sizeof(key_str) - 1 ? key_len : sizeof(key_str) - 1;
            memcpy(key_str, line, kl);
            key_str[kl] = '\0';
            fprintf(stderr, "Warning: unknown config key '%s' in %s\n",
                    key_str, config_path);
        }
    }

    if (ferror(fp)) {
        fprintf(stderr, "Warning: read error on config file: %s\n", config_path);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Comparison for prune: sort by timestamp ascending (oldest first)    */
/* ------------------------------------------------------------------ */

typedef struct {
    char filename[256]; /* dirent filename, bounded by NAME_MAX */
    long long timestamp_us;
    off_t size;
} prune_entry_t;

static int prune_compare(const void *a, const void *b)
{
    ASSERT_MSG(a != NULL, "prune_compare: a is NULL");
    ASSERT_MSG(b != NULL, "prune_compare: b is NULL");
    const prune_entry_t *ea = (const prune_entry_t *)a;
    const prune_entry_t *eb = (const prune_entry_t *)b;
    if (ea->timestamp_us < eb->timestamp_us) return -1;
    if (ea->timestamp_us > eb->timestamp_us) return  1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

int bus_publish(const char *events_dir, const char *source, const char *type,
                int priority, const char *payload)
{
    ASSERT_MSG(events_dir != NULL, "bus_publish: events_dir is NULL");
    ASSERT_MSG(source != NULL, "bus_publish: source is NULL");
    ASSERT_MSG(type != NULL, "bus_publish: type is NULL");
    ASSERT_MSG(source[0] != '\0', "bus_publish: source is empty");
    ASSERT_MSG(type[0] != '\0', "bus_publish: type is empty");
    ASSERT_MSG(priority >= 0 && priority <= 3,
               "bus_publish: invalid priority %d", priority);

    /* Validate no whitespace in source/type — documented precondition */
    ASSERT_MSG(!has_whitespace(source),
               "bus_publish: source contains whitespace: '%s'", source);
    ASSERT_MSG(!has_whitespace(type),
               "bus_publish: type contains whitespace: '%s'", type);

    /* Verify events directory exists */
    struct stat st;
    if (stat(events_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: events directory not found: %s\n", events_dir);
        return -1;
    }

    /* Ensure processed/ subdirectory exists */
    char processed_dir[BUS_MAX_FULLPATH];
    int n_proc1 = snprintf(processed_dir, sizeof(processed_dir), "%s/processed", events_dir);
    ASSERT_MSG(n_proc1 >= 0 && (size_t)n_proc1 < sizeof(processed_dir),
               "bus_publish: path truncated for %s/processed", events_dir);
    if (stat(processed_dir, &st) != 0) {
        if (mkdir(processed_dir, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "Error: cannot create processed directory: %s\n",
                    strerror(errno));
            return -1;
        }
    }

    long long ts_us = now_us();

    /* Build filename — use microsecond timestamp for unique ordering.
     * Append PID to prevent collisions from concurrent processes
     * publishing the same source+type within the same microsecond.
     */
    char filename[BUS_MAX_FILENAME];
    int n_fn = snprintf(filename, sizeof(filename), "%lld-%s-%s-%d.event",
             ts_us, source, type, (int)getpid());
    ASSERT_MSG(n_fn >= 0 && (size_t)n_fn < sizeof(filename),
               "bus_publish: event filename truncated (source=%s, type=%s)", source, type);

    /* Build temp and final paths */
    char tmp_path[BUS_MAX_FULLPATH];
    char final_path[BUS_MAX_FULLPATH];
    int n_tmp = snprintf(tmp_path, sizeof(tmp_path), "%s/.tmp-%lld-%d.event",
             events_dir, ts_us, (int)getpid());
    ASSERT_MSG(n_tmp >= 0 && (size_t)n_tmp < sizeof(tmp_path),
               "bus_publish: path truncated for tmp file");
    int n_final = snprintf(final_path, sizeof(final_path), "%s/%s", events_dir, filename);
    ASSERT_MSG(n_final >= 0 && (size_t)n_final < sizeof(final_path),
               "bus_publish: path truncated for %s/%s", events_dir, filename);

    /* Format timestamp */
    char iso_time[32];
    format_iso8601(iso_time, sizeof(iso_time));

    /* Write event file to temp path */
    FILE *fp = fopen(tmp_path, "w");
    if (!fp) {
        fprintf(stderr, "Error: cannot create event file: %s\n", strerror(errno));
        return -1;
    }

    int write_err = 0;
    if (fprintf(fp, "source: %s\n", source) < 0) write_err = 1;
    if (!write_err && fprintf(fp, "type: %s\n", type) < 0) write_err = 1;
    if (!write_err && fprintf(fp, "priority: %s\n", bus_priority_to_str(priority)) < 0) write_err = 1;
    if (!write_err && fprintf(fp, "timestamp: %s\n", iso_time) < 0) write_err = 1;
    if (!write_err && fprintf(fp, "dedup-key: %s:%s\n", source, type) < 0) write_err = 1;

    if (!write_err && payload && payload[0] != '\0') {
        if (fprintf(fp, "payload: |\n") < 0) write_err = 1;
        /* Write payload with 2-space indent per YAML block scalar convention */
        const char *p = payload;
        while (!write_err && *p) {
            if (fprintf(fp, "  ") < 0) { write_err = 1; break; }
            const char *nl = strchr(p, '\n');
            if (nl) {
                if (fwrite(p, 1, (size_t)(nl - p + 1), fp) != (size_t)(nl - p + 1))
                    write_err = 1;
                p = nl + 1;
            } else {
                if (fprintf(fp, "%s\n", p) < 0) write_err = 1;
                break;
            }
        }
    }

    /* H2: check write errors BEFORE fclose using fflush+ferror,
     * so we detect errors while the FILE* is still valid. */
    if (!write_err) {
        if (fflush(fp) != 0 || ferror(fp))
            write_err = 1;
    }

    if (fclose(fp) != 0)
        write_err = 1;

    if (write_err) {
        if (unlink(tmp_path) != 0)
            fprintf(stderr, "Warning: failed to remove temp file %s: %s\n",
                    tmp_path, strerror(errno));
        fprintf(stderr, "Error: write error creating event file: %s\n", strerror(errno));
        return -1;
    }

    /* Atomic rename */
    if (rename(tmp_path, final_path) != 0) {
        if (unlink(tmp_path) != 0)
            fprintf(stderr, "Warning: failed to remove temp file %s: %s\n",
                    tmp_path, strerror(errno));
        fprintf(stderr, "Error: failed to finalise event file: %s\n",
                strerror(errno));
        return -1;
    }

    /* Print the filename to stdout */
    if (printf("%s\n", filename) < 0) {
        fprintf(stderr, "Error: failed to write filename to stdout\n");
        return -1;
    }
    return 0;
}

int bus_check(const char *events_dir, const char *handle)
{
    ASSERT_MSG(events_dir != NULL, "bus_check: events_dir is NULL");

    struct stat st;
    if (stat(events_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: events directory not found: %s\n", events_dir);
        return -1;
    }

    /* ~196 KB stack allocation (sizeof(bus_event_t) * 256). */
    bus_event_t events[BUS_MAX_EVENTS];
    int count = scan_events(events_dir, events, BUS_MAX_EVENTS);
    if (count < 0) {
        fprintf(stderr, "Error: cannot scan events directory: %s\n", events_dir);
        return -1;
    }

    if (count == 0)
        return 0;

    /* Sort by priority then timestamp */
    qsort(events, (size_t)count, sizeof(bus_event_t), event_compare);

    /* Print results, optionally filtered by source handle */
    long long current_us = now_us();
    for (int i = 0; i < count; i++) {
        if (handle && handle[0] != '\0' &&
            strcmp(events[i].source, handle) != 0)
            continue;
        char age[32];
        format_age(current_us - events[i].timestamp_us, age, sizeof(age));
        if (printf("[%s] %s (%s)\n",
               bus_priority_to_str(events[i].priority),
               events[i].filename,
               age) < 0) {
            fprintf(stderr, "Error: failed to write event listing to stdout\n");
            return -1;
        }
    }

    return 0;
}

/* SECURITY: shared path traversal and suffix validation for event filenames.
 * Checks: non-empty, no '/', no ".." prefix, no ".", .event suffix, length limit.
 * Returns 0 if valid, -1 if rejected.
 * NOTE: logic is kept consistent with validate_event_filename in main.c (S6). */
static int validate_event_filename(const char *event_file)
{
    ASSERT_MSG(event_file != NULL, "validate_event_filename: event_file is NULL");

    if (event_file[0] == '\0') {
        fprintf(stderr, "Error: invalid event filename (empty)\n");
        return -1;
    }

    if (strchr(event_file, '/') != NULL ||
        strncmp(event_file, "..", 2) == 0 || strcmp(event_file, ".") == 0) {
        fprintf(stderr, "Error: invalid event filename (path traversal): %s\n",
                event_file);
        return -1;
    }

    size_t elen = strlen(event_file);
    if (elen >= BUS_MAX_FILENAME) {
        fprintf(stderr, "Error: event filename too long (%zu >= %d): %s\n",
                elen, BUS_MAX_FILENAME, event_file);
        return -1;
    }

    if (elen < 7 || strcmp(event_file + elen - 6, ".event") != 0) {
        fprintf(stderr, "Error: invalid event filename (must end in .event): %s\n",
                event_file);
        return -1;
    }

    return 0;
}

int bus_read(const char *events_dir, const char *event_file)
{
    ASSERT_MSG(events_dir != NULL, "bus_read: events_dir is NULL");
    ASSERT_MSG(event_file != NULL, "bus_read: event_file is NULL");

    if (validate_event_filename(event_file) != 0)
        return -1;

    char filepath[BUS_MAX_FULLPATH];
    int n_filepath = snprintf(filepath, sizeof(filepath), "%s/%s", events_dir, event_file);
    ASSERT_MSG(n_filepath >= 0 && (size_t)n_filepath < sizeof(filepath),
               "bus_get_event: path truncated for %s/%s", events_dir, event_file);

    FILE *fp = fopen(filepath, "r");
    if (!fp) {
        fprintf(stderr, "Error: event not found: %s\n", event_file);
        return -2;
    }

    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
        if (fwrite(buf, 1, n, stdout) != n) {
            fprintf(stderr, "Error: failed to write event to stdout\n");
            fclose(fp);
            return -1;
        }
    }

    if (ferror(fp)) {
        fprintf(stderr, "Error: read error on event file: %s\n", event_file);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    return 0;
}

int bus_ack(const char *events_dir, const char *event_file)
{
    ASSERT_MSG(events_dir != NULL, "bus_ack: events_dir is NULL");
    ASSERT_MSG(event_file != NULL, "bus_ack: event_file is NULL");

    if (validate_event_filename(event_file) != 0)
        return -1;

    char src_path[BUS_MAX_FULLPATH];
    char dst_path[BUS_MAX_FULLPATH];
    int n_src = snprintf(src_path, sizeof(src_path), "%s/%s", events_dir, event_file);
    ASSERT_MSG(n_src >= 0 && (size_t)n_src < sizeof(src_path),
               "bus_ack: path truncated for %s/%s", events_dir, event_file);
    int n_dst = snprintf(dst_path, sizeof(dst_path), "%s/processed/%s",
             events_dir, event_file);
    ASSERT_MSG(n_dst >= 0 && (size_t)n_dst < sizeof(dst_path),
               "bus_ack: path truncated for %s/processed/%s", events_dir, event_file);

    /* Ensure processed/ exists */
    char processed_dir[BUS_MAX_FULLPATH];
    int n_proc2 = snprintf(processed_dir, sizeof(processed_dir), "%s/processed", events_dir);
    ASSERT_MSG(n_proc2 >= 0 && (size_t)n_proc2 < sizeof(processed_dir),
               "bus_ack: path truncated for %s/processed", events_dir);
    struct stat st;
    if (stat(processed_dir, &st) != 0) {
        if (mkdir(processed_dir, 0755) != 0 && errno != EEXIST) {
            fprintf(stderr, "Error: cannot create processed directory: %s\n",
                    strerror(errno));
            return -1;
        }
    }

    /* Check source exists */
    if (stat(src_path, &st) != 0) {
        fprintf(stderr, "Error: event not found: %s\n", event_file);
        return -2;
    }

    if (rename(src_path, dst_path) != 0) {
        fprintf(stderr, "Error: cannot acknowledge event: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

int bus_ack_all(const char *events_dir, const char *handle)
{
    ASSERT_MSG(events_dir != NULL, "bus_ack_all: events_dir is NULL");

    struct stat st;
    if (stat(events_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: events directory not found: %s\n", events_dir);
        return -1;
    }

    /* ~196 KB stack allocation (sizeof(bus_event_t) * 256). */
    bus_event_t events[BUS_MAX_EVENTS];
    int count = scan_events(events_dir, events, BUS_MAX_EVENTS);
    if (count < 0) {
        fprintf(stderr, "Error: cannot scan events directory\n");
        return -1;
    }

    int acked = 0;
    for (int i = 0; i < count; i++) {
        if (handle && handle[0] != '\0' &&
            strcmp(events[i].source, handle) != 0)
            continue;
        if (bus_ack(events_dir, events[i].filename) == 0)
            acked++;
    }

    if (printf("Acknowledged %d event%s\n", acked, acked == 1 ? "" : "s") < 0) {
        fprintf(stderr, "Error: failed to write ack count to stdout\n");
        return -1;
    }
    return 0;
}

int bus_prune(const char *events_dir, long long max_bytes)
{
    ASSERT_MSG(events_dir != NULL, "bus_prune: events_dir is NULL");
    ASSERT_MSG(max_bytes > 0, "bus_prune: max_bytes <= 0: %lld", max_bytes);

    char processed_dir[BUS_MAX_FULLPATH];
    int n_proc3 = snprintf(processed_dir, sizeof(processed_dir), "%s/processed", events_dir);
    ASSERT_MSG(n_proc3 >= 0 && (size_t)n_proc3 < sizeof(processed_dir),
               "bus_prune: path truncated for %s/processed", events_dir);

    struct stat dir_st;
    if (stat(processed_dir, &dir_st) != 0) {
        /* No processed directory — nothing to prune */
        if (printf("Pruned 0 events (no processed directory)\n") < 0) {
            fprintf(stderr, "Error: failed to write prune status to stdout\n");
            return -1;
        }
        return 0;
    }

    /* Scan processed directory */
    DIR *dir = opendir(processed_dir);
    if (!dir) {
        fprintf(stderr, "Error: cannot open processed directory: %s\n",
                strerror(errno));
        return -1;
    }

    prune_entry_t entries[BUS_MAX_EVENTS];
    int count = 0;
    long long total_size = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL && count < BUS_MAX_EVENTS) {
        const char *name = entry->d_name;
        size_t nlen = strlen(name);
        if (nlen < 7 || strcmp(name + nlen - 6, ".event") != 0)
            continue;

        char fullpath[BUS_MAX_FULLPATH];
        int n_fullpath2 = snprintf(fullpath, sizeof(fullpath), "%s/processed/%s", events_dir, name);
        ASSERT_MSG(n_fullpath2 >= 0 && (size_t)n_fullpath2 < sizeof(fullpath),
                   "bus_prune: path truncated for %s/processed/%s", events_dir, name);

        struct stat st;
        if (stat(fullpath, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        long long ts_us;
        if (parse_event_filename_timestamp(name, &ts_us) != 0)
            continue;

        prune_entry_t *pe = &entries[count];
        int n_pefn = snprintf(pe->filename, sizeof(pe->filename), "%s", name);
        ASSERT_MSG(n_pefn >= 0 && (size_t)n_pefn < sizeof(pe->filename),
                   "bus_prune: filename truncated for %s", name);
        pe->timestamp_us = ts_us;
        pe->size = st.st_size;
        /* H9: overflow check before accumulating total_size */
        ASSERT_MSG(total_size <= LLONG_MAX - (long long)st.st_size,
                   "bus_prune: total_size overflow at %lld + %lld",
                   total_size, (long long)st.st_size);
        total_size += st.st_size;
        count++;
    }

    closedir(dir);

    if (total_size <= max_bytes) {
        if (printf("Pruned 0 events (%.1f KB / %.1f KB limit)\n",
               (double)total_size / 1024.0,
               (double)max_bytes / 1024.0) < 0) {
            fprintf(stderr, "Error: failed to write prune status to stdout\n");
            return -1;
        }
        return 0;
    }

    /* Sort oldest first */
    qsort(entries, (size_t)count, sizeof(prune_entry_t), prune_compare);

    /* Delete oldest until we're under the limit */
    int pruned = 0;
    for (int i = 0; i < count && total_size > max_bytes; i++) {
        char del_path[BUS_MAX_FULLPATH];
        int n_del = snprintf(del_path, sizeof(del_path), "%s/processed/%s",
                 events_dir, entries[i].filename);
        ASSERT_MSG(n_del >= 0 && (size_t)n_del < sizeof(del_path),
                   "bus_prune: path truncated for %s/processed/%s", events_dir, entries[i].filename);
        if (unlink(del_path) == 0) {
            total_size -= entries[i].size;
            pruned++;
        } else {
            fprintf(stderr, "Warning: failed to prune %s: %s\n",
                    entries[i].filename, strerror(errno));
        }
    }

    if (printf("Pruned %d event%s (%.1f KB remaining, %.1f KB limit)\n",
           pruned, pruned == 1 ? "" : "s",
           (double)total_size / 1024.0,
           (double)max_bytes / 1024.0) < 0) {
        fprintf(stderr, "Error: failed to write prune status to stdout\n");
        return -1;
    }
    return 0;
}

int bus_status(const char *events_dir)
{
    ASSERT_MSG(events_dir != NULL, "bus_status: events_dir is NULL");

    struct stat st;
    if (stat(events_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "Error: events directory not found: %s\n", events_dir);
        return -1;
    }

    /* Count pending events by priority */
    /* ~196 KB stack allocation (sizeof(bus_event_t) * 256). */
    bus_event_t events[BUS_MAX_EVENTS];
    int count = scan_events(events_dir, events, BUS_MAX_EVENTS);
    if (count < 0) {
        fprintf(stderr, "Error: cannot scan events directory\n");
        return -1;
    }

    int priority_counts[4] = {0, 0, 0, 0};
    long long oldest_ts = 0;
    for (int i = 0; i < count; i++) {
        ASSERT_MSG(events[i].priority >= 0 && events[i].priority <= 3,
                   "bus_status: bad priority %d in event %s",
                   events[i].priority, events[i].filename);
        priority_counts[events[i].priority]++;
        if (oldest_ts == 0 || events[i].timestamp_us < oldest_ts)
            oldest_ts = events[i].timestamp_us;
    }

    /* Count processed events and total size */
    char processed_dir[BUS_MAX_FULLPATH];
    int n_proc4 = snprintf(processed_dir, sizeof(processed_dir), "%s/processed", events_dir);
    ASSERT_MSG(n_proc4 >= 0 && (size_t)n_proc4 < sizeof(processed_dir),
               "bus_status: path truncated for %s/processed", events_dir);
    int processed_count = 0;
    long long processed_size = 0;

    DIR *dir = opendir(processed_dir);
    if (dir) {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL) {
            const char *name = entry->d_name;
            size_t nlen = strlen(name);
            if (nlen < 7 || strcmp(name + nlen - 6, ".event") != 0)
                continue;

            char fullpath[BUS_MAX_FULLPATH];
            int n_fullpath3 = snprintf(fullpath, sizeof(fullpath), "%s/processed/%s", events_dir, name);
            ASSERT_MSG(n_fullpath3 >= 0 && (size_t)n_fullpath3 < sizeof(fullpath),
                       "bus_status: path truncated for %s/processed/%s", events_dir, name);
            struct stat fst;
            if (stat(fullpath, &fst) == 0 && S_ISREG(fst.st_mode)) {
                processed_count++;
                /* H10: overflow check before accumulating processed_size */
                ASSERT_MSG(processed_size <= LLONG_MAX - (long long)fst.st_size,
                           "bus_status: processed_size overflow at %lld + %lld",
                           processed_size, (long long)fst.st_size);
                processed_size += fst.st_size;
            }
        }
        closedir(dir);
    }

    if (printf("Pending: %d total", count) < 0) {
        fprintf(stderr, "Error: failed to write status to stdout\n");
        return -1;
    }
    if (count > 0) {
        if (printf(" (critical=%d, high=%d, normal=%d, low=%d)",
               priority_counts[0], priority_counts[1],
               priority_counts[2], priority_counts[3]) < 0) {
            fprintf(stderr, "Error: failed to write status to stdout\n");
            return -1;
        }
    }
    if (printf("\n") < 0) {
        fprintf(stderr, "Error: failed to write status to stdout\n");
        return -1;
    }

    if (oldest_ts > 0) {
        time_t oldest_sec = (time_t)(oldest_ts / 1000000);
        struct tm tm;
        char oldest_str[32];
        struct tm *gmrc2 = gmtime_r(&oldest_sec, &tm);
        ASSERT_MSG(gmrc2 != NULL, "bus_status: gmtime_r failed for time_t %lld",
                   (long long)oldest_sec);
        size_t sfrc = strftime(oldest_str, sizeof(oldest_str), "%Y-%m-%dT%H:%M:%SZ", &tm);
        ASSERT_MSG(sfrc != 0, "bus_status: strftime failed (buffer size %zu)",
                   sizeof(oldest_str));
        if (printf("Oldest pending: %s\n", oldest_str) < 0) {
            fprintf(stderr, "Error: failed to write status to stdout\n");
            return -1;
        }
    }

    if (printf("Processed: %d events (%.1f KB)\n",
           processed_count, (double)processed_size / 1024.0) < 0) {
        fprintf(stderr, "Error: failed to write status to stdout\n");
        return -1;
    }

    /* Check for stale events if ack-timeout is configured */
    bus_config_t cfg;
    int cfg_rc = bus_load_config(events_dir, &cfg);
    if (cfg_rc != 0) {
        fprintf(stderr, "Warning: failed to load config from %s, skipping stale event check\n",
                events_dir);
        return 0;
    }
    if (cfg.ack_timeout_s > 0 && count > 0) {
        ASSERT_MSG(cfg.ack_timeout_s <= LLONG_MAX / 1000000LL,
                   "bus_status: ack_timeout_s too large for us conversion: %lld",
                   cfg.ack_timeout_s);
        long long current_us = now_us();
        long long timeout_us = cfg.ack_timeout_s * 1000000LL;
        int stale = 0;
        for (int i = 0; i < count; i++) {
            if (current_us - events[i].timestamp_us > timeout_us)
                stale++;
        }
        if (stale > 0) {
            if (printf("WARNING: %d stale event%s (unacked > %llds)\n",
                   stale, stale == 1 ? "" : "s", cfg.ack_timeout_s) < 0) {
                fprintf(stderr, "Error: failed to write status to stdout\n");
                return -1;
            }
        }
    }

    return 0;
}

int bus_publish_dedup(const char *events_dir, const char *source,
                      const char *type, int priority, const char *payload,
                      long long dedup_window_us)
{
    ASSERT_MSG(events_dir != NULL, "bus_publish_dedup: events_dir is NULL");
    ASSERT_MSG(source != NULL, "bus_publish_dedup: source is NULL");
    ASSERT_MSG(type != NULL, "bus_publish_dedup: type is NULL");
    ASSERT_MSG(source[0] != '\0', "bus_publish_dedup: source is empty");
    ASSERT_MSG(type[0] != '\0', "bus_publish_dedup: type is empty");
    ASSERT_MSG(priority >= 0 && priority <= 3,
               "bus_publish_dedup: invalid priority %d", priority);
    ASSERT_MSG(dedup_window_us > 0,
               "bus_publish_dedup: dedup_window_us <= 0: %lld", dedup_window_us);

    /* Build the dedup key for this proposed event */
    char proposed_key[BUS_MAX_HANDLE + BUS_MAX_TYPE + 2];
    int n_key = snprintf(proposed_key, sizeof(proposed_key), "%s:%s", source, type);
    ASSERT_MSG(n_key >= 0 && (size_t)n_key < sizeof(proposed_key),
               "bus_publish_dedup: dedup key truncated for %s:%s", source, type);

    long long current_us = now_us();
    long long cutoff_us = current_us - dedup_window_us;
    if (cutoff_us < 0) cutoff_us = 0; /* Clamp: avoid underflow when window > current time */

    /* Scan pending events directory for duplicates */
    DIR *dir = opendir(events_dir);
    if (!dir) {
        /* Directory doesn't exist — proceed to publish (it will fail there) */
        return bus_publish(events_dir, source, type, priority, payload);
    }

    struct dirent *entry;
    int duplicate_found = 0;

    while ((entry = readdir(dir)) != NULL) {
        const char *name = entry->d_name;
        size_t nlen = strlen(name);
        if (nlen < 7 || strcmp(name + nlen - 6, ".event") != 0)
            continue;

        /* Parse timestamp from filename */
        long long ts_us;
        if (parse_event_filename_timestamp(name, &ts_us) != 0)
            continue;

        /* Skip events older than the dedup window */
        if (ts_us < cutoff_us)
            continue;

        /* Read dedup-key from file content */
        char fullpath[BUS_MAX_FULLPATH];
        int n_fullpath4 = snprintf(fullpath, sizeof(fullpath), "%s/%s", events_dir, name);
        ASSERT_MSG(n_fullpath4 >= 0 && (size_t)n_fullpath4 < sizeof(fullpath),
                   "bus_is_duplicate: path truncated for %s/%s", events_dir, name);

        /* Skip directories (e.g. processed/) */
        struct stat st;
        if (stat(fullpath, &st) != 0 || !S_ISREG(st.st_mode))
            continue;

        char existing_key[BUS_MAX_HANDLE + BUS_MAX_TYPE + 2];
        if (read_event_dedup_key(fullpath, existing_key, sizeof(existing_key)) != 0)
            continue;

        if (strcmp(proposed_key, existing_key) == 0) {
            duplicate_found = 1;
            break;
        }
    }

    closedir(dir);

    if (duplicate_found) {
        fprintf(stderr, "Dedup: event %s dropped (duplicate within window)\n",
                proposed_key);
        return BUS_EXIT_DEDUP;
    }

    return bus_publish(events_dir, source, type, priority, payload);
}
