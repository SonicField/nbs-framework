/*
 * tsp_heldkarp.c — Held-Karp lower bound via 1-trees + Lagrangian relaxation
 *
 * Computes a lower bound on the optimal TSP tour using:
 *   1. Minimum 1-tree: MST on cities {1..n-1} + 2 cheapest edges to city 0
 *   2. Subgradient optimisation of Lagrange multipliers to tighten the bound
 *   3. If the 1-tree is a valid Hamiltonian cycle, the solution is proven optimal
 *
 * Also runs the heuristic solver (NN + 2-opt) and reports the gap.
 *
 * Compile: gcc -O2 -Wall -o tsp_heldkarp tsp_heldkarp.c -lm
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

/* ── Minimum spanning tree (Prim's) on a subset of vertices ───────── */

/* Compute MST on vertices in subset[] (count elements).
   Uses modified edge weights: w(i,j) = D(i,j) + lambda[i] + lambda[j].
   Returns MST cost (with modified weights) and fills mst_adj with adjacency.
   mst_deg[v] = degree of v in the MST. */
static double prim_mst(const int *subset, int count, const double *lambda,
                       int mst_adj[][2], int *mst_deg, int *num_edges)
{
    if (count <= 1) {
        *num_edges = 0;
        return 0.0;
    }

    char *in_tree = calloc(n, 1);
    double *min_cost = malloc(n * sizeof(double));
    int *min_from = malloc(n * sizeof(int));

    for (int i = 0; i < n; i++) min_cost[i] = DBL_MAX;

    /* Start from first vertex in subset. */
    int start = subset[0];
    in_tree[start] = 1;
    for (int i = 1; i < count; i++) {
        int v = subset[i];
        min_cost[v] = D(start, v) + lambda[start] + lambda[v];
        min_from[v] = start;
    }

    double total = 0.0;
    *num_edges = 0;

    for (int step = 1; step < count; step++) {
        /* Find cheapest edge to add. */
        int best = -1;
        double best_cost = DBL_MAX;
        for (int i = 0; i < count; i++) {
            int v = subset[i];
            if (!in_tree[v] && min_cost[v] < best_cost) {
                best_cost = min_cost[v];
                best = v;
            }
        }

        in_tree[best] = 1;
        total += best_cost;
        mst_adj[*num_edges][0] = min_from[best];
        mst_adj[*num_edges][1] = best;
        (*num_edges)++;
        mst_deg[best]++;
        mst_deg[min_from[best]]++;

        /* Update costs. */
        for (int i = 0; i < count; i++) {
            int v = subset[i];
            if (!in_tree[v]) {
                double c = D(best, v) + lambda[best] + lambda[v];
                if (c < min_cost[v]) {
                    min_cost[v] = c;
                    min_from[v] = best;
                }
            }
        }
    }

    free(in_tree);
    free(min_cost);
    free(min_from);
    return total;
}

/* ── Held-Karp lower bound ────────────────────────────────────────── */

typedef struct {
    double lower_bound;
    double upper_bound;     /* from heuristic */
    int is_tour;            /* 1 if the 1-tree is a valid tour */
    int *best_tour;         /* valid only if is_tour */
} HKResult;

/* Check if a 1-tree (MST on {1..n-1} + 2 edges to vertex 0) forms
   a Hamiltonian cycle. This requires every vertex to have degree 2. */
static int is_hamiltonian(int *deg)
{
    for (int i = 0; i < n; i++)
        if (deg[i] != 2) return 0;
    return 1;
}

/* Extract tour from 1-tree adjacency (only valid if is_hamiltonian). */
static void extract_tour(int edges[][2], int nedges, int *tour)
{
    /* Build adjacency list from edges. */
    int (*adj)[2] = calloc(n, sizeof(int[2]));
    int *deg = calloc(n, sizeof(int));

    for (int i = 0; i < nedges; i++) {
        int a = edges[i][0], b = edges[i][1];
        adj[a][deg[a]++] = b;
        adj[b][deg[b]++] = a;
    }

    /* Walk the cycle starting from 0. */
    tour[0] = 0;
    int prev = -1;
    for (int step = 1; step < n; step++) {
        int curr = tour[step - 1];
        int next = adj[curr][0];
        if (next == prev) next = adj[curr][1];
        tour[step] = next;
        prev = curr;
    }
    free(adj);
    free(deg);
}

