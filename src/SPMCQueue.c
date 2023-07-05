#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>

#include "SPMCQueue.h"

#define CACHE_LINE_SIZE 64 // Common cache line size

struct SPMCQueue {
    size_t capacity;
    uint64_t mask;
    _Alignas(CACHE_LINE_SIZE) _Atomic uint64_t writeIdx;
    _Alignas(CACHE_LINE_SIZE) uint64_t readIdxCache;
    _Alignas(CACHE_LINE_SIZE) _Atomic uint64_t readIdx;
    _Alignas(CACHE_LINE_SIZE) uint64_t writeIdxCache;
    _Alignas(CACHE_LINE_SIZE) void* slots[0]; // FAM for void pointer type slots
};

// Function to create a new queue
SPMCQueue *
create_queue(size_t capacity)
{
    SPMCQueue* queue = (SPMCQueue*) aligned_alloc(CACHE_LINE_SIZE, sizeof(SPMCQueue) + sizeof(void*) * capacity);
    if (queue == NULL) {
        return NULL;
    }
    queue->capacity = capacity;
    queue->mask = capacity - 1;
    atomic_init(&queue->writeIdx, 0);
    atomic_init(&queue->readIdx, 0);
    queue->writeIdxCache = 0;
    queue->readIdxCache = 0;
    return queue;
}

// Function to destroy a queue
void destroy_queue(SPMCQueue* queue) {
    free(queue);
}

// Function to push an element into the queue.
// This should be called from a single producer thread.
bool
try_push(SPMCQueue* queue, void* value)
{
    uint64_t writeIdx = atomic_load_explicit(&queue->writeIdx, memory_order_relaxed);
    uint64_t nextWriteIdx = writeIdx + 1;
    // If the queue is not full
    uint64_t newsize = nextWriteIdx - queue->readIdxCache;
    if(newsize <= queue->capacity) {
        queue->slots[writeIdx & queue->mask] = value;
        atomic_store_explicit(&queue->writeIdx, nextWriteIdx, memory_order_release);
        return true;
    }
    // Update the cached index and retry
    queue->readIdxCache = atomic_load_explicit(&queue->readIdx, memory_order_acquire);
    newsize = nextWriteIdx - queue->readIdxCache;
    if(newsize <= queue->capacity) {
        queue->slots[writeIdx & queue->mask] = value;
        atomic_store_explicit(&queue->writeIdx, nextWriteIdx, memory_order_release);
        return true;
    }
    // Queue was full
    return false;
}

// Function to pop an element from the queue.
// This can be called from multiple consumer threads.
bool
try_pop(SPMCQueue* queue, void** value)
{
    uint64_t readIdx, newReadIdx;
    void *rval;
    do {
        readIdx = atomic_load_explicit(&queue->readIdx, memory_order_relaxed);
        // If the queue is not empty
        if (readIdx < queue->writeIdxCache) {
            rval = queue->slots[readIdx & queue->mask];
            newReadIdx = readIdx + 1;
        } else {
            // Update the cached index and retry
            queue->writeIdxCache = atomic_load_explicit(&queue->writeIdx, memory_order_acquire);
            if(readIdx < queue->writeIdxCache) {
                rval = queue->slots[readIdx & queue->mask];
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

size_t
try_pop_many(SPMCQueue* queue, void** values, size_t howmany)
{
    uint64_t readIdx, newReadIdx;

    do {
        readIdx = atomic_load_explicit(&queue->readIdx, memory_order_relaxed);
        // If the queue is not empty
        if (readIdx >= queue->writeIdxCache) {
            // Update the cached index and retry
            queue->writeIdxCache = atomic_load_explicit(&queue->writeIdx, memory_order_acquire);
            if(readIdx == queue->writeIdxCache) {
                // Queue was empty
                return 0;
            }
            assert(readIdx < queue->writeIdxCache);
        }
        //newReadIdx = (readIdx + howmany > queue->writeIdxCache) ? queue->writeIdxCache : readIdx + howmany;
        newReadIdx = readIdx + howmany;
        if (newReadIdx > queue->writeIdxCache)
            newReadIdx = queue->writeIdxCache;
        for (uint64_t i = readIdx; i < newReadIdx; i++) {
            values[i - readIdx] = queue->slots[i & queue->mask];
        }
    } while(!atomic_compare_exchange_weak_explicit(&queue->readIdx, &readIdx, newReadIdx,
      memory_order_release, memory_order_relaxed));
    return (newReadIdx - readIdx);
}
