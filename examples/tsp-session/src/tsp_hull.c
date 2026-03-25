/*
 * tsp_hull.c — Convex hull layering TSP experiment
 *
 * Tests the hypothesis: "For Euclidean TSP, the optimal tour can be
 * constructed in polynomial time by exploiting convex hull layers
 * and the non-crossing property."
 *
 * Approach:
 *   1. Compute nested convex hull layers ("onion peeling")
 *   2. Outer hull points must appear in cyclic order (non-crossing)
 *   3. Insert inner-layer points via cheapest-insertion DP
 *   4. Compare against brute-force optimal for small n
 *
 * Compile: gcc -O2 -Wall -o tsp_hull tsp_hull.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <float.h>

typedef struct {
    double x, y;
    int id;     /* original index */
} Point;

static int n;
static Point *pts;
static double *dist;

#define D(i,j) dist[(i)*n+(j)]

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

/* ── Convex hull (Andrew's monotone chain) ────────────────────────── */

static double cross2d(Point o, Point a, Point b)
{
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

static int pt_cmp(const void *a, const void *b)
{
    const Point *pa = a, *pb = b;
    if (pa->x != pb->x) return (pa->x < pb->x) ? -1 : 1;
    return (pa->y < pb->y) ? -1 : (pa->y > pb->y) ? 1 : 0;
}

/* Returns hull size. hull[] must have space for at least cnt elements.
   Points are returned in CCW order. */
static int convex_hull(Point *p, int cnt, Point *hull)
{
    if (cnt < 3) {
        memcpy(hull, p, cnt * sizeof(Point));
        return cnt;
    }
    qsort(p, cnt, sizeof(Point), pt_cmp);

    int k = 0;
    /* Lower hull */
    for (int i = 0; i < cnt; i++) {
        while (k >= 2 && cross2d(hull[k-2], hull[k-1], p[i]) <= 0) k--;
        hull[k++] = p[i];
    }
    /* Upper hull */
    int lower = k + 1;
    for (int i = cnt - 2; i >= 0; i--) {
        while (k >= lower && cross2d(hull[k-2], hull[k-1], p[i]) <= 0) k--;
        hull[k++] = p[i];
    }
    return k - 1;  /* last point == first point */
}

/* ── Onion peeling — compute hull layers ──────────────────────────── */

#define MAX_LAYERS 64

typedef struct {
    int *ids;       /* original point indices */
    int count;
} Layer;

static Layer layers[MAX_LAYERS];
static int nlayers;

static void compute_layers(void)
{
    /* Work with a copy so we don't destroy pts. */
    Point *remaining = malloc(n * sizeof(Point));
    memcpy(remaining, pts, n * sizeof(Point));
    int rem = n;

    Point *hull = malloc((2 * n + 1) * sizeof(Point));
    nlayers = 0;

    while (rem > 0 && nlayers < MAX_LAYERS) {
        int hsize = convex_hull(remaining, rem, hull);

        layers[nlayers].ids = malloc(hsize * sizeof(int));
        layers[nlayers].count = hsize;
        for (int i = 0; i < hsize; i++)
            layers[nlayers].ids[i] = hull[i].id;
        nlayers++;

        /* Remove hull points from remaining. */
        char *on_hull = calloc(n, 1);
        for (int i = 0; i < hsize; i++)
            on_hull[hull[i].id] = 1;

        int new_rem = 0;
        for (int i = 0; i < rem; i++) {
            if (!on_hull[remaining[i].id])
                remaining[new_rem++] = remaining[i];
        }
        rem = new_rem;
        free(on_hull);
    }
    free(remaining);
    free(hull);
}

/* ── Hull-layer TSP solver ────────────────────────────────────────── */

/* Start with outermost hull as initial tour (in order).
   For each inner layer, insert each point at the cheapest position.
   This is a greedy heuristic — the question is whether it's optimal. */
static double hull_layer_solve(int *tour, int *tour_len)
{
    /* Start with outer hull. */
    *tour_len = layers[0].count;
    memcpy(tour, layers[0].ids, layers[0].count * sizeof(int));

    /* Insert inner layer points one at a time, cheapest insertion. */
    for (int layer = 1; layer < nlayers; layer++) {
        for (int p = 0; p < layers[layer].count; p++) {
            int city = layers[layer].ids[p];
            double best_cost = DBL_MAX;
            int best_pos = 0;

            for (int i = 0; i < *tour_len; i++) {
                int a = tour[i];
                int b = tour[(i + 1) % *tour_len];
                double cost = D(a, city) + D(city, b) - D(a, b);
                if (cost < best_cost) {
                    best_cost = cost;
                    best_pos = i + 1;
                }
            }

            /* Insert at best_pos. */
            memmove(&tour[best_pos + 1], &tour[best_pos],
                    (*tour_len - best_pos) * sizeof(int));
            tour[best_pos] = city;
            (*tour_len)++;
        }
    }

    /* Calculate tour length. */
    double total = 0;
    for (int i = 0; i < *tour_len; i++)
        total += D(tour[i], tour[(i + 1) % *tour_len]);
    return total;
}

/* ── Brute-force optimal tour ─────────────────────────────────────── */

static double bf_best;
static int *bf_tour_result;
static int *bf_perm;
static char *bf_used;

static void bf_search(int depth, double cost)
{
    if (cost >= bf_best) return;
    if (depth == n) {
        double total = cost + D(bf_perm[n-1], bf_perm[0]);
        if (total < bf_best) {
            bf_best = total;
            memcpy(bf_tour_result, bf_perm, n * sizeof(int));
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
    bf_tour_result = tour;
    bf_perm = malloc(n * sizeof(int));
    bf_used = calloc(n, 1);
    bf_perm[0] = 0;
    bf_used[0] = 1;
    for (int i = 1; i < n; i++) {
        bf_perm[1] = i;
        bf_used[i] = 1;
        bf_search(2, D(0, i));
        bf_used[i] = 0;
    }
    free(bf_perm);
    free(bf_used);
    return bf_best;
}

/* ── 2-opt improvement ────────────────────────────────────────────── */

static void reverse_seg(int *t, int i, int j)
{
    while (i < j) { int tmp = t[i]; t[i] = t[j]; t[j] = tmp; i++; j--; }
}

static double two_opt(int *tour, int len)
{
    int improved = 1;
    while (improved) {
        improved = 0;
        for (int i = 0; i < len - 1; i++) {
            for (int j = i + 2; j < len; j++) {
                if (i == 0 && j == len - 1) continue;
                int a = tour[i], b = tour[i+1];
                int c = tour[j], d = tour[(j+1) % len];
                if (D(a,b) + D(c,d) > D(a,c) + D(b,d) + 1e-10) {
                    reverse_seg(tour, i+1, j);
                    improved = 1;
                }
            }
        }
    }
    double total = 0;
    for (int i = 0; i < len; i++)
        total += D(tour[i], tour[(i+1) % len]);
    return total;
}

/* ── Experiments ──────────────────────────────────────────────────── */

static int run_experiment(const char *label, Point *input_pts, int num)
{
    n = num;
    pts = malloc(n * sizeof(Point));
    memcpy(pts, input_pts, n * sizeof(Point));
    for (int i = 0; i < n; i++) pts[i].id = i;

    build_dist();
    compute_layers();

    printf("  %s (n=%d, %d layers):\n", label, n, nlayers);
    for (int l = 0; l < nlayers; l++)
        printf("    Layer %d: %d points\n", l, layers[l].count);

    /* Hull-layer solve. */
    int *tour = malloc(n * sizeof(int));
    int tour_len = 0;
    double hull_len = hull_layer_solve(tour, &tour_len);
    printf("    Hull-layer greedy: %.4f\n", hull_len);

    /* Hull-layer + 2-opt. */
    double hull_2opt_len = two_opt(tour, tour_len);
    printf("    Hull-layer + 2-opt: %.4f\n", hull_2opt_len);

    /* Brute-force (only for small n). */
    int falsified = 0;
    if (n <= 13) {
        int *bf_t = malloc(n * sizeof(int));
        double bf_len = brute_force(bf_t);
        printf("    Brute-force optimal: %.4f\n", bf_len);

        double gap = (hull_len - bf_len) / bf_len * 100.0;
        double gap_2opt = (hull_2opt_len - bf_len) / bf_len * 100.0;
        printf("    Gap (greedy): %.2f%%  Gap (+ 2-opt): %.2f%%\n",
               gap, gap_2opt);

        if (fabs(hull_len - bf_len) < 1e-9) {
            printf("    Hull-layer greedy IS optimal.\n");
        } else if (fabs(hull_2opt_len - bf_len) < 1e-9) {
            printf("    Hull-layer + 2-opt reaches optimal.\n");
        } else {
            printf("    Hull-layer does NOT reach optimal (even with 2-opt).\n");
            falsified = 1;
        }
        free(bf_t);
    }

    for (int l = 0; l < nlayers; l++) free(layers[l].ids);
    free(tour);
    free(pts);
    free(dist);
    return falsified;
}

static int run_random(int num, int seed)
{
    Point *p = malloc(num * sizeof(Point));
    unsigned long rng = seed;
    for (int i = 0; i < num; i++) {
        rng = rng * 1103515245 + 12345;
        p[i].x = (double)((rng >> 16) % 10000) / 100.0;
        rng = rng * 1103515245 + 12345;
        p[i].y = (double)((rng >> 16) % 10000) / 100.0;
        p[i].id = i;
    }
    char label[64];
    snprintf(label, sizeof(label), "random seed=%d", seed);
    int r = run_experiment(label, p, num);
    free(p);
    return r;
}

static int run_circle_interior(int on_circle, int interior)
{
    int num = on_circle + interior;
    Point *p = malloc(num * sizeof(Point));
    for (int i = 0; i < on_circle; i++) {
        double a = 2.0 * M_PI * i / on_circle;
        p[i] = (Point){50 + 40*cos(a), 50 + 40*sin(a), i};
    }
    for (int i = 0; i < interior; i++) {
        double a = 2.0 * M_PI * (i + 0.5) / (interior > 0 ? interior : 1);
        double r = 5.0 + 15.0 * i / (interior > 1 ? interior - 1 : 1);
        p[on_circle+i] = (Point){50 + r*cos(a), 50 + r*sin(a), on_circle+i};
    }
    char label[64];
    snprintf(label, sizeof(label), "circle(%d)+interior(%d)", on_circle, interior);
    int r = run_experiment(label, p, num);
    free(p);
    return r;
}

static int run_collinear(int num, int seed)
{
    Point *p = malloc(num * sizeof(Point));
    unsigned long rng = seed;
    for (int i = 0; i < num; i++) {
        p[i].x = 10.0 * i;
        rng = rng * 1103515245 + 12345;
        p[i].y = ((double)((rng >> 16) % 1000) / 1000.0 - 0.5) * 2.0;
        p[i].id = i;
    }
    char label[64];
    snprintf(label, sizeof(label), "collinear seed=%d", seed);
    int r = run_experiment(label, p, num);
    free(p);
    return r;
}

int main(void)
{
    printf("=== Hull-Layer TSP Hypothesis Test ===\n");
    printf("Hypothesis: convex hull layering + cheapest insertion\n");
    printf("produces optimal Euclidean TSP tours.\n\n");

    int falsified = 0, tests = 0, non_optimal = 0;

    /* Random instances */
    printf("--- Random instances ---\n");
    for (int sz = 5; sz <= 12 && !falsified; sz++) {
        int seeds = (sz <= 8) ? 10 : 5;
        for (int s = 1; s <= seeds; s++) {
            int f = run_random(sz, s);
            if (f) non_optimal++;
            tests++;
        }
    }

    /* Circle + interior */
    printf("\n--- Circle + interior ---\n");
    for (int c = 6; c <= 10; c += 2) {
        for (int in = 0; in <= 3; in++) {
            if (c + in > 13) continue;
            int f = run_circle_interior(c, in);
            if (f) non_optimal++;
            tests++;
        }
    }

    /* Near-collinear */
    printf("\n--- Near-collinear ---\n");
    for (int nc = 6; nc <= 12; nc++) {
        for (int s = 1; s <= 3; s++) {
            int f = run_collinear(nc, s);
            if (f) non_optimal++;
            tests++;
        }
    }

    printf("\n=== Summary ===\n");
    printf("Tests run: %d\n", tests);
    printf("Non-optimal results: %d\n", non_optimal);
    if (non_optimal > 0)
        printf("Result: Hull-layer greedy is NOT always optimal (even with 2-opt)\n");
    else
        printf("Result: Hull-layer greedy was optimal for all tested instances\n");

    return non_optimal > 0 ? 1 : 0;
}
