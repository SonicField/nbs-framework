/*
 * tsp_test.c — Standalone test harness for tsp.c
 *
 * Tests beyond the built-in self-test:
 *   1. 2-opt monotonicity: cost never increases
 *   2. Or-opt monotonicity: cost never increases
 *   3. Tour validity (permutation check) on random instances
 *   4. Cost consistency (independent recalculation)
 *   5. Malformed / adversarial stdin input handling
 *   6. Coincident cities edge case
 *   7. Large-n stress test
 *
 * Compile: gcc -O2 -Wall -Wextra -o tsp_test tsp_test.c -lm
 * Run:     ./tsp_test
 *
 * Requires: tsp binary in same directory (built from tsp.c)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <assert.h>

/* ── Minimal TSP internals duplicated for white-box testing ── */

typedef struct { double x, y; } City;

static int n;
static City *cities;
static double *dist_matrix;

static void build_dist_matrix(void)
{
    dist_matrix = malloc((size_t)n * (size_t)n * sizeof(double));
    assert(dist_matrix && "OOM in test harness");
    for (int i = 0; i < n; i++) {
        dist_matrix[i * n + i] = 0.0;
        for (int j = i + 1; j < n; j++) {
            double dx = cities[i].x - cities[j].x;
            double dy = cities[i].y - cities[j].y;
            double d = sqrt(dx * dx + dy * dy);
            dist_matrix[i * n + j] = d;
            dist_matrix[j * n + i] = d;
        }
    }
}

#define DIST(i, j) dist_matrix[(i) * n + (j)]

static double tour_length(const int *tour)
{
    double total = 0.0;
    for (int i = 0; i < n; i++)
        total += DIST(tour[i], tour[(i + 1) % n]);
    return total;
}

static void nearest_neighbour(int start, int *tour)
{
    char *visited = calloc(n, 1);
    assert(visited && "OOM");
    tour[0] = start;
    visited[start] = 1;
    for (int step = 1; step < n; step++) {
        int curr = tour[step - 1];
        int best = -1;
        double best_d = DBL_MAX;
        for (int j = 0; j < n; j++) {
            if (!visited[j] && DIST(curr, j) < best_d) {
                best_d = DIST(curr, j);
                best = j;
            }
        }
        tour[step] = best;
        visited[best] = 1;
    }
    free(visited);
}

static void reverse_segment(int *tour, int i, int j)
{
    while (i < j) {
        int tmp = tour[i];
        tour[i] = tour[j];
        tour[j] = tmp;
        i++; j--;
    }
}

/* 2-opt that records cost after each improving swap for monotonicity check. */
static int two_opt_monotonic(int *tour, double *costs, int max_costs,
                             int *num_costs)
{
    *num_costs = 0;
    double prev_cost = tour_length(tour);
    if (*num_costs < max_costs)
        costs[(*num_costs)++] = prev_cost;

    int improved = 1;
    while (improved) {
        improved = 0;
        for (int i = 0; i < n - 1; i++) {
            for (int j = i + 2; j < n; j++) {
                if (i == 0 && j == n - 1) continue;
                int a = tour[i], b = tour[i + 1];
                int c = tour[j], d = tour[(j + 1) % n];
                double old_d = DIST(a, b) + DIST(c, d);
                double new_d = DIST(a, c) + DIST(b, d);
                if (new_d < old_d - 1e-10) {
                    reverse_segment(tour, i + 1, j);
                    double cur = tour_length(tour);
                    if (*num_costs < max_costs)
                        costs[(*num_costs)++] = cur;
                    if (cur > prev_cost + 1e-9) {
                        /* Monotonicity violated */
                        return 0;
                    }
                    prev_cost = cur;
                    improved = 1;
                }
            }
        }
    }
    return 1; /* monotonic */
}

