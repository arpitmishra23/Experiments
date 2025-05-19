// process4.c
// Victim process: pointer-chasing R–M–W over a 16 MiB buffer (half of 32 MiB LLC)

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>
#include <string.h>
#include <unistd.h> 

// Target buffer size = 16 MiB
#define BUF_SIZE   (16UL * 1024 * 1024)
#define STRIDE     64                  // cache-line size

static uint8_t *rawbuf;    // data buffer
static uint8_t **chain;    // array of pointers into rawbuf
static size_t nodes;

// Fisher–Yates shuffle for index array
static void shuffle(size_t *idx, size_t n, unsigned int *seed) {
    for (size_t i = n - 1; i > 0; --i) {
        size_t j = rand_r(seed) % (i + 1);
        size_t tmp = idx[i]; idx[i] = idx[j]; idx[j] = tmp;
    }
}

// Build raw buffer and pointer chain covering BUF_SIZE
static void build_chain(void) {
    nodes = BUF_SIZE / STRIDE;
    // allocate buffer aligned to cache lines
    if (posix_memalign((void**)&rawbuf, STRIDE, BUF_SIZE) != 0) {
        perror("posix_memalign rawbuf"); exit(EXIT_FAILURE);
    }
    memset(rawbuf, 0, BUF_SIZE);  // fault pages in

    // allocate chain array
    chain = malloc(nodes * sizeof(uint8_t*));
    if (!chain) { perror("malloc chain"); exit(EXIT_FAILURE); }

    // prepare and shuffle indices
    size_t *idx = malloc(nodes * sizeof(size_t));
    if (!idx) { perror("malloc idx"); exit(EXIT_FAILURE); }
    for (size_t i = 0; i < nodes; ++i) idx[i] = i;
    unsigned int seed = (unsigned int)time(NULL) ^ getpid();
    shuffle(idx, nodes, &seed);

    // fill chain with pointers into rawbuf at randomized offsets
    for (size_t i = 0; i < nodes; ++i) {
        chain[i] = rawbuf + idx[i] * STRIDE;
    }
    free(idx);
}

// Worker thread: iterate the chain array, doing R–M–W on each pointer
static void *worker(void *arg) {
    (void)arg;
    size_t i = 0;
    while (1) {
        // access random location
        uint8_t *p = chain[i];
        *p = *p + 1;
        // advance sweep
        i = (i + 1) % nodes;
    }
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
    for (int t = 0; t < threads; ++t) pthread_join(tids[t], NULL);
    return 0;
}
