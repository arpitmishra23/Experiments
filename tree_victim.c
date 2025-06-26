#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

// Each node is 64 bytes
typedef struct Node {
    uint64_t      val;
    struct Node  *left, *right;
    uint8_t       pad[40];
} Node;


Node* build_tree(unsigned levels) {
    if (levels == 0) return NULL;
    size_t max_nodes = (1ULL << levels) - 1;
    Node **queue   = malloc(max_nodes * sizeof(Node*));
    if (!queue) { perror("malloc queue"); exit(1); }

    unsigned int seed = 42;  

    Node *root = malloc(sizeof(Node));
    if (!root) { perror("malloc root"); exit(1); }
    root->val = 0;
    root->left = root->right = NULL;

    size_t head = 0, tail = 0, count = 1;
    queue[tail++] = root;

    while (count < max_nodes) {
        Node *parent = queue[head++];
        // left child
        Node *L = malloc(sizeof(Node));
        if (!L) { perror("malloc L"); exit(1); }
        L->val   = count;
        L->left  = L->right = NULL;
        parent->left = L;
        queue[tail++] = L;
        count++;
        // right child
        if (count < max_nodes) {
            Node *R = malloc(sizeof(Node));
            if (!R) { perror("malloc R"); exit(1); }
            R->val   = count;
            R->left  = R->right = NULL;
            parent->right = R;
            queue[tail++] = R;
            count++;
        } else {
            parent->right = NULL;
        }
    }

    free(queue);
    return root;
}

// if val even, go right→left, else left→right.
void traverse(Node *n) {
    if (!n) return;
    if ((n->val % 2) == 0) {
        traverse(n->right);
        traverse(n->left);
    } else {
        traverse(n->left);
        traverse(n->right);
    }
}

int main(int argc, char **argv) {
    // Number of levels in the tree (default 18 → ~256 k nodes)
    unsigned levels = (argc>1 ? atoi(argv[1]) : 18);
    fprintf(stderr,"Building %u-level full tree (%zu nodes)\n",
            levels, (size_t)((1ULL<<levels)-1));

    Node *root = build_tree(levels);

    // Time N sweeps
    const int sweeps = 256;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int s = 0; s < sweeps; s++) {
        traverse(root);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double total = (t1.tv_sec - t0.tv_sec)
                 + (t1.tv_nsec - t0.tv_nsec)*1e-9;
    printf("Tree-traverse: %d sweeps over %zu nodes took %.6f s (avg %.6f s/sweep)\n",
           sweeps, (size_t)((1ULL<<levels)-1), total, total/sweeps);
    return 0;
}
