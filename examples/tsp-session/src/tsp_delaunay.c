/*
 * tsp_delaunay.c — Delaunay-constrained TSP experiment
 *
 * Tests the hypothesis: "For Euclidean TSP, the optimal tour uses
 * only edges from the Delaunay triangulation."
 *
 * Approach:
 *   1. Compute Delaunay triangulation (incremental with Bowyer-Watson)
 *   2. Find optimal Hamiltonian cycle within Delaunay graph (backtracking)
 *   3. Find true optimal tour by brute force (for small n)
 *   4. Compare — if they differ, the hypothesis is falsified
 *
 * Practical limit: brute-force comparison up to ~15 cities.
 *
 * Compile: gcc -O2 -Wall -o tsp_delaunay tsp_delaunay.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <float.h>

/* ── Data structures ──────────────────────────────────────────────── */

typedef struct {
    double x, y;
} Point;

/* Adjacency list for Delaunay graph. */
#define MAX_NEIGHBOURS 32

typedef struct {
    int adj[MAX_NEIGHBOURS];
    int deg;
} AdjList;

static int n;
static Point *pts;
static double *dist;       /* n×n distance matrix */
static AdjList *graph;     /* Delaunay adjacency */

#define D(i,j) dist[(i)*n+(j)]

/* ── Distance matrix ──────────────────────────────────────────────── */

static void build_dist(void)
{
    dist = malloc((size_t)n * (size_t)n * sizeof(double));
    for (int i = 0; i < n; i++) {
        dist[i*n+i] = 0.0;
        for (int j = i+1; j < n; j++) {
            double dx = pts[i].x - pts[j].x;
            double dy = pts[i].y - pts[j].y;
            double d = sqrt(dx*dx + dy*dy);
            dist[i*n+j] = d;
            dist[j*n+i] = d;
        }
    }
}

/* ── Delaunay triangulation (Bowyer-Watson) ───────────────────────── */

typedef struct {
    int v[3];       /* vertex indices (-1, -2, -3 for super-triangle) */
    int alive;
} Triangle;

static Triangle *tris;
static int ntris, tcap;

static void tri_add(int a, int b, int c)
{
    if (ntris >= tcap) {
        tcap = tcap ? tcap * 2 : 256;
        tris = realloc(tris, tcap * sizeof(Triangle));
    }
    tris[ntris++] = (Triangle){{a, b, c}, 1};
}

/* Super-triangle points (stored at indices -1, -2, -3). */
static Point super[3];

static Point get_pt(int i)
{
    if (i >= 0) return pts[i];
    return super[-i - 1];
}

/* Circumcircle test: is point p inside the circumcircle of triangle
   with vertices a, b, c?  Returns > 0 if inside. */
static double in_circumcircle(Point p, Point a, Point b, Point c)
{
    double ax = a.x - p.x, ay = a.y - p.y;
    double bx = b.x - p.x, by = b.y - p.y;
    double cx = c.x - p.x, cy = c.y - p.y;
    return (ax*ax + ay*ay) * (bx*cy - cx*by)
         - (bx*bx + by*by) * (ax*cy - cx*ay)
         + (cx*cx + cy*cy) * (ax*by - bx*ay);
}

/* Ensure triangle vertices are in CCW order. */
static void ensure_ccw(int *v)
{
    Point a = get_pt(v[0]), b = get_pt(v[1]), c = get_pt(v[2]);
    double cross = (b.x - a.x)*(c.y - a.y) - (b.y - a.y)*(c.x - a.x);
    if (cross < 0) {
        int tmp = v[1]; v[1] = v[2]; v[2] = tmp;
    }
}

