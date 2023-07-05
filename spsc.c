#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define CACHE_LINE_SIZE 64 // Common cache line size

#define QUEUE_SIZE 4096
#define WRKR_BATCH_SIZE 16

typedef struct {
    size_t capacity;
    _Alignas(CACHE_LINE_SIZE) _Atomic uint64_t writeIdx;
    _Alignas(CACHE_LINE_SIZE) uint64_t readIdxCache;
    _Alignas(CACHE_LINE_SIZE) _Atomic uint64_t readIdx;
    _Alignas(CACHE_LINE_SIZE) uint64_t writeIdxCache;
    _Alignas(CACHE_LINE_SIZE) void* slots[0]; // FAM for void pointer type slots
} SPSCQueue;

// Function to create a new queue
SPSCQueue* create_queue(size_t capacity) {
    SPSCQueue* queue = (SPSCQueue*) aligned_alloc(CACHE_LINE_SIZE, sizeof(SPSCQueue) + sizeof(void*) * capacity);
    queue->capacity = capacity;
    atomic_init(&queue->writeIdx, 0);
    atomic_init(&queue->readIdx, 0);
    queue->writeIdxCache = 0;
    queue->readIdxCache = 0;
    return queue;
}

// Function to destroy a queue
void destroy_queue(SPSCQueue* queue) {
    free(queue);
}

// Function to push an element into the queue.
// This should be called from a single producer thread.
bool try_push(SPSCQueue* queue, void* value) {
    uint64_t writeIdx = atomic_load_explicit(&queue->writeIdx, memory_order_relaxed);
    uint64_t nextWriteIdx = writeIdx + 1;
    // If the queue is not full
    uint64_t newsize = nextWriteIdx - queue->readIdxCache;
    if(newsize <= queue->capacity) {
        queue->slots[writeIdx % queue->capacity] = value;
        atomic_store_explicit(&queue->writeIdx, nextWriteIdx, memory_order_release);
        return true;
    }
    // Update the cached index and retry
    queue->readIdxCache = atomic_load_explicit(&queue->readIdx, memory_order_acquire);
    newsize = nextWriteIdx - queue->readIdxCache;
    if(newsize <= queue->capacity) {
        queue->slots[writeIdx % queue->capacity] = value;
        atomic_store_explicit(&queue->writeIdx, nextWriteIdx, memory_order_release);
        return true;
    }
    // Queue was full
    return false;
}

// Function to pop an element from the queue.
// This should be called from a single consumer thread.
bool _try_pop(SPSCQueue* queue, void** value) {
    uint64_t readIdx = atomic_load_explicit(&queue->readIdx, memory_order_relaxed);
    // If the queue is not empty
    if(readIdx < queue->writeIdxCache) {
        *value = queue->slots[readIdx % queue->capacity];
        atomic_store_explicit(&queue->readIdx, readIdx + 1, memory_order_release);
        return true;
    }
    // Update the cached index and retry
    queue->writeIdxCache = atomic_load_explicit(&queue->writeIdx, memory_order_acquire);
    if(readIdx < queue->writeIdxCache) {
        *value = queue->slots[readIdx % queue->capacity];
        atomic_store_explicit(&queue->readIdx, readIdx + 1, memory_order_release);
        return true;
    }
    // Queue was empty
    return false;
}

// Function to pop an element from the queue.
// This can be called from multiple consumer threads.
bool try_pop(SPSCQueue* queue, void** value) {
    uint64_t readIdx, newReadIdx;
    void *rval;
    do {
        readIdx = atomic_load_explicit(&queue->readIdx, memory_order_relaxed);
        // If the queue is not empty
        if(readIdx < queue->writeIdxCache) {
            rval = queue->slots[readIdx % queue->capacity];
            newReadIdx = readIdx + 1;
        } else {
            // Update the cached index and retry
            queue->writeIdxCache = atomic_load_explicit(&queue->writeIdx, memory_order_acquire);
            if(readIdx < queue->writeIdxCache) {
                rval = queue->slots[readIdx % queue->capacity];
                newReadIdx = readIdx + 1;
            } else {
                // Queue was empty
                return false;
            }
        }
    } while(!atomic_compare_exchange_weak_explicit(&queue->readIdx, &readIdx, newReadIdx,
                                                   memory_order_release, memory_order_relaxed));
    *value = rval;
    return true;
}

#define _MIN(a, b) ((a) > (b) ? (b) : (a))

#include <pthread.h>

