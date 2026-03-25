/*
 * tsp_topo.c — Topological TSP experiment (boundary cycle extraction)
 *
 * Tests the hypothesis: "The boundary cycle of the Vietoris-Rips complex
 * at the critical filtration parameter approximates or equals the optimal
 * Euclidean TSP tour."
 *
 * Approach:
 *   1. Find the critical filtration radius r* where the Rips complex
 *      first becomes a single connected component (= MST max edge)
 *   2. At radius slightly above r*, extract the "boundary" — the outer
 *      cycle of the planar graph induced by edges ≤ r*
 *   3. If the boundary visits all vertices, compare against brute-force optimal
 *
 * Also explores: alpha-complex boundary cycles and persistent H1 generators.
 *
 * Compile: gcc -O2 -Wall -o tsp_topo tsp_topo.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <float.h>

static int n;
static double *px, *py;
static double *dist;

#define D(i,j) dist[(i)*n+(j)]

static void build_dist(void)
{
    dist = malloc((size_t)n * (size_t)n * sizeof(double));
    for (int i = 0; i < n; i++) {
        dist[i*n+i] = 0.0;
        for (int j = i+1; j < n; j++) {
            double dx = px[i] - px[j];
            double dy = py[i] - py[j];
            double d = sqrt(dx*dx + dy*dy);
            dist[i*n+j] = d;
            dist[j*n+i] = d;
        }
    }
}

/* ── Union-Find ───────────────────────────────────────────────────── */

static int *uf_parent, *uf_rank_arr;

static void uf_init(int sz)
{
    uf_parent = malloc(sz * sizeof(int));
    uf_rank_arr = malloc(sz * sizeof(int));
    for (int i = 0; i < sz; i++) { uf_parent[i] = i; uf_rank_arr[i] = 0; }
}

static int uf_find(int x)
{
    while (uf_parent[x] != x) { uf_parent[x] = uf_parent[uf_parent[x]]; x = uf_parent[x]; }
    return x;
}

static int uf_union(int a, int b)
{
    a = uf_find(a); b = uf_find(b);
    if (a == b) return 0;
    if (uf_rank_arr[a] < uf_rank_arr[b]) { int t = a; a = b; b = t; }
    uf_parent[b] = a;
    if (uf_rank_arr[a] == uf_rank_arr[b]) uf_rank_arr[a]++;
    return 1;
}

static void uf_free(void) { free(uf_parent); free(uf_rank_arr); }

/* ── Edge list sorted by distance ─────────────────────────────────── */

typedef struct { int u, v; double w; } Edge;

static int edge_cmp(const void *a, const void *b)
{
    double d = ((const Edge*)a)->w - ((const Edge*)b)->w;
    return (d < 0) ? -1 : (d > 0) ? 1 : 0;
}

/* ── Find critical radius (MST max edge = connectivity threshold) ── */

static double find_critical_radius(Edge *edges, int nedges)
{
    uf_init(n);
    double r_crit = 0;
    int components = n;
    for (int i = 0; i < nedges && components > 1; i++) {
        if (uf_union(edges[i].u, edges[i].v)) {
            r_crit = edges[i].w;
            components--;
        }
    }
    uf_free();
    return r_crit;
}

/* ── Build Rips graph at radius r and extract boundary cycle ──────── */

/* Adjacency list for the Rips graph. */
#define MAX_ADJ 64

typedef struct {
    int adj[MAX_ADJ];
    int deg;
} AdjNode;

/* Compute angle of vector from point i to point j. */
static double angle_of(int i, int j)
{
    return atan2(py[j] - py[i], px[j] - px[i]);
}

/* Extract the outer boundary of a planar graph by walking edges,
   always turning as far right (clockwise) as possible.
   This traces the convex-hull-like outer face of the planar embedding. */
