/*
 * nbs_helper_check.c — Check if nbs-ts-helper is running.
 */

#include "nbs_helper_check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int helper_is_running(void)
{
    const char *home = getenv("HOME");
    if (!home)
        return 0;

    char sock_path[256];
    int n = snprintf(sock_path, sizeof(sock_path),
                     "%s/.nbs-ts/helper.sock", home);
    if (n < 0 || (size_t)n >= sizeof(sock_path))
        return 0;

    struct stat st;
    return stat(sock_path, &st) == 0;
}

int helper_warn_if_not_running(FILE *out, int verbose)
{
    if (helper_is_running())
        return 0;

    if (verbose) {
        fprintf(out,
            "Warning: nbs-ts-helper is not running.\n"
            "  SSH, proxy access, and git push will not work.\n"
            "  Run: nbs-ts-sysctl install    (automatic start on login)\n"
            "  Or:  nbs-ts-helper             (manual, this session only)\n");
    } else {
        fprintf(out,
            "nbs-ts: helper not running — using direct fork "
            "(some operations may be restricted)\n"
            "  Run: nbs-ts-sysctl install    (automatic start on login)\n"
            "  Or:  nbs-ts-helper             (manual, this session only)\n");
    }

    return 1;
}