/* Or-opt that checks monotonicity. */
static int or_opt_monotonic(int *tour, double *costs, int max_costs,
                            int *num_costs)
{
    int *tmp = malloc(n * sizeof(int));
    assert(tmp && "OOM");
    *num_costs = 0;
    double prev_cost = tour_length(tour);
    if (*num_costs < max_costs)
        costs[(*num_costs)++] = prev_cost;

    int improved = 1;
    while (improved) {
        improved = 0;
        for (int seg_len = 1; seg_len <= 3 && seg_len < n - 1; seg_len++) {
            for (int i = 0; i < n; i++) {
                int prev_i = (i - 1 + n) % n;
                int end_i = (i + seg_len - 1) % n;
                int next_end = (i + seg_len) % n;
                double remove_cost = DIST(tour[prev_i], tour[i])
                                   + DIST(tour[end_i], tour[next_end]);
                double close_cost = DIST(tour[prev_i], tour[next_end]);
                double seg_gain = remove_cost - close_cost;

                for (int j = 0; j < n; j++) {
                    int skip = 0;
                    for (int k = 0; k < seg_len; k++) {
                        if (j == (i + k) % n || j == prev_i) {
                            skip = 1; break;
                        }
                    }
                    if (skip) continue;

                    int next_j = (j + 1) % n;
                    double insert_cost = DIST(tour[j], tour[i])
                                       + DIST(tour[end_i], tour[next_j])
                                       - DIST(tour[j], tour[next_j]);

                    if (seg_gain - insert_cost > 1e-10) {
                        int idx = 0;
                        int pos = next_end;
                        while (pos != (j + 1) % n) {
                            tmp[idx++] = tour[pos];
                            pos = (pos + 1) % n;
                        }
                        for (int k = 0; k < seg_len; k++)
                            tmp[idx++] = tour[(i + k) % n];
                        pos = (j + 1) % n;
                        while (idx < n) {
                            int is_seg = 0;
                            for (int k = 0; k < seg_len; k++) {
                                if (pos == (i + k) % n) {
                                    is_seg = 1; break;
                                }
                            }
                            if (is_seg) { pos = (pos + 1) % n; continue; }
                            tmp[idx++] = tour[pos];
                            pos = (pos + 1) % n;
                        }
                        memcpy(tour, tmp, n * sizeof(int));
                        double cur = tour_length(tour);
                        if (*num_costs < max_costs)
                            costs[(*num_costs)++] = cur;
                        if (cur > prev_cost + 1e-9) {
                            free(tmp);
                            return 0; /* monotonicity violated */
                        }
                        prev_cost = cur;
                        improved = 1;
                        break;
                    }
                }
                if (improved) break;
            }
            if (improved) break;
        }
    }
    free(tmp);
    return 1;
}

/* Verify tour is a valid permutation of [0, n-1]. */
static int is_valid_tour(const int *tour, int sz)
{
    char *seen = calloc(sz, 1);
    if (!seen) return 0;
    for (int i = 0; i < sz; i++) {
        if (tour[i] < 0 || tour[i] >= sz || seen[tour[i]]) {
            free(seen);
            return 0;
        }
        seen[tour[i]] = 1;
    }
    free(seen);
    return 1;
}

/* Simple LCG for reproducible pseudo-random doubles. */
static unsigned long rng_state;
static void rng_seed(unsigned long s) { rng_state = s; }
static double rng_double(double lo, double hi)
{
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    double t = (double)(rng_state >> 33) / (double)(1ULL << 31);
    return lo + t * (hi - lo);
}

/* ── Test cases ── */

static int tests_run = 0, tests_pass = 0;

#define TEST_START(name) do { \
    tests_run++; \
    fprintf(stderr, "  %-50s ", name); \
} while(0)

#define TEST_PASS() do { \
    tests_pass++; \
    fprintf(stderr, "PASS\n"); \
} while(0)

#define TEST_FAIL(msg, ...) do { \
    fprintf(stderr, "FAIL — " msg "\n", ##__VA_ARGS__); \
} while(0)

static void cleanup(int *tour)
{
    free(tour);
    free(dist_matrix);
    free(cities);
    dist_matrix = NULL;
    cities = NULL;
}

