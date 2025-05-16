// SEQUENTIAL 8 MB
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>

// size of buffer to touch (e.g. 8 MB)
#define BUF_SIZE (8UL * 1024 * 1024)
#define STRIDE    64            // cache‐line size

void* worker(void* arg) {
    volatile uint8_t* buf = malloc(BUF_SIZE);
    if (!buf) { perror("malloc"); return NULL; }

    while (1) {
        // sequential R‐M‐W through the buffer
        for (size_t i = 0; i < BUF_SIZE; i += STRIDE) {
            buf[i] = buf[i] + 1;
        }
    }
    return NULL;
}

int main(int argc, char** argv) {
    int threads = (argc > 1) ? atoi(argv[1]) : 1;
    pthread_t tid[threads];
    for (int t = 0; t < threads; t++)
        pthread_create(&tid[t], NULL, worker, NULL);
    for (int t = 0; t < threads; t++)
        pthread_join(tid[t], NULL);
    return 0;
}