static void delaunay(void)
{
    /* Compute bounding box and create super-triangle. */
    double minx = pts[0].x, maxx = pts[0].x;
    double miny = pts[0].y, maxy = pts[0].y;
    for (int i = 1; i < n; i++) {
        if (pts[i].x < minx) minx = pts[i].x;
        if (pts[i].x > maxx) maxx = pts[i].x;
        if (pts[i].y < miny) miny = pts[i].y;
        if (pts[i].y > maxy) maxy = pts[i].y;
    }
    double dx = maxx - minx, dy = maxy - miny;
    double dmax = dx > dy ? dx : dy;
    double midx = (minx + maxx) / 2.0, midy = (miny + maxy) / 2.0;

    super[0] = (Point){midx - 20*dmax, midy - dmax};
    super[1] = (Point){midx + 20*dmax, midy - dmax};
    super[2] = (Point){midx,           midy + 20*dmax};

    ntris = 0;
    tri_add(-1, -2, -3);

    /* Edge buffer for cavity boundary. */
    int (*edges)[2] = NULL;
    int nedges = 0, ecap = 0;

    for (int i = 0; i < n; i++) {
        Point p = pts[i];
        nedges = 0;

        /* Find all triangles whose circumcircle contains p. */
        for (int t = 0; t < ntris; t++) {
            if (!tris[t].alive) continue;
            Point a = get_pt(tris[t].v[0]);
            Point b = get_pt(tris[t].v[1]);
            Point c = get_pt(tris[t].v[2]);
            if (in_circumcircle(p, a, b, c) > 0) {
                tris[t].alive = 0;
                /* Add edges of this triangle to boundary. */
                for (int e = 0; e < 3; e++) {
                    int ea = tris[t].v[e];
                    int eb = tris[t].v[(e+1)%3];
                    /* Check if this edge is shared (appears twice). */
                    int found = -1;
                    for (int k = 0; k < nedges; k++) {
                        if ((edges[k][0] == eb && edges[k][1] == ea) ||
                            (edges[k][0] == ea && edges[k][1] == eb)) {
                            found = k;
                            break;
                        }
                    }
                    if (found >= 0) {
                        /* Shared edge — remove by swapping with last. */
                        edges[found][0] = edges[nedges-1][0];
                        edges[found][1] = edges[nedges-1][1];
                        nedges--;
                    } else {
                        if (nedges >= ecap) {
                            ecap = ecap ? ecap * 2 : 64;
                            edges = realloc(edges, ecap * sizeof(int[2]));
                        }
                        edges[nedges][0] = ea;
                        edges[nedges][1] = eb;
                        nedges++;
                    }
                }
            }
        }

        /* Create new triangles from boundary edges to point i. */
        for (int e = 0; e < nedges; e++) {
            int v[3] = {edges[e][0], edges[e][1], i};
            ensure_ccw(v);
            tri_add(v[0], v[1], v[2]);
        }
    }
    free(edges);

    /* Build adjacency list from triangles (excluding super-triangle vertices). */
    graph = calloc(n, sizeof(AdjList));
    for (int t = 0; t < ntris; t++) {
        if (!tris[t].alive) continue;
        for (int e = 0; e < 3; e++) {
            int a = tris[t].v[e];
            int b = tris[t].v[(e+1)%3];
            if (a < 0 || b < 0) continue;  /* skip super-triangle */
            /* Add edge a-b if not already present. */
            int found = 0;
            for (int k = 0; k < graph[a].deg; k++) {
                if (graph[a].adj[k] == b) { found = 1; break; }
            }
            if (!found && graph[a].deg < MAX_NEIGHBOURS)
                graph[a].adj[graph[a].deg++] = b;
            found = 0;
            for (int k = 0; k < graph[b].deg; k++) {
                if (graph[b].adj[k] == a) { found = 1; break; }
            }
            if (!found && graph[b].deg < MAX_NEIGHBOURS)
                graph[b].adj[graph[b].deg++] = a;
        }
    }
    free(tris);
    tris = NULL;
    tcap = 0;
}

/* ── Brute-force optimal tour (all permutations) ──────────────────── */

static double bf_best;
static int *bf_tour;
static int *bf_perm;
static char *bf_used;

static void bf_search(int depth, double cost)
{
    if (cost >= bf_best) return;  /* prune */

    if (depth == n) {
        double total = cost + D(bf_perm[n-1], bf_perm[0]);
        if (total < bf_best) {
            bf_best = total;
            memcpy(bf_tour, bf_perm, n * sizeof(int));
        }
        return;
    }

    for (int i = 0; i < n; i++) {
        if (bf_used[i]) continue;
        double edge = (depth == 0) ? 0.0 : D(bf_perm[depth-1], i);
        bf_perm[depth] = i;
        bf_used[i] = 1;
        bf_search(depth + 1, cost + edge);
        bf_used[i] = 0;
    }
}

static double brute_force(int *tour)
{
    bf_best = DBL_MAX;
    bf_tour = tour;
    bf_perm = malloc(n * sizeof(int));
    bf_used = calloc(n, 1);
    /* Fix first city to 0 to avoid rotational duplicates. */
    bf_perm[0] = 0;
    bf_used[0] = 1;
    for (int i = 1; i < n; i++) {
        if (!bf_used[i]) {
            bf_perm[1] = i;
            bf_used[i] = 1;
            bf_search(2, D(0, i));
            bf_used[i] = 0;
        }
    }
    free(bf_perm);
    free(bf_used);
    return bf_best;
}