static HKResult held_karp(void)
{
    HKResult result;
    result.lower_bound = -DBL_MAX;
    result.is_tour = 0;
    result.best_tour = malloc(n * sizeof(int));

    double *lambda = calloc(n, sizeof(double));

    /* Subset for MST: vertices 1..n-1 */
    int *subset = malloc((n - 1) * sizeof(int));
    for (int i = 0; i < n - 1; i++) subset[i] = i + 1;

    int (*mst_edges)[2] = malloc(n * sizeof(int[2]));
    int (*all_edges)[2] = malloc((n + 1) * sizeof(int[2]));

    /* Subgradient parameters. */
    int max_iter = 200;
    double t = 1.0;              /* step size */
    int no_improve = 0;
    double best_lb = -DBL_MAX;

    for (int iter = 0; iter < max_iter; iter++) {
        int *deg = calloc(n, sizeof(int));
        int mst_nedges = 0;

        /* 1. MST on {1..n-1} with modified weights. */
        double mst_cost = prim_mst(subset, n - 1, lambda,
                                    mst_edges, deg, &mst_nedges);

        /* 2. Add two cheapest edges from vertex 0 (modified weights). */
        double e1_cost = DBL_MAX, e2_cost = DBL_MAX;
        int e1_to = -1, e2_to = -1;
        for (int j = 1; j < n; j++) {
            double c = D(0, j) + lambda[0] + lambda[j];
            if (c < e1_cost) {
                e2_cost = e1_cost; e2_to = e1_to;
                e1_cost = c; e1_to = j;
            } else if (c < e2_cost) {
                e2_cost = c; e2_to = j;
            }
        }

        /* Build complete 1-tree edge set. */
        int total_edges = 0;
        for (int i = 0; i < mst_nedges; i++) {
            all_edges[total_edges][0] = mst_edges[i][0];
            all_edges[total_edges][1] = mst_edges[i][1];
            total_edges++;
        }
        all_edges[total_edges][0] = 0; all_edges[total_edges][1] = e1_to;
        total_edges++;
        all_edges[total_edges][0] = 0; all_edges[total_edges][1] = e2_to;
        total_edges++;

        deg[0] = 2;
        deg[e1_to]++;
        deg[e2_to]++;

        /* 3. Compute 1-tree cost (original weights). */
        double onetree_cost = 0;
        for (int i = 0; i < total_edges; i++)
            onetree_cost += D(all_edges[i][0], all_edges[i][1]);

        /* The lower bound = 1-tree cost with modified weights - 2 * sum(lambda)
           But equivalently, the original-weight 1-tree cost is a valid bound
           after subgradient adjustment. Actually the correct formula:
           LB = mst_cost + e1_cost + e2_cost - 2 * sum(lambda) */
        double sum_lambda = 0;
        for (int i = 0; i < n; i++) sum_lambda += lambda[i];
        double lb = mst_cost + e1_cost + e2_cost - 2.0 * sum_lambda;

        if (lb > result.lower_bound) {
            result.lower_bound = lb;

            /* Check if this 1-tree is a tour. */
            if (is_hamiltonian(deg)) {
                result.is_tour = 1;
                extract_tour(all_edges, total_edges, result.best_tour);
            }
        }

        if (lb > best_lb) {
            best_lb = lb;
            no_improve = 0;
        } else {
            no_improve++;
        }

        /* Step size reduction. */
        if (no_improve >= 20) {
            t *= 0.5;
            no_improve = 0;
        }
        if (t < 1e-10) {
            free(deg);
            break;
        }

        /* 4. Subgradient step. */
        /* Gradient: g[i] = deg[i] - 2 (want all degrees = 2). */
        double norm2 = 0;
        for (int i = 0; i < n; i++) {
            double g = deg[i] - 2;
            norm2 += g * g;
        }

        if (norm2 < 1e-10) {
            free(deg);
            break;  /* All degrees are 2 — it's a tour! */
        }

        /* Step: lambda[i] += t * (deg[i] - 2) */
        for (int i = 0; i < n; i++)
            lambda[i] += t * (deg[i] - 2);

        free(deg);
    }

    free(lambda);
    free(subset);
    free(mst_edges);
    free(all_edges);
    return result;
}