static int extract_boundary(AdjNode *g, int *tour)
{
    /* Start from the leftmost point (like convex hull). */
    int start = 0;
    for (int i = 1; i < n; i++) {
        if (px[i] < px[start] || (px[i] == px[start] && py[i] < py[start]))
            start = i;
    }

    /* Begin walking: from start, go to the neighbour with the smallest
       angle (most clockwise from "pointing down"). */
    int curr = start;
    /* Initial direction: pointing downward (angle = -PI/2). */
    double prev_angle = -M_PI / 2.0;

    int len = 0;
    int max_steps = n + 1;

    for (int step = 0; step < max_steps; step++) {
        tour[len++] = curr;

        if (g[curr].deg == 0) return -1;  /* disconnected */

        /* Find the neighbour with the largest right turn from prev_angle.
           That is, the neighbour whose angle relative to curr is
           the "next" angle clockwise from (prev_angle + PI). */
        double incoming = prev_angle + M_PI;  /* direction we came from */

        int best_nb = -1;
        double best_turn = -10;  /* want the smallest positive turn */

        for (int k = 0; k < g[curr].deg; k++) {
            int nb = g[curr].adj[k];
            double a = angle_of(curr, nb);
            /* Turn angle: how far CCW from incoming direction. */
            double turn = a - incoming;
            /* Normalise to (-2PI, 0] — we want the most clockwise (most negative). */
            while (turn > 0) turn -= 2 * M_PI;
            while (turn <= -2 * M_PI) turn += 2 * M_PI;

            if (best_nb == -1 || turn > best_turn) {
                best_turn = turn;
                best_nb = nb;
            }
        }

        prev_angle = angle_of(curr, best_nb);
        curr = best_nb;

        if (curr == start && len > 1) break;  /* completed cycle */
    }

    return len;
}

/* ── Brute-force optimal ──────────────────────────────────────────── */

static double bf_best;
static int *bf_tour, *bf_perm;
static char *bf_used;

static void bf_search(int depth, double cost)
{
    if (cost >= bf_best) return;
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
    bf_perm[0] = 0; bf_used[0] = 1;
    for (int i = 1; i < n; i++) {
        bf_perm[1] = i; bf_used[i] = 1;
        bf_search(2, D(0, i));
        bf_used[i] = 0;
    }
    free(bf_perm);
    free(bf_used);
    return bf_best;
}

/* ── Tour length ──────────────────────────────────────────────────── */

static double tour_length(const int *tour, int len)
{
    double total = 0;
    for (int i = 0; i < len; i++)
        total += D(tour[i], tour[(i+1) % len]);
    return total;
}

/* ── Run experiment at multiple radii ─────────────────────────────── */

