// Random using RDTSC 512 MB
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <x86intrin.h>
#include <time.h>
#include <unistd.h>

// size of buffer to touch (e.g. 512 MB)
#define BUF_SIZE   (512UL * 1024 * 1024)
#define STRIDE     64              // cache-line size
#define ADDR_MASK  ((1UL << 29) - 1)

void* worker(void* arg) {
    // seed per-thread RNG
    unsigned int seed = time(NULL) ^ (uintptr_t)pthread_self();
    volatile uint8_t* buf = malloc(BUF_SIZE);
    if (!buf) {
        perror("malloc");
        return NULL;
    }

    size_t max_index = BUF_SIZE / STRIDE;

    while (1) {
        // random R–M–W through the buffer
        for (size_t i = 0; i < BUF_SIZE; i+=STRIDE) {
            // generate a random cache-line index
            unsigned long long tsc = __rdtsc();
            uint32_t low  = (uint32_t)tsc;
            uint32_t high = (uint32_t)(tsc >> 32);
            uint32_t rnd  = low ^ high;
            size_t offset = (rnd & ADDR_MASK);
            buf[offset] += 1;
        }
        // light CPU-heavy inner loop
        
    }

    // never reached
    free((void*)buf);
    return NULL;
}

int main(int argc, char** argv) {
    int threads = (argc > 1) ? atoi(argv[1]) : 1;
    pthread_t tid[threads];

    for (int t = 0; t < threads; t++) {
        if (pthread_create(&tid[t], NULL, worker, NULL)) {
            perror("pthread_create");
            exit(EXIT_FAILURE);
        }
    }
    for (int t = 0; t < threads; t++) {
        pthread_join(tid[t], NULL);
    }

    return 0;
}