/* ── Heuristic solver (NN + 2-opt) for upper bound ────────────────── */

static void nearest_neighbour(int start, int *tour)
{
    char *visited = calloc(n, 1);
    tour[0] = start;
    visited[start] = 1;
    for (int step = 1; step < n; step++) {
        int curr = tour[step-1], best = -1;
        double best_d = DBL_MAX;
        for (int j = 0; j < n; j++) {
            if (!visited[j] && D(curr,j) < best_d) {
                best_d = D(curr,j);
                best = j;
            }
        }
        tour[step] = best;
        visited[best] = 1;
    }
    free(visited);
}

static double tour_length(const int *tour)
{
    double total = 0;
    for (int i = 0; i < n; i++)
        total += D(tour[i], tour[(i+1)%n]);
    return total;
}

static void reverse_seg(int *t, int i, int j)
{
    while (i < j) { int tmp = t[i]; t[i] = t[j]; t[j] = tmp; i++; j--; }
}

static double two_opt(int *tour)
{
    int improved = 1;
    while (improved) {
        improved = 0;
        for (int i = 0; i < n-1; i++) {
            for (int j = i+2; j < n; j++) {
                if (i == 0 && j == n-1) continue;
                int a = tour[i], b = tour[i+1];
                int c = tour[j], d = tour[(j+1)%n];
                if (D(a,b) + D(c,d) > D(a,c) + D(b,d) + 1e-10) {
                    reverse_seg(tour, i+1, j);
                    improved = 1;
                }
            }
        }
    }
    return tour_length(tour);
}

static double heuristic_solve(int *tour)
{
    int *tmp = malloc(n * sizeof(int));
    double best = DBL_MAX;
    int starts = n < 50 ? n : 50;
    for (int s = 0; s < starts; s++) {
        int start = (starts == n) ? s : (s * n / starts);
        nearest_neighbour(start, tmp);
        double len = two_opt(tmp);
        if (len < best) {
            best = len;
            memcpy(tour, tmp, n * sizeof(int));
        }
    }
    free(tmp);
    return best;
}

/* ── Brute-force (small n only) ───────────────────────────────────── */

static double bf_best_g;
static int *bf_tour_g, *bf_perm_g;
static char *bf_used_g;

static void bf_search(int depth, double cost)
{
    if (cost >= bf_best_g) return;
    if (depth == n) {
        double total = cost + D(bf_perm_g[n-1], bf_perm_g[0]);
        if (total < bf_best_g) {
            bf_best_g = total;
            memcpy(bf_tour_g, bf_perm_g, n * sizeof(int));
        }
        return;
    }
    for (int i = 0; i < n; i++) {
        if (bf_used_g[i]) continue;
        double edge = (depth == 0) ? 0.0 : D(bf_perm_g[depth-1], i);
        bf_perm_g[depth] = i;
        bf_used_g[i] = 1;
        bf_search(depth + 1, cost + edge);
        bf_used_g[i] = 0;
    }
}

static double brute_force(int *tour)
{
    bf_best_g = DBL_MAX;
    bf_tour_g = tour;
    bf_perm_g = malloc(n * sizeof(int));
    bf_used_g = calloc(n, 1);
    bf_perm_g[0] = 0; bf_used_g[0] = 1;
    for (int i = 1; i < n; i++) {
        bf_perm_g[1] = i; bf_used_g[i] = 1;
        bf_search(2, D(0, i));
        bf_used_g[i] = 0;
    }
    free(bf_perm_g);
    free(bf_used_g);
    return bf_best_g;
}

/* ── Experiments ──────────────────────────────────────────────────── */

