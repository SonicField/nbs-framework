/*
 * dashboard.h — NBS team dashboard public API.
 *
 * Provides a full-screen TUI showing all 7 agents with live status,
 * sidecar health, cursor position, and drill-down into agent terminal
 * output via nbs-ts-render.
 */

#ifndef NBS_DASHBOARD_H
#define NBS_DASHBOARD_H

typedef struct dashboard dashboard_t;

/*
 * Create a dashboard for the given project root.
 * nbs_root must contain a .nbs/ directory with chat file and project-id.
 * Returns NULL on failure.
 */
dashboard_t *dashboard_init(const char *nbs_root);

/*
 * Run the dashboard event loop.  Blocks until the user exits.
 * Enters alternate screen and raw terminal mode; restores on exit.
 */
void dashboard_run(dashboard_t *d);

/*
 * Free all resources.
 */
void dashboard_free(dashboard_t *d);

#endif /* NBS_DASHBOARD_H */