/* ── Delaunay-constrained optimal tour (backtracking on Delaunay graph) ── */

static double del_best;
static int *del_tour;
static int *del_perm;
static char *del_used;

static void del_search(int depth, double cost)
{
    if (cost >= del_best) return;

    if (depth == n) {
        /* Check if last city connects back to first in Delaunay graph. */
        int last = del_perm[n-1], first = del_perm[0];
        int connected = 0;
        for (int k = 0; k < graph[last].deg; k++) {
            if (graph[last].adj[k] == first) { connected = 1; break; }
        }
        if (!connected) return;

        double total = cost + D(last, first);
        if (total < del_best) {
            del_best = total;
            memcpy(del_tour, del_perm, n * sizeof(int));
        }
        return;
    }

    int curr = del_perm[depth - 1];
    for (int k = 0; k < graph[curr].deg; k++) {
        int next = graph[curr].adj[k];
        if (del_used[next]) continue;
        del_perm[depth] = next;
        del_used[next] = 1;
        del_search(depth + 1, cost + D(curr, next));
        del_used[next] = 0;
    }
}

static double delaunay_optimal(int *tour)
{
    del_best = DBL_MAX;
    del_tour = tour;
    del_perm = malloc(n * sizeof(int));
    del_used = calloc(n, 1);
    /* Fix first city to 0. */
    del_perm[0] = 0;
    del_used[0] = 1;
    del_search(1, 0.0);
    free(del_perm);
    free(del_used);
    return del_best;
}

/* ── Check if a tour uses only Delaunay edges ─────────────────────── */

static int tour_uses_only_delaunay(const int *tour)
{
    for (int i = 0; i < n; i++) {
        int a = tour[i], b = tour[(i+1)%n];
        int found = 0;
        for (int k = 0; k < graph[a].deg; k++) {
            if (graph[a].adj[k] == b) { found = 1; break; }
        }
        if (!found) return 0;
    }
    return 1;
}

/* ── Main: run experiments ────────────────────────────────────────── */

static int run_experiment(int seed, int num_cities)
{
    n = num_cities;
    pts = malloc(n * sizeof(Point));

    /* Simple LCG for reproducibility. */
    unsigned long rng = seed;
    for (int i = 0; i < n; i++) {
        rng = rng * 1103515245 + 12345;
        pts[i].x = (double)((rng >> 16) % 10000) / 100.0;
        rng = rng * 1103515245 + 12345;
        pts[i].y = (double)((rng >> 16) % 10000) / 100.0;
    }

    build_dist();
    delaunay();

    /* Print Delaunay graph stats. */
    int total_edges = 0;
    for (int i = 0; i < n; i++) total_edges += graph[i].deg;
    total_edges /= 2;
    int complete_edges = n * (n - 1) / 2;

    printf("  n=%d, seed=%d: Delaunay edges=%d/%d (%.0f%%)\n",
           n, seed, total_edges, complete_edges,
           100.0 * total_edges / complete_edges);

    /* Brute-force true optimal. */
    int *bf_t = malloc(n * sizeof(int));
    double bf_len = brute_force(bf_t);

    /* Delaunay-constrained optimal. */
    int *del_t = malloc(n * sizeof(int));
    double del_len = delaunay_optimal(del_t);

    int bf_in_del = tour_uses_only_delaunay(bf_t);

    printf("  Brute-force optimal: %.4f (uses only Delaunay edges: %s)\n",
           bf_len, bf_in_del ? "YES" : "NO");

    if (del_len < DBL_MAX)
        printf("  Delaunay-constrained optimal: %.4f\n", del_len);
    else
        printf("  Delaunay-constrained: NO Hamiltonian cycle found!\n");

    int falsified = 0;
    if (!bf_in_del) {
        printf("  ** HYPOTHESIS FALSIFIED: optimal tour uses non-Delaunay edge **\n");
        falsified = 1;
    } else if (del_len < DBL_MAX && fabs(del_len - bf_len) < 1e-9) {
        printf("  Hypothesis holds for this instance.\n");
    } else if (del_len < DBL_MAX) {
        printf("  Delaunay-constrained optimal differs from true optimal.\n");
        printf("  ** HYPOTHESIS FALSIFIED: Delaunay-constrained != true optimal **\n");
        falsified = 1;
    }

    free(bf_t);
    free(del_t);
    free(pts);
    free(dist);
    free(graph);
    return falsified;
}