static void run_instance(const char *label, int do_bf)
{
    build_dist();

    HKResult hk = held_karp();

    int *heur_tour = malloc(n * sizeof(int));
    double heur_len = heuristic_solve(heur_tour);

    printf("  %s (n=%d):\n", label, n);
    printf("    HK lower bound: %.4f\n", hk.lower_bound);
    printf("    Heuristic:      %.4f\n", heur_len);

    double gap = (heur_len - hk.lower_bound) / hk.lower_bound * 100.0;
    printf("    Gap:            %.2f%%\n", gap);

    if (hk.is_tour) {
        double hk_tour_len = tour_length(hk.best_tour);
        printf("    ** HK found a valid tour! Cost: %.4f **\n", hk_tour_len);
        if (fabs(hk_tour_len - hk.lower_bound) < 1e-6)
            printf("    ** PROVEN OPTIMAL **\n");
    }

    /* Sanity: lower bound must not exceed heuristic. */
    if (hk.lower_bound > heur_len + 1e-6) {
        printf("    !! BUG: lower bound > heuristic cost !!\n");
    }

    if (do_bf) {
        int *bf_t = malloc(n * sizeof(int));
        double bf_len = brute_force(bf_t);
        printf("    Brute-force:    %.4f\n", bf_len);
        double bf_gap = (bf_len - hk.lower_bound) / hk.lower_bound * 100.0;
        printf("    BF-HK gap:      %.2f%%\n", bf_gap);
        if (fabs(bf_len - hk.lower_bound) < 1e-6)
            printf("    ** HK bound is tight (equals optimal) **\n");
        free(bf_t);
    }

    free(heur_tour);
    free(hk.best_tour);
    free(dist);
}

int main(int argc, char **argv)
{
    /* Interactive mode: read from stdin. */
    if (argc > 1 && strcmp(argv[1], "--stdin") == 0) {
        if (scanf("%d", &n) != 1 || n < 2) {
            fprintf(stderr, "Need n >= 2\n");
            return 1;
        }
        px = malloc(n * sizeof(double));
        py = malloc(n * sizeof(double));
        for (int i = 0; i < n; i++)
            scanf("%lf %lf", &px[i], &py[i]);
        run_instance("stdin", n <= 13);
        free(px); free(py);
        return 0;
    }

    printf("=== Held-Karp Lower Bound Experiment ===\n\n");

    /* Random instances at various sizes. */
    int sizes[] = {5, 6, 7, 8, 9, 10, 12, 15, 20, 50, 100};
    int nsizes = sizeof(sizes) / sizeof(sizes[0]);

    int tight_count = 0, total_tests = 0;
    double max_gap = 0;

    for (int si = 0; si < nsizes; si++) {
        int sz = sizes[si];
        int seeds = (sz <= 12) ? 5 : 3;
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

            build_dist();
            HKResult hk = held_karp();

            int *heur_tour = malloc(n * sizeof(int));
            double heur_len = heuristic_solve(heur_tour);

            printf("  n=%d seed=%d: HK=%.2f  heur=%.2f  gap=%.2f%%",
                   n, seed, hk.lower_bound, heur_len,
                   (heur_len - hk.lower_bound) / hk.lower_bound * 100.0);

            double gap = (heur_len - hk.lower_bound) / hk.lower_bound * 100.0;
            if (gap > max_gap) max_gap = gap;

            if (hk.lower_bound > heur_len + 1e-6)
                printf("  !! BUG: LB > UB !!");

            if (sz <= 12) {
                int *bf_t = malloc(n * sizeof(int));
                double bf_len = brute_force(bf_t);
                if (fabs(bf_len - hk.lower_bound) < 1e-6) {
                    printf("  [TIGHT]");
                    tight_count++;
                }
                if (fabs(bf_len - heur_len) < 1e-6)
                    printf("  [heur=opt]");
                free(bf_t);
            }

            if (hk.is_tour) printf("  [HK=tour]");

            printf("\n");

            total_tests++;
            free(heur_tour);
            free(hk.best_tour);
            free(dist);
            free(px); free(py);
        }
    }

    printf("\n=== Summary ===\n");
    printf("Tests: %d\n", total_tests);
    printf("Tight bounds (HK = optimal): %d (of those with brute-force)\n", tight_count);
    printf("Max heuristic-HK gap: %.2f%%\n", max_gap);

    return 0;
}
