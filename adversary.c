// adversary.c

#define _GNU_SOURCE
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <omp.h>

// Array length: 200 million elements (~1.6 GiB per array of doubles)
#define SIZE 50000000

int main() {
    // Allocate three arrays of doubles, aligned to 64 B for cache
    double *a = aligned_alloc(64, SIZE * sizeof(double));
    double *b = aligned_alloc(64, SIZE * sizeof(double));
    double *c = aligned_alloc(64, SIZE * sizeof(double));
    if (!a || !b || !c) {
        perror("aligned_alloc");
        return EXIT_FAILURE;
    }

    // Determine number of OpenMP threads (set via OMP_NUM_THREADS)
    int threads = omp_get_max_threads();
    printf("Initializing arrays with %d threads...\n", threads);

    // Parallel first-touch initialization to distribute pages across NUMA
    #pragma omp parallel for schedule(static)
    for (size_t i = 0; i < SIZE; ++i) {
        a[i] = 1.0;
        b[i] = 2.0;
        c[i] = 0.0;
    }

    const double scalar = 3.0;

    while (1) {
        #pragma omp parallel for schedule(static)
        for (size_t i = 0; i < SIZE; ++i) {
            c[i] = a[i] + scalar * b[i];
        }
    }

    return EXIT_SUCCESS;
}
