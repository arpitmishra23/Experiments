// Pointer-chasing random on 18 MB buffer.
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#define BUF_SIZE   (18UL * 1024 * 1024)   // 18 MiB total
#define STRIDE     64                     // cache-line size

static void **chain;    // pointer chain array
static size_t nodes;    // number of nodes in the chain

// Fisher–Yates shuffle for index array
static void shuffle(size_t *idx, size_t n, unsigned int seed) {
    for (size_t i = n - 1; i > 0; --i) {
        size_t j = rand_r(&seed) % (i + 1);
        size_t tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
    }
}

// Build a randomized cyclic pointer chain covering BUF_SIZE
static void build_chain(void) {

    nodes = BUF_SIZE / STRIDE;
    // allocate pointer array
    chain = malloc(nodes * sizeof(void*));
    if (!chain) { perror("malloc chain"); exit(EXIT_FAILURE); }

    // prepare and shuffle indices
    size_t *idx = malloc(nodes * sizeof(size_t));
    if (!idx) { perror("malloc idx"); exit(EXIT_FAILURE); }
    for (size_t i = 0; i < nodes; ++i) idx[i] = i;
    unsigned int seed = time(NULL) ^ getpid();
    shuffle(idx, nodes, seed);

    // link each node to the next in shuffled order
    for (size_t i = 0; i < nodes - 1; ++i) {
        chain[idx[i]] = (void*)&chain[idx[i+1]];
    }
    // close cycle
    chain[idx[nodes - 1]] = (void*)&chain[idx[0]];
    free(idx);

    // pre-touch pages
    for (size_t i = 0; i < nodes; i += (4096/STRIDE)) {
        volatile char *p = (char*)&chain[i]; (void)p;
    }
}

// Worker thread: traverse chain and do R–M–W
static void *worker(void *arg) {
                        //      printf("worker start\n");

    (void)arg;
    void *p = (void*)&chain[0];
    while (1) {
        for (size_t i = 0; i < nodes; ++i) {
            p = *(void* volatile*)p;      // follow pointer
            uint8_t *b = (uint8_t*)p;
            //*b = *b + 1;                  // R-M-W
        }
        for (volatile int k = 0; k < 100; ++k);
    }
                      //printf("worker end\n");

    return NULL;
}

int main(int argc, char **argv) {
    int threads = (argc > 1) ? atoi(argv[1]) : 1;
    if (threads < 1) threads = 1;

    build_chain();

    pthread_t *tids = malloc(threads * sizeof(pthread_t));
    if (!tids) { perror("malloc tids"); exit(EXIT_FAILURE); }

    for (int t = 0; t < threads; ++t) {
        if (pthread_create(&tids[t], NULL, worker, NULL) != 0) {
            perror("pthread_create"); exit(EXIT_FAILURE);
        }
    }

    for (int t = 0; t < threads; ++t) {
        pthread_join(tids[t], NULL);
    }

    return 0;
}