static int run_experiment(const char *label, int do_bf)
{
    build_dist();

    /* Build sorted edge list. */
    int nedges = n * (n - 1) / 2;
    Edge *edges = malloc(nedges * sizeof(Edge));
    int ei = 0;
    for (int i = 0; i < n; i++)
        for (int j = i+1; j < n; j++)
            edges[ei++] = (Edge){i, j, D(i,j)};
    qsort(edges, nedges, sizeof(Edge), edge_cmp);

    double r_crit = find_critical_radius(edges, nedges);

    printf("  %s (n=%d): r_crit=%.4f\n", label, n, r_crit);

    /* Try boundary extraction at several radii around r_crit. */
    double radii[] = {r_crit, r_crit * 1.1, r_crit * 1.3, r_crit * 1.5, r_crit * 2.0};
    int nradii = 5;

    double bf_len = 0;
    int *bf_t = NULL;
    if (do_bf) {
        bf_t = malloc(n * sizeof(int));
        bf_len = brute_force(bf_t);
        printf("    Brute-force optimal: %.4f\n", bf_len);
    }

    int found_match = 0;

    for (int ri = 0; ri < nradii; ri++) {
        double r = radii[ri];

        /* Build Rips graph at radius r. */
        AdjNode *g = calloc(n, sizeof(AdjNode));
        for (int e = 0; e < nedges; e++) {
            if (edges[e].w > r) break;
            int u = edges[e].u, v = edges[e].v;
            if (g[u].deg < MAX_ADJ) g[u].adj[g[u].deg++] = v;
            if (g[v].deg < MAX_ADJ) g[v].adj[g[v].deg++] = u;
        }

        /* Sort adjacency lists by angle for planar embedding. */
        for (int i = 0; i < n; i++) {
            /* Insertion sort by angle. */
            for (int a = 1; a < g[i].deg; a++) {
                int key = g[i].adj[a];
                double key_angle = angle_of(i, key);
                int b = a - 1;
                while (b >= 0 && angle_of(i, g[i].adj[b]) > key_angle) {
                    g[i].adj[b+1] = g[i].adj[b];
                    b--;
                }
                g[i].adj[b+1] = key;
            }
        }

        int *tour = malloc(n * sizeof(int));
        int tour_len = extract_boundary(g, tour);

        if (tour_len > 0 && tour_len <= n) {
            /* Check if it visits all vertices. */
            char *seen = calloc(n, 1);
            int all_visited = 1;
            for (int i = 0; i < tour_len; i++) {
                if (tour[i] < 0 || tour[i] >= n || seen[tour[i]]) {
                    all_visited = 0;
                    break;
                }
                seen[tour[i]] = 1;
            }
            if (all_visited && tour_len == n) {
                double tl = tour_length(tour, tour_len);
                printf("    r=%.2f (%.0f%% of r_crit): boundary visits ALL %d cities, len=%.4f",
                       r, r/r_crit*100, n, tl);
                if (do_bf) {
                    double gap = (tl - bf_len) / bf_len * 100.0;
                    printf("  gap=%.2f%%", gap);
                    if (fabs(gap) < 0.01) {
                        printf("  ** OPTIMAL **");
                        found_match = 1;
                    }
                }
                printf("\n");
            } else {
                printf("    r=%.2f: boundary visits %d/%d cities\n",
                       r, tour_len, n);
            }
            free(seen);
        } else {
            printf("    r=%.2f: boundary extraction failed\n", r);
        }

        free(tour);
        free(g);
    }

    free(edges);
    if (bf_t) free(bf_t);
    free(dist);

    return found_match;
}

int main(void)
{
    printf("=== Topological TSP Experiment ===\n");
    printf("Hypothesis: Rips boundary cycle at critical radius = optimal tour\n\n");

    int optimal_count = 0, total = 0;

    /* Random instances */
    printf("--- Random instances ---\n");
    int sizes[] = {5, 6, 7, 8, 9, 10, 12};
    for (int si = 0; si < 7; si++) {
        int sz = sizes[si];
        int seeds = (sz <= 8) ? 10 : 5;
        for (int seed = 1; seed <= seeds; seed++) {
            n = sz;
            px = malloc(n * sizeof(double));
            py = malloc(n * sizeof(double));
            unsigned long rng = seed;
            for (int i = 0; i < n; i++) {
                rng = rng * 1103515245 + 12345;
                px[i] = (double)((rng >> 16) % 10000) / 100.0;
                rng = rng * 1103515245 + 12345;
                py[i] = (double)((rng >> 16) % 10000) / 100.0;
            }
            char label[64];
            snprintf(label, sizeof(label), "random seed=%d", seed);
            optimal_count += run_experiment(label, 1);
            total++;
            free(px); free(py);
        }
    }

    /* Circle instances */
    printf("\n--- Circle instances ---\n");
    for (int nc = 5; nc <= 12; nc++) {
        n = nc;
        px = malloc(n * sizeof(double));
        py = malloc(n * sizeof(double));
        for (int i = 0; i < n; i++) {
            double a = 2.0 * M_PI * i / n;
            px[i] = 50 + 40 * cos(a);
            py[i] = 50 + 40 * sin(a);
        }
        char label[64];
        snprintf(label, sizeof(label), "circle n=%d", n);
        optimal_count += run_experiment(label, 1);
        total++;
        free(px); free(py);
    }

    printf("\n=== Summary ===\n");
    printf("Tests: %d\n", total);
    printf("Boundary = optimal: %d\n", optimal_count);
    if (optimal_count == total)
        printf("Result: Hypothesis holds for all tested instances\n");
    else
        printf("Result: Boundary cycle is NOT always optimal\n");

    return (optimal_count < total) ? 1 : 0;
}