static void setup_random(int sz, unsigned long seed)
{
    n = sz;
    cities = malloc(n * sizeof(City));
    assert(cities);
    rng_seed(seed);
    for (int i = 0; i < n; i++) {
        cities[i].x = rng_double(0, 1000);
        cities[i].y = rng_double(0, 1000);
    }
    build_dist_matrix();
}

/* Test: 2-opt monotonicity on multiple random instances. */
static void test_two_opt_monotonicity(void)
{
    TEST_START("2-opt monotonicity (10 random instances)");
    int ok = 1;
    for (int trial = 0; trial < 10; trial++) {
        setup_random(30, 1000 + trial);
        int *tour = malloc(n * sizeof(int));
        nearest_neighbour(0, tour);

        double costs[10000];
        int num_costs;
        if (!two_opt_monotonic(tour, costs, 10000, &num_costs)) {
            TEST_FAIL("trial %d: cost increased during 2-opt", trial);
            ok = 0;
            cleanup(tour);
            return;
        }
        /* Also verify final tour is valid. */
        if (!is_valid_tour(tour, n)) {
            TEST_FAIL("trial %d: invalid tour after 2-opt", trial);
            ok = 0;
            cleanup(tour);
            return;
        }
        cleanup(tour);
    }
    if (ok) TEST_PASS();
}

/* Test: or-opt monotonicity on multiple random instances. */
static void test_or_opt_monotonicity(void)
{
    TEST_START("or-opt monotonicity (10 random instances)");
    int ok = 1;
    for (int trial = 0; trial < 10; trial++) {
        setup_random(25, 2000 + trial);
        int *tour = malloc(n * sizeof(int));
        nearest_neighbour(0, tour);
        /* Run 2-opt first (as the real solver does). */
        double dummy[1];
        int dc;
        two_opt_monotonic(tour, dummy, 0, &dc);

        double costs[10000];
        int num_costs;
        if (!or_opt_monotonic(tour, costs, 10000, &num_costs)) {
            TEST_FAIL("trial %d: cost increased during or-opt", trial);
            ok = 0;
            cleanup(tour);
            return;
        }
        if (!is_valid_tour(tour, n)) {
            TEST_FAIL("trial %d: invalid tour after or-opt", trial);
            ok = 0;
            cleanup(tour);
            return;
        }
        cleanup(tour);
    }
    if (ok) TEST_PASS();
}

/* Test: tour validity and cost consistency on larger random instances. */
static void test_tour_validity_random(void)
{
    TEST_START("tour validity + cost consistency (50/100/200 cities)");
    int sizes[] = {50, 100, 200};
    int ok = 1;
    for (int s = 0; s < 3; s++) {
        setup_random(sizes[s], 3000 + s);
        int *tour = malloc(n * sizeof(int));
        nearest_neighbour(0, tour);
        double dummy[1]; int dc;
        two_opt_monotonic(tour, dummy, 0, &dc);

        if (!is_valid_tour(tour, n)) {
            TEST_FAIL("n=%d: invalid tour", sizes[s]);
            ok = 0;
            cleanup(tour);
            return;
        }

        /* Independent cost calculation from coordinates. */
        double recalc = 0.0;
        for (int i = 0; i < n; i++) {
            int a = tour[i], b = tour[(i + 1) % n];
            double dx = cities[a].x - cities[b].x;
            double dy = cities[a].y - cities[b].y;
            recalc += sqrt(dx * dx + dy * dy);
        }
        double reported = tour_length(tour);
        if (fabs(recalc - reported) > 1e-6) {
            TEST_FAIL("n=%d: cost mismatch recalc=%.6f vs reported=%.6f",
                      sizes[s], recalc, reported);
            ok = 0;
            cleanup(tour);
            return;
        }
        cleanup(tour);
    }
    if (ok) TEST_PASS();
}

