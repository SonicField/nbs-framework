/*
 * tsp.c — Heuristic TSP solver
 *
 * Nearest-neighbour construction + 2-opt local search.
 * Reads Euclidean city coordinates from stdin, outputs a tour.
 *
 * Input format:
 *   N
 *   x0 y0
 *   x1 y1
 *   ...
 *
 * Output:
 *   Tour as ordered city indices (0-based), one per line,
 *   followed by total distance.
 *
 * Compile: gcc -O2 -Wall -o tsp tsp.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <float.h>

typedef struct {
    double x, y;
} City;

static int n;
static City *cities;
static double *dist_matrix;  /* n x n, row-major */

static double city_dist(int i, int j)
{
    double dx = cities[i].x - cities[j].x;
    double dy = cities[i].y - cities[j].y;
    return sqrt(dx * dx + dy * dy);
}

static void build_dist_matrix(void)
{
    dist_matrix = malloc((size_t)n * (size_t)n * sizeof(double));
    if (!dist_matrix) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
    for (int i = 0; i < n; i++) {
        dist_matrix[i * n + i] = 0.0;
        for (int j = i + 1; j < n; j++) {
            double d = city_dist(i, j);
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

/* Nearest-neighbour heuristic starting from city `start`. */
static void nearest_neighbour(int start, int *tour)
{
    char *visited = calloc(n, 1);
    if (!visited) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }
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

/* Reverse tour[i..j] in place. */
static void reverse_segment(int *tour, int i, int j)
{
    while (i < j) {
        int tmp = tour[i];
        tour[i] = tour[j];
        tour[j] = tmp;
        i++;
        j--;
    }
}

/* 2-opt local search. Returns final tour length. */
static double two_opt(int *tour)
{
    int improved = 1;
    while (improved) {
        improved = 0;
        for (int i = 0; i < n - 1; i++) {
            for (int j = i + 2; j < n; j++) {
                /* Skip the wrap-around edge case where i=0, j=n-1
                   (same edge). */
                if (i == 0 && j == n - 1)
                    continue;

                int a = tour[i], b = tour[i + 1];
                int c = tour[j], d = tour[(j + 1) % n];

                double old_d = DIST(a, b) + DIST(c, d);
                double new_d = DIST(a, c) + DIST(b, d);

                if (new_d < old_d - 1e-10) {
                    reverse_segment(tour, i + 1, j);
                    improved = 1;
                }
            }
        }
    }
    return tour_length(tour);
}

/* Or-opt: move a segment of 1, 2, or 3 cities to a better position. */
static double or_opt(int *tour)
{
    int *tmp = malloc(n * sizeof(int));
    if (!tmp) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }

    int improved = 1;
    while (improved) {
        improved = 0;
        for (int seg_len = 1; seg_len <= 3 && seg_len < n - 1; seg_len++) {
            for (int i = 0; i < n; i++) {
                /* Segment: tour[i], tour[i+1], ..., tour[i+seg_len-1]
                   (indices mod n) */
                int prev_i = (i - 1 + n) % n;
                int end_i = (i + seg_len - 1) % n;
                int next_end = (i + seg_len) % n;

                double remove_cost = DIST(tour[prev_i], tour[i])
                                   + DIST(tour[end_i], tour[next_end]);
                double close_cost = DIST(tour[prev_i], tour[next_end]);
                double seg_gain = remove_cost - close_cost;

                /* Try inserting the segment between every other pair. */
                for (int j = 0; j < n; j++) {
                    /* Skip positions that overlap with the segment. */
                    int skip = 0;
                    for (int k = 0; k < seg_len; k++) {
                        if (j == (i + k) % n || j == prev_i) {
                            skip = 1;
                            break;
                        }
                    }
                    if (skip) continue;

                    int next_j = (j + 1) % n;
                    double insert_cost = DIST(tour[j], tour[i])
                                       + DIST(tour[end_i], tour[next_j])
                                       - DIST(tour[j], tour[next_j]);

                    if (seg_gain - insert_cost > 1e-10) {
                        /* Rebuild tour: remove segment, insert at j. */
                        int idx = 0;
                        /* Copy from next_end to j (skipping segment). */
                        int pos = next_end;
                        while (pos != (j + 1) % n) {
                            tmp[idx++] = tour[pos];
                            pos = (pos + 1) % n;
                        }
                        /* Insert the segment. */
                        for (int k = 0; k < seg_len; k++)
                            tmp[idx++] = tour[(i + k) % n];
                        /* Copy remaining. */
                        pos = (j + 1) % n;
                        while (idx < n) {
                            /* Avoid re-adding segment cities. */
                            int is_seg = 0;
                            for (int k = 0; k < seg_len; k++) {
                                if (pos == (i + k) % n) {
                                    is_seg = 1;
                                    break;
                                }
                            }
                            if (is_seg) {
                                pos = (pos + 1) % n;
                                continue;
                            }
                            tmp[idx++] = tour[pos];
                            pos = (pos + 1) % n;
                        }
                        memcpy(tour, tmp, n * sizeof(int));
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
    return tour_length(tour);
}

/* Try NN from every starting city, apply 2-opt + or-opt, keep best. */
static double solve(int *best_tour)
{
    int *tour = malloc(n * sizeof(int));
    if (!tour) {
        fprintf(stderr, "Out of memory\n");
        exit(1);
    }

    double best_len = DBL_MAX;

    /* For large n, limit the number of starting cities to keep runtime
       polynomial. O(n) starts × O(n^2) 2-opt = O(n^3). */
    int max_starts = n;
    if (max_starts > 50)
        max_starts = 50;

    for (int s = 0; s < max_starts; s++) {
        int start = (max_starts == n) ? s : (s * n / max_starts);
        nearest_neighbour(start, tour);
        two_opt(tour);
        or_opt(tour);

        double len = tour_length(tour);
        if (len < best_len) {
            best_len = len;
            memcpy(best_tour, tour, n * sizeof(int));
        }
    }

    free(tour);
    return best_len;
}

/* Built-in self-test. */
static int run_tests(void)
{
    int pass = 1;

    /* Test 1: Square — 4 cities at corners of unit square.
       Optimal tour = 4.0 */
    {
        n = 4;
        cities = malloc(4 * sizeof(City));
        cities[0] = (City){0, 0};
        cities[1] = (City){1, 0};
        cities[2] = (City){1, 1};
        cities[3] = (City){0, 1};
        build_dist_matrix();

        int *tour = malloc(n * sizeof(int));
        double len = solve(tour);

        /* Verify valid tour. */
        char *seen = calloc(n, 1);
        for (int i = 0; i < n; i++) {
            if (tour[i] < 0 || tour[i] >= n || seen[tour[i]]) {
                fprintf(stderr, "FAIL test1: invalid tour\n");
                pass = 0;
                break;
            }
            seen[tour[i]] = 1;
        }

        if (fabs(len - 4.0) > 1e-6) {
            fprintf(stderr, "FAIL test1: expected 4.0, got %.6f\n", len);
            pass = 0;
        } else {
            fprintf(stderr, "PASS test1: square (len=%.6f)\n", len);
        }

        free(seen);
        free(tour);
        free(dist_matrix);
        free(cities);
    }

    /* Test 2: Triangle — 3 equilateral cities.
       Optimal = 3 * side_length */
    {
        n = 3;
        cities = malloc(3 * sizeof(City));
        cities[0] = (City){0, 0};
        cities[1] = (City){1, 0};
        cities[2] = (City){0.5, sqrt(3.0) / 2.0};
        build_dist_matrix();

        int *tour = malloc(n * sizeof(int));
        double len = solve(tour);
        double expected = 3.0;

        if (fabs(len - expected) > 1e-6) {
            fprintf(stderr, "FAIL test2: expected %.6f, got %.6f\n",
                    expected, len);
            pass = 0;
        } else {
            fprintf(stderr, "PASS test2: triangle (len=%.6f)\n", len);
        }

        free(tour);
        free(dist_matrix);
        free(cities);
    }

    /* Test 3: Collinear — 5 cities on a line.
       Optimal = 2 * (max - min) = 8.0 */
    {
        n = 5;
        cities = malloc(5 * sizeof(City));
        cities[0] = (City){0, 0};
        cities[1] = (City){1, 0};
        cities[2] = (City){2, 0};
        cities[3] = (City){3, 0};
        cities[4] = (City){4, 0};
        build_dist_matrix();

        int *tour = malloc(n * sizeof(int));
        double len = solve(tour);

        if (fabs(len - 8.0) > 1e-6) {
            fprintf(stderr, "FAIL test3: expected 8.0, got %.6f\n", len);
            pass = 0;
        } else {
            fprintf(stderr, "PASS test3: collinear (len=%.6f)\n", len);
        }

        free(tour);
        free(dist_matrix);
        free(cities);
    }

    /* Test 4: Single city — tour length = 0 */
    {
        n = 1;
        cities = malloc(sizeof(City));
        cities[0] = (City){5, 5};
        build_dist_matrix();

        int *tour = malloc(sizeof(int));
        double len = solve(tour);

        if (tour[0] != 0 || fabs(len) > 1e-6) {
            fprintf(stderr, "FAIL test4: single city, got len=%.6f\n", len);
            pass = 0;
        } else {
            fprintf(stderr, "PASS test4: single city (len=%.6f)\n", len);
        }

        free(tour);
        free(dist_matrix);
        free(cities);
    }

    /* Test 5: Two cities */
    {
        n = 2;
        cities = malloc(2 * sizeof(City));
        cities[0] = (City){0, 0};
        cities[1] = (City){3, 4};
        build_dist_matrix();

        int *tour = malloc(2 * sizeof(int));
        double len = solve(tour);
        double expected = 10.0; /* 5 + 5 */

        if (fabs(len - expected) > 1e-6) {
            fprintf(stderr, "FAIL test5: expected %.6f, got %.6f\n",
                    expected, len);
            pass = 0;
        } else {
            fprintf(stderr, "PASS test5: two cities (len=%.6f)\n", len);
        }

        free(tour);
        free(dist_matrix);
        free(cities);
    }

    return pass;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--test") == 0)
        return run_tests() ? 0 : 1;

    if (scanf("%d", &n) != 1 || n < 1) {
        fprintf(stderr, "Invalid input: expected N >= 1\n");
        return 1;
    }

    cities = malloc(n * sizeof(City));
    if (!cities) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    for (int i = 0; i < n; i++) {
        if (scanf("%lf %lf", &cities[i].x, &cities[i].y) != 2) {
            fprintf(stderr, "Invalid input at city %d\n", i);
            return 1;
        }
    }

    build_dist_matrix();

    int *tour = malloc(n * sizeof(int));
    if (!tour) {
        fprintf(stderr, "Out of memory\n");
        return 1;
    }

    double len = solve(tour);

    printf("Tour:\n");
    for (int i = 0; i < n; i++)
        printf("%d\n", tour[i]);
    printf("Distance: %.6f\n", len);

    free(tour);
    free(dist_matrix);
    free(cities);
    return 0;
}