size_t try_pop_many(SPSCQueue* queue, void** values, size_t howmany) {
    uint64_t readIdx, newReadIdx;
    void **rval = alloca(sizeof(*values) * howmany);
    size_t rnum;
    do {
        rnum = 0;
        readIdx = atomic_load_explicit(&queue->readIdx, memory_order_relaxed);
        // If the queue is not empty
        if(readIdx >= queue->writeIdxCache) {
            // Update the cached index and retry
            queue->writeIdxCache = atomic_load_explicit(&queue->writeIdx, memory_order_acquire);
            if(readIdx == queue->writeIdxCache) {
                // Queue was empty
                return 0;
            }
            assert(readIdx < queue->writeIdxCache);
        }
        newReadIdx = queue->writeIdxCache;
        uint64_t i;
        for (i = readIdx; i != newReadIdx && rnum < howmany; i++) {
            rval[rnum] = queue->slots[i % queue->capacity];
            rnum++;
        }
        newReadIdx = i;
    } while(!atomic_compare_exchange_weak_explicit(&queue->readIdx, &readIdx, newReadIdx,
      memory_order_release, memory_order_relaxed));
    memcpy(values, rval, rnum * sizeof(*values));
    return rnum;
}


#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <inttypes.h>

#define NUM_SECONDS 10

typedef struct {
    SPSCQueue* queue;
    uint64_t count;
    uint64_t chksum;
} WorkerArgs;

void* worker_thread(void* arg) {
    WorkerArgs* args = (WorkerArgs*) arg;
    SPSCQueue* queue = args->queue;
    void* values[WRKR_BATCH_SIZE] = {};
    uint64_t last_value = 0;
    struct timespec delay = {};
    int sleepcycles = 0;

    while (1) {
        size_t n = try_pop_many(queue, values, WRKR_BATCH_SIZE);
        if (n > 0) {
            for (size_t i = 0; i < n; i++) {
                if (values[i] == (void*)-1) {
                    goto out;
                }
                uint64_t current_value = (uint64_t)values[i];
                if (current_value <= last_value) {
                    printf("Error: Expected value greater than %" PRIu64 " but got %" PRIu64 "\n", last_value, current_value);
                    abort();
                    exit(EXIT_FAILURE);
                }
                last_value = current_value;
                args->count += 1;
                args->chksum += current_value;
            }
            sleepcycles = sleepcycles * (QUEUE_SIZE - n) / QUEUE_SIZE;
        } else {
            sleepcycles += 1;
        }

        for(volatile size_t i = 0; i < sleepcycles / QUEUE_SIZE; i++) {
            continue;
        }

        //delay.tv_nsec = random() % 100;
        //delay.tv_nsec = 1;
        //nanosleep(&delay, NULL);
    }
out:
    return NULL;
}

#define SEC(x)   ((x)->tv_sec)
#define NSEC(x)  ((x)->tv_nsec)
#define timespec2dtime(s) ((double)SEC(s) + \
  (double)NSEC(s) / 1000000000.0)

int main() {
    SPSCQueue* queue = create_queue(QUEUE_SIZE);
    pthread_t worker;
    WorkerArgs args = {.queue = queue};
    struct timespec st = {}, et = {};

    if (pthread_create(&worker, NULL, worker_thread, &args)) {
        fprintf(stderr, "Error creating thread\n");
        return 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &st);
    double stime = timespec2dtime(&st);
    double etime = 0;
    uintptr_t i = 0;
    uintptr_t disc = 0;
    uint64_t chksum = 0;
    while (etime < stime + NUM_SECONDS) {
        while (!try_push(queue, (void*) i + 1)) {
            struct timespec delay = {.tv_nsec = 1};
            nanosleep(&delay, NULL);
            void *junk;
            if (try_pop(queue, &junk)) {
                chksum -= (uintptr_t)junk;
                disc++;
            }
        }
        chksum += i + 1;
        if ((((1 << 13) - 1) & i) == 0) {
            clock_gettime(CLOCK_MONOTONIC, &et);
            etime = timespec2dtime(&et);
        }
        i++;
    }
    while (!try_push(queue, (void*)-1)) { pthread_yield(); } // Add EOF marker

    // Wait for the worker thread to exit
    if (pthread_join(worker, NULL)) {
        fprintf(stderr, "Error joining thread\n");
        return 2;
    }

    assert(chksum == args.chksum);
    printf("Sent %" PRIu64 " + %" PRIu64 ", received %" PRIu64 " messages in %d seconds\n", i - disc, disc, args.count, NUM_SECONDS);
    printf("PPS is %.3f MPPS, packet loss rate %.4f%%\n", 1e-6 * (double)(i - disc) / (etime - stime), 100.0 * (double)disc / (double)i);

    destroy_queue(queue);
    return 0;
}
