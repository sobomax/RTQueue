#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>

#define CACHE_LINE_SIZE 64 // Common cache line size

typedef struct {
    _Alignas(CACHE_LINE_SIZE) _Atomic size_t writeIdx;
    size_t readIdxCache;
    _Alignas(CACHE_LINE_SIZE) _Atomic size_t readIdx;
    size_t writeIdxCache;
    _Alignas(CACHE_LINE_SIZE) char padding[CACHE_LINE_SIZE - sizeof(size_t)]; // Padding to avoid false sharing
    size_t capacity;
    _Alignas(CACHE_LINE_SIZE) void* slots[0]; // FAM for void pointer type slots
} SPSCQueue;

// Function to create a new queue
SPSCQueue* create_queue(size_t capacity) {
    SPSCQueue* queue = (SPSCQueue*) aligned_alloc(CACHE_LINE_SIZE, sizeof(SPSCQueue) + sizeof(void*) * capacity);
    queue->capacity = capacity;
    queue->writeIdx = 0;
    queue->readIdx = 0;
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
    size_t writeIdx = atomic_load_explicit(&queue->writeIdx, memory_order_relaxed);
    size_t nextWriteIdx = (writeIdx + 1) % queue->capacity;
    // If the queue is not full
    if(nextWriteIdx != queue->readIdxCache) {
        queue->slots[writeIdx] = value;
        atomic_store_explicit(&queue->writeIdx, nextWriteIdx, memory_order_release);
        return true;
    }
    // Update the cached index and retry
    queue->readIdxCache = atomic_load_explicit(&queue->readIdx, memory_order_acquire);
    if(nextWriteIdx != queue->readIdxCache) {
        queue->slots[writeIdx] = value;
        atomic_store_explicit(&queue->writeIdx, nextWriteIdx, memory_order_release);
        return true;
    }
    // Queue was full
    return false;
}

// Function to pop an element from the queue.
// This should be called from a single consumer thread.
bool _try_pop(SPSCQueue* queue, void** value) {
    size_t readIdx = atomic_load_explicit(&queue->readIdx, memory_order_relaxed);
    // If the queue is not empty
    if(readIdx != queue->writeIdxCache) {
        *value = queue->slots[readIdx];
        atomic_store_explicit(&queue->readIdx, (readIdx + 1) % queue->capacity, memory_order_release);
        return true;
    }
    // Update the cached index and retry
    queue->writeIdxCache = atomic_load_explicit(&queue->writeIdx, memory_order_acquire);
    if(readIdx != queue->writeIdxCache) {
        *value = queue->slots[readIdx];
        atomic_store_explicit(&queue->readIdx, (readIdx + 1) % queue->capacity, memory_order_release);
        return true;
    }
    // Queue was empty
    return false;
}

// Function to pop an element from the queue.
// This can be called from multiple consumer threads.
bool try_pop(SPSCQueue* queue, void** value) {
    size_t readIdx, newReadIdx;
    void *rval;
    do {
        readIdx = atomic_load_explicit(&queue->readIdx, memory_order_relaxed);
        // If the queue is not empty
        if(readIdx != queue->writeIdxCache) {
            rval = queue->slots[readIdx];
            newReadIdx = (readIdx + 1) % queue->capacity;
        } else {
            // Update the cached index and retry
            queue->writeIdxCache = atomic_load_explicit(&queue->writeIdx, memory_order_acquire);
            if(readIdx != queue->writeIdxCache) {
                rval = queue->slots[readIdx];
                newReadIdx = (readIdx + 1) % queue->capacity;
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

#include <assert.h>
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
    void* value = NULL;
    uint64_t last_value = 0;
    struct timespec delay = {};

    while (1) {
        if (try_pop(queue, &value)) {
            if (value == (void*)-1) {
                break; // Exit the thread when EOF marker is found
            }
            uint64_t current_value = (uint64_t) value;
            if (current_value <= last_value) {
                printf("Error: Expected value greater than %" PRIu64 " but got %" PRIu64 "\n", last_value, current_value);
                exit(EXIT_FAILURE);
            }
            last_value = current_value;
            args->count += 1;
            args->chksum += current_value;
        } else {
            pthread_yield();
        }
        delay.tv_nsec = random() % 10000;
        nanosleep(&delay, NULL);
    }
    return NULL;
}

int main() {
    SPSCQueue* queue = create_queue(1024);
    pthread_t worker;
    WorkerArgs args = {.queue = queue};

    if (pthread_create(&worker, NULL, worker_thread, &args)) {
        fprintf(stderr, "Error creating thread\n");
        return 1;
    }

    time_t start_time = time(NULL);
    uintptr_t i = 1;
    uintptr_t disc = 0;
    uint64_t chksum = 0;
    while (time(NULL) < start_time + NUM_SECONDS) {
        while (!try_push(queue, (void*) i)) {
            void *junk;
            if (try_pop(queue, &junk)) {
                chksum -= (uintptr_t)junk;
                disc++;
            }
        }
        chksum += i;
        i++;
    }
    while (!try_push(queue, (void*)-1)) { pthread_yield(); } // Add EOF marker

    // Wait for the worker thread to exit
    if (pthread_join(worker, NULL)) {
        fprintf(stderr, "Error joining thread\n");
        return 2;
    }

    assert(chksum == args.chksum);
    printf("Sent %" PRIu64 ", received %" PRIu64 " messages in %d seconds\n", i - disc, args.count, NUM_SECONDS);

    destroy_queue(queue);
    return 0;
}
