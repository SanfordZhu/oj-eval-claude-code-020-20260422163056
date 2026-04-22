#include "buddy.h"
#include <stdint.h>
#include <stdlib.h>

#define PAGE_SIZE 4096
#define MAX_RANK_LIMIT 16

static void *base = NULL;
static int n_pages = 0;
static int max_rank = 0;

static int *next_free = NULL;   // next pointer for free-list nodes (by start page index)
static int *prev_free = NULL;   // prev pointer for free-list nodes
static int head_free[MAX_RANK_LIMIT + 1]; // free list heads for ranks [1..16]
static unsigned char *is_free = NULL;     // whether a start page index currently represents a free block
static unsigned char *is_alloc = NULL;    // whether a start page index currently represents an allocated block
static unsigned char *rank_arr = NULL;    // current block rank for start indices (free or allocated)

static int is_power_of_two(int x) { return x > 0 && (x & (x - 1)) == 0; }

static int calc_max_rank(int pages) {
    int r = 1, v = 1;
    while (v < pages && r < MAX_RANK_LIMIT) { v <<= 1; r++; }
    return (v == pages) ? r : 0;
}

static void insert_free(int r, int idx) {
    prev_free[idx] = -1;
    next_free[idx] = head_free[r];
    if (head_free[r] != -1) prev_free[head_free[r]] = idx;
    head_free[r] = idx;
    is_free[idx] = 1;
    rank_arr[idx] = (unsigned char)r;
}

static void remove_free(int r, int idx) {
    int p = prev_free[idx];
    int n = next_free[idx];
    if (p == -1) head_free[r] = n; else next_free[p] = n;
    if (n != -1) prev_free[n] = p;
    prev_free[idx] = -1;
    next_free[idx] = -1;
    is_free[idx] = 0;
}

int init_page(void *p, int pgcount){
    base = p;
    n_pages = pgcount;
    max_rank = calc_max_rank(n_pages);

    // allocate/initialize metadata arrays
    free(next_free); free(prev_free); free(is_free); free(is_alloc); free(rank_arr);
    next_free = (int *)malloc(sizeof(int) * n_pages);
    prev_free = (int *)malloc(sizeof(int) * n_pages);
    is_free   = (unsigned char *)malloc(n_pages);
    is_alloc  = (unsigned char *)malloc(n_pages);
    rank_arr  = (unsigned char *)malloc(n_pages);

    for (int i = 0; i < n_pages; ++i) {
        next_free[i] = -1;
        prev_free[i] = -1;
        is_free[i] = 0;
        is_alloc[i] = 0;
        rank_arr[i] = 0;
    }
    for (int r = 1; r <= MAX_RANK_LIMIT; ++r) head_free[r] = -1;

    // Start with one big free block covering the entire pool
    if (max_rank > 0) insert_free(max_rank, 0);

    return OK;
}

void *alloc_pages(int rank){
    if (rank < 1 || rank > max_rank) return ERR_PTR(-EINVAL);

    int found_rank = -1;
    for (int r = rank; r <= max_rank; ++r) {
        if (head_free[r] != -1) { found_rank = r; break; }
    }
    if (found_rank == -1) return ERR_PTR(-ENOSPC);

    int idx = head_free[found_rank];
    remove_free(found_rank, idx);

    // split down to requested rank, always keeping the left half and pushing right halves onto free lists
    for (int r = found_rank; r > rank; --r) {
        int block_pages = 1 << (r - 1);
        int half = block_pages >> 1;
        insert_free(r - 1, idx + half);
    }

    is_alloc[idx] = 1;
    rank_arr[idx] = (unsigned char)rank;

    return (void *)((uintptr_t)base + (uintptr_t)idx * PAGE_SIZE);
}

int return_pages(void *p){
    if (p == NULL) return -EINVAL;
    if (base == NULL || n_pages <= 0) return -EINVAL;

    uintptr_t off = (uintptr_t)p - (uintptr_t)base;
    if (off % PAGE_SIZE != 0) return -EINVAL;
    if (off >= (uintptr_t)n_pages * PAGE_SIZE) return -EINVAL;

    int idx = (int)(off / PAGE_SIZE);
    if (!is_alloc[idx]) return -EINVAL;

    int r = rank_arr[idx];
    is_alloc[idx] = 0;

    // try to coalesce upwards
    while (r < max_rank) {
        int block_pages = 1 << (r - 1);
        int buddy_idx = idx ^ block_pages;
        if (buddy_idx < 0 || buddy_idx >= n_pages) break;
        if (is_free[buddy_idx] && rank_arr[buddy_idx] == (unsigned char)r) {
            // remove buddy from free list and merge
            remove_free(r, buddy_idx);
            // parent start index is the lower of the two
            idx = (buddy_idx < idx) ? buddy_idx : idx;
            r += 1;
            continue;
        }
        break;
    }

    insert_free(r, idx);
    rank_arr[idx] = (unsigned char)r;
    return OK;
}

int query_ranks(void *p){
    if (p == NULL) return -EINVAL;
    if (base == NULL || n_pages <= 0) return -EINVAL;

    uintptr_t off = (uintptr_t)p - (uintptr_t)base;
    if (off % PAGE_SIZE != 0) return -EINVAL;
    if (off >= (uintptr_t)n_pages * PAGE_SIZE) return -EINVAL;

    int idx = (int)(off / PAGE_SIZE);
    if (is_alloc[idx]) return (int)rank_arr[idx];
    if (is_free[idx]) return (int)rank_arr[idx];

    // find the maximum free block rank that covers this page
    for (int r = max_rank; r >= 1; --r) {
        int block_pages = 1 << (r - 1);
        int start = (idx / block_pages) * block_pages;
        if (start >= 0 && start < n_pages && is_free[start] && rank_arr[start] == (unsigned char)r)
            return r;
    }

    return -EINVAL;
}

int query_page_counts(int rank){
    if (rank < 1 || rank > max_rank) return -EINVAL;
    int cnt = 0;
    for (int i = head_free[rank]; i != -1; i = next_free[i]) cnt++;
    return cnt;
}
