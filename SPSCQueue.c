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