/* ── Structured (adversarial) experiments ─────────────────────────── */

/* Points on a circle with one interior point. The interior point
   creates non-obvious Delaunay structure. */
static int run_circle_experiment(int num_on_circle, int num_interior)
{
    n = num_on_circle + num_interior;
    pts = malloc(n * sizeof(Point));

    for (int i = 0; i < num_on_circle; i++) {
        double angle = 2.0 * M_PI * i / num_on_circle;
        pts[i].x = 50.0 + 40.0 * cos(angle);
        pts[i].y = 50.0 + 40.0 * sin(angle);
    }
    /* Interior points at various radii. */
    for (int i = 0; i < num_interior; i++) {
        double angle = 2.0 * M_PI * (i + 0.5) / num_interior;
        double r = 10.0 + 15.0 * i / (num_interior > 1 ? num_interior - 1 : 1);
        pts[num_on_circle + i].x = 50.0 + r * cos(angle);
        pts[num_on_circle + i].y = 50.0 + r * sin(angle);
    }

    build_dist();
    delaunay();

    int total_edges = 0;
    for (int i = 0; i < n; i++) total_edges += graph[i].deg;
    total_edges /= 2;
    int complete_edges = n * (n - 1) / 2;

    printf("  circle(%d)+interior(%d), n=%d: Delaunay edges=%d/%d (%.0f%%)\n",
           num_on_circle, num_interior, n, total_edges, complete_edges,
           100.0 * total_edges / complete_edges);

    int *bf_t = malloc(n * sizeof(int));
    double bf_len = brute_force(bf_t);
    int bf_in_del = tour_uses_only_delaunay(bf_t);

    printf("  Brute-force optimal: %.4f (uses only Delaunay edges: %s)\n",
           bf_len, bf_in_del ? "YES" : "NO");

    int falsified = 0;
    if (!bf_in_del) {
        printf("  ** HYPOTHESIS FALSIFIED **\n");
        /* Print the non-Delaunay edges used. */
        for (int i = 0; i < n; i++) {
            int a = bf_t[i], b = bf_t[(i+1)%n];
            int found = 0;
            for (int k = 0; k < graph[a].deg; k++)
                if (graph[a].adj[k] == b) { found = 1; break; }
            if (!found)
                printf("    Non-Delaunay edge: %d—%d (dist=%.4f)\n",
                       a, b, D(a, b));
        }
        falsified = 1;
    } else {
        printf("  Hypothesis holds.\n");
    }

    free(bf_t);
    free(pts);
    free(dist);
    free(graph);
    return falsified;
}

/* Near-collinear: points along a line with small perpendicular perturbations. */
static int run_collinear_experiment(int num_cities, int seed)
{
    n = num_cities;
    pts = malloc(n * sizeof(Point));

    unsigned long rng = seed;
    for (int i = 0; i < n; i++) {
        pts[i].x = 10.0 * i;
        rng = rng * 1103515245 + 12345;
        pts[i].y = ((double)((rng >> 16) % 1000) / 1000.0 - 0.5) * 2.0;
    }

    build_dist();
    delaunay();

    int total_edges = 0;
    for (int i = 0; i < n; i++) total_edges += graph[i].deg;
    total_edges /= 2;
    int complete_edges = n * (n - 1) / 2;

    printf("  near-collinear n=%d seed=%d: Delaunay edges=%d/%d (%.0f%%)\n",
           n, seed, total_edges, complete_edges,
           100.0 * total_edges / complete_edges);

    int *bf_t = malloc(n * sizeof(int));
    double bf_len = brute_force(bf_t);
    int bf_in_del = tour_uses_only_delaunay(bf_t);

    printf("  Brute-force optimal: %.4f (Delaunay-only: %s)\n",
           bf_len, bf_in_del ? "YES" : "NO");

    int falsified = 0;
    if (!bf_in_del) {
        printf("  ** HYPOTHESIS FALSIFIED **\n");
        falsified = 1;
    } else {
        printf("  Hypothesis holds.\n");
    }

    free(bf_t);
    free(pts);
    free(dist);
    free(graph);
    return falsified;
}

