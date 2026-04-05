/*
 * main.c — Standalone entry point for the NBS team dashboard.
 *
 * Usage: nbs-dashboard <project-root>
 */

#include <stdio.h>
#include <stdlib.h>
#include "dashboard.h"

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: nbs-dashboard <project-root>\n");
        return 1;
    }

    dashboard_t *d = dashboard_init(argv[1]);
    if (!d) {
        fprintf(stderr, "Error: could not initialise dashboard for '%s'\n",
                argv[1]);
        return 1;
    }

    dashboard_run(d);
    dashboard_free(d);
    return 0;
}