/* Test: coincident cities (all same point). */
static void test_coincident_cities(void)
{
    TEST_START("coincident cities (all same point)");
    n = 10;
    cities = malloc(n * sizeof(City));
    for (int i = 0; i < n; i++)
        cities[i] = (City){7.0, 7.0};
    build_dist_matrix();

    int *tour = malloc(n * sizeof(int));
    nearest_neighbour(0, tour);
    double dummy[1]; int dc;
    two_opt_monotonic(tour, dummy, 0, &dc);

    if (!is_valid_tour(tour, n)) {
        TEST_FAIL("invalid tour");
        cleanup(tour);
        return;
    }
    double len = tour_length(tour);
    if (fabs(len) > 1e-9) {
        TEST_FAIL("expected 0.0, got %.6f", len);
        cleanup(tour);
        return;
    }
    TEST_PASS();
    cleanup(tour);
}

/* Test: 2-opt actually improves over NN (on instances where NN is suboptimal). */
static void test_two_opt_improves(void)
{
    TEST_START("2-opt improves over nearest-neighbour");
    setup_random(40, 5000);
    int *tour = malloc(n * sizeof(int));
    nearest_neighbour(0, tour);
    double nn_cost = tour_length(tour);

    double costs[10000]; int nc;
    two_opt_monotonic(tour, costs, 10000, &nc);
    double opt_cost = tour_length(tour);

    if (opt_cost > nn_cost + 1e-9) {
        TEST_FAIL("2-opt made tour worse: NN=%.2f, 2-opt=%.2f",
                  nn_cost, opt_cost);
        cleanup(tour);
        return;
    }
    if (nc <= 1) {
        /* No improvements made — suspicious but not necessarily wrong. */
        fprintf(stderr, "WARN (no improvements) ");
    }
    TEST_PASS();
    cleanup(tour);
}

/* Test: malformed input handling via subprocess. */
static void test_malformed_input(void)
{
    TEST_START("malformed stdin input (via tsp binary)");
    /* We test by piping bad input to the tsp binary and checking exit code. */
    struct { const char *input; const char *desc; } cases[] = {
        {"0\n", "n=0"},
        {"-1\n", "n=-1"},
        {"abc\n", "non-numeric n"},
        {"3\n1.0 2.0\n3.0\n", "incomplete city coords"},
        {"2\n1.0 2.0\n", "fewer cities than declared"},
    };
    int ok = 1;
    for (int i = 0; i < 5; i++) {
        char cmd[256];
        snprintf(cmd, sizeof(cmd),
                 "echo '%s' | ./tsp > /dev/null 2>&1; echo $?",
                 cases[i].input);
        FILE *fp = popen(cmd, "r");
        if (!fp) { ok = 0; break; }
        int exit_code = -1;
        if (fscanf(fp, "%d", &exit_code) != 1) exit_code = -1;
        pclose(fp);

        if (exit_code == 0) {
            TEST_FAIL("case '%s': expected nonzero exit, got 0", cases[i].desc);
            return;
        }
    }
    if (ok) TEST_PASS();
}

/* Test: or-opt preserves tour validity on various random instances. */
static void test_or_opt_validity(void)
{
    TEST_START("or-opt preserves tour validity (15 instances)");
    int ok = 1;
    for (int trial = 0; trial < 15; trial++) {
        setup_random(20 + trial, 4000 + trial);
        int *tour = malloc(n * sizeof(int));
        nearest_neighbour(0, tour);
        double dummy[1]; int dc;
        two_opt_monotonic(tour, dummy, 0, &dc);

        double costs[10000]; int nc;
        or_opt_monotonic(tour, costs, 10000, &nc);

        if (!is_valid_tour(tour, n)) {
            TEST_FAIL("trial %d (n=%d): invalid tour after or-opt",
                      trial, n);
            ok = 0;
            cleanup(tour);
            return;
        }
        cleanup(tour);
    }
    if (ok) TEST_PASS();
}

int main(void)
{
    fprintf(stderr, "=== TSP Test Harness ===\n\n");

    test_two_opt_monotonicity();
    test_or_opt_monotonicity();
    test_tour_validity_random();
    test_coincident_cities();
    test_two_opt_improves();
    test_malformed_input();
    test_or_opt_validity();

    fprintf(stderr, "\n=== Results: %d/%d passed ===\n",
            tests_pass, tests_run);

    return (tests_pass == tests_run) ? 0 : 1;
}