/* Clustered: k clusters of m points each. */
static int run_cluster_experiment(int k, int m, int seed)
{
    n = k * m;
    pts = malloc(n * sizeof(Point));

    unsigned long rng = seed;
    for (int c = 0; c < k; c++) {
        rng = rng * 1103515245 + 12345;
        double cx = (double)((rng >> 16) % 10000) / 100.0;
        rng = rng * 1103515245 + 12345;
        double cy = (double)((rng >> 16) % 10000) / 100.0;
        for (int i = 0; i < m; i++) {
            rng = rng * 1103515245 + 12345;
            pts[c*m+i].x = cx + ((double)((rng >> 16) % 1000) / 1000.0 - 0.5) * 5.0;
            rng = rng * 1103515245 + 12345;
            pts[c*m+i].y = cy + ((double)((rng >> 16) % 1000) / 1000.0 - 0.5) * 5.0;
        }
    }

    build_dist();
    delaunay();

    int total_edges = 0;
    for (int i = 0; i < n; i++) total_edges += graph[i].deg;
    total_edges /= 2;
    int complete_edges = n * (n - 1) / 2;

    printf("  clustered k=%d m=%d n=%d seed=%d: Delaunay edges=%d/%d (%.0f%%)\n",
           k, m, n, seed, total_edges, complete_edges,
           100.0 * total_edges / complete_edges);

    int *bf_t = malloc(n * sizeof(int));
    double bf_len = brute_force(bf_t);
    int bf_in_del = tour_uses_only_delaunay(bf_t);

    printf("  Brute-force optimal: %.4f (Delaunay-only: %s)\n",
           bf_len, bf_in_del ? "YES" : "NO");

    int falsified = 0;
    if (!bf_in_del) {
        printf("  ** HYPOTHESIS FALSIFIED **\n");
        for (int i = 0; i < n; i++) {
            int a = bf_t[i], b = bf_t[(i+1)%n];
            int found = 0;
            for (int kk = 0; kk < graph[a].deg; kk++)
                if (graph[a].adj[kk] == b) { found = 1; break; }
            if (!found)
                printf("    Non-Delaunay edge: %d—%d (dist=%.4f)\n",
                       a, b, D(a, b));
        }
        falsified = 1;
    } else {
        printf("  Hypothesis holds.\n");
    }

    free(bf_t);
    free(pts);
    free(dist);
    free(graph);
    return falsified;
}

int main(int argc, char **argv)
{
    printf("=== Delaunay TSP Hypothesis Test ===\n");
    printf("Hypothesis: optimal Euclidean TSP tour uses only Delaunay edges.\n\n");

    int falsified = 0;
    int tests = 0;

    /* Phase 1: Random instances. */
    printf("--- Phase 1: Random instances ---\n");
    int sizes[] = {4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);

    for (int si = 0; si < nsizes && !falsified; si++) {
        int num_seeds;
        if (sizes[si] <= 8)       num_seeds = 20;
        else if (sizes[si] <= 12) num_seeds = 10;
        else                      num_seeds = 5;
        for (int seed = 1; seed <= num_seeds && !falsified; seed++) {
            falsified = run_experiment(seed, sizes[si]);
            tests++;
        }
    }

    /* Phase 2: Structured / adversarial instances. */
    if (!falsified) {
        printf("\n--- Phase 2: Structured instances ---\n");

        printf("\nCircle + interior points:\n");
        for (int c = 6; c <= 12 && !falsified; c += 2) {
            for (int interior = 0; interior <= 3 && !falsified; interior++) {
                if (c + interior > 15) continue;
                falsified = run_circle_experiment(c, interior);
                tests++;
            }
        }

        printf("\nNear-collinear:\n");
        for (int nc = 6; nc <= 13 && !falsified; nc++) {
            for (int s = 1; s <= 5 && !falsified; s++) {
                falsified = run_collinear_experiment(nc, s);
                tests++;
            }
        }

        printf("\nClustered:\n");
        for (int s = 1; s <= 10 && !falsified; s++) {
            falsified = run_cluster_experiment(3, 4, s);  /* 12 cities */
            tests++;
        }
        for (int s = 1; s <= 5 && !falsified; s++) {
            falsified = run_cluster_experiment(4, 3, s);  /* 12 cities */
            tests++;
        }
    }

    printf("\n=== Summary ===\n");
    printf("Tests run: %d\n", tests);
    if (falsified)
        printf("Result: HYPOTHESIS FALSIFIED\n");
    else
        printf("Result: Hypothesis held for all tested instances (not proven)\n");

    return falsified ? 1 : 0;
}
