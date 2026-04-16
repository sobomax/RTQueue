#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <malloc.h>
#endif

#include "SPMCQueue.h"

#if !defined(CACHE_LINE_SIZE)
#define CACHE_LINE_SIZE 64 // Common cache line size
#endif

#if defined(NDEBUG)
# if defined(_MSC_VER)
#  define SPMC_ASSERT(expr) __assume(expr)
# elif defined(__clang__)
#  if __has_builtin(__builtin_assume)
#   define SPMC_ASSERT(expr) __builtin_assume(expr)
#  else
#   define SPMC_ASSERT(expr) do { if (!(expr)) __builtin_unreachable(); } while (0)
#  endif
# elif defined(__GNUC__)
#  define SPMC_ASSERT(expr) do { if (!(expr)) __builtin_unreachable(); } while (0)
# else
#  define SPMC_ASSERT(expr) ((void)0)
# endif
#else
# define SPMC_ASSERT(expr) assert(expr)
#endif

#define RESERVED_BITS 4

struct SPMCQueue {
    size_t capacity;
    uint64_t mask;
    _Alignas(CACHE_LINE_SIZE) _Atomic uint64_t writeIdx;
    _Alignas(CACHE_LINE_SIZE) uint64_t readIdxCache;
    _Alignas(CACHE_LINE_SIZE) _Atomic uint64_t readIdx;
    _Alignas(CACHE_LINE_SIZE) _Atomic uint64_t writeIdxCache;
    _Alignas(CACHE_LINE_SIZE) void* slots[0]; // FAM for void pointer type slots
};

static size_t
round_up_size(size_t size, size_t alignment)
{
    return (size + alignment - 1) & ~(alignment - 1);
}

static void *
spmc_aligned_alloc(size_t alignment, size_t size)
{
    size_t alloc_size = round_up_size(size, alignment);
#if defined(_WIN32)
    return _aligned_malloc(alloc_size, alignment);
#else
    void *ptr = NULL;
    if (posix_memalign(&ptr, alignment, alloc_size) != 0) {
        return NULL;
    }
    return ptr;
#endif
}

static void
spmc_aligned_free(void *ptr)
{
#if defined(_WIN32)
    _aligned_free(ptr);
#else
    free(ptr);
#endif
}

// Function to create a new queue
SPMCQueue *
create_queue(size_t capacity)
{
    size_t alloc_size = sizeof(SPMCQueue) + sizeof(void*) * capacity;

    assert(capacity > 0);
    assert((capacity & (capacity - 1)) == 0);

    SPMCQueue* queue = (SPMCQueue*) spmc_aligned_alloc(CACHE_LINE_SIZE, alloc_size);
    if (queue == NULL) {
        return NULL;
    }
    queue->capacity = capacity;
    queue->mask = capacity - 1;
    atomic_init(&queue->writeIdx, 0);
    atomic_init(&queue->readIdx, 0);
    atomic_init(&queue->writeIdxCache, 0);
    queue->readIdxCache = 0;
    return queue;
}

// Function to destroy a queue
void destroy_queue(SPMCQueue* queue) {
    spmc_aligned_free(queue);
}

#define LOAD_R_IDX(q, mo) \
    (atomic_load_explicit(&(q)->readIdx,             (mo)))
#define LOAD_W_IDX(q, mo) \
    (atomic_load_explicit(&(q)->writeIdx,            (mo)))
#define LOAD_W_CACHE(q)   \
    (atomic_load_explicit(&(q)->writeIdxCache,       memory_order_relaxed))
#define UPDATE_R_IDX(q, ov, nv) \
    (atomic_compare_exchange_weak_explicit(&(q)->readIdx, &(ov), (nv), \
                                                     memory_order_release, \
                                                     memory_order_relaxed))
#define UPDATE_W_IDX(q, v) \
    (atomic_store_explicit(&(q)->writeIdx,      (v), memory_order_release))
#define UPDATE_W_CACHE(q, v) \
    (atomic_store_explicit(&(q)->writeIdxCache, (v), memory_order_relaxed))

// Function to push an element into the queue.
// This should be called from a single producer thread.
bool
try_push(SPMCQueue* queue, void* value)
{
    uint64_t writeIdx = LOAD_W_IDX(queue, memory_order_relaxed);
    uint64_t nextWriteIdx = writeIdx + 1;
    // If the queue is not full
    uint64_t newsize = nextWriteIdx - queue->readIdxCache;
    if(newsize <= queue->capacity) {
        queue->slots[writeIdx & queue->mask] = value;
        UPDATE_W_IDX(queue, nextWriteIdx);
        return true;
    }
    // Update the cached index and retry
    queue->readIdxCache = LOAD_R_IDX(queue, memory_order_acquire);
    newsize = nextWriteIdx - queue->readIdxCache;
    if (newsize <= queue->capacity) {
        queue->slots[writeIdx & queue->mask] = value;
        UPDATE_W_IDX(queue, nextWriteIdx);
        return true;
    }
    // Queue was full
    return false;
}

size_t
try_push_many(SPMCQueue* queue, void** values, size_t howmany)
{
    uint64_t writeIdx = LOAD_W_IDX(queue, memory_order_relaxed);
    uint64_t readIdx = queue->readIdxCache;
    size_t available = (size_t)(queue->capacity - (writeIdx - readIdx));

    if (available < howmany) {
        readIdx = LOAD_R_IDX(queue, memory_order_acquire);
        queue->readIdxCache = readIdx;
        available = (size_t)(queue->capacity - (writeIdx - readIdx));
    }

    size_t count = howmany;
    if (count > available) {
        count = available;
    }
    if (count == 0) {
        return 0;
    }

    size_t start = (size_t)(writeIdx & queue->mask);
    size_t first_n = queue->capacity - start;

    if (count <= first_n) {
        memcpy(&queue->slots[start], values, count * sizeof(values[0]));
    } else {
        memcpy(&queue->slots[start], values, first_n * sizeof(values[0]));
        memcpy(&queue->slots[0], values + first_n,
          (count - first_n) * sizeof(values[0]));
    }

    UPDATE_W_IDX(queue, writeIdx + count);
    return count;
}

// Function to pop an element from the queue.
// This can be called from multiple consumer threads.
bool
try_pop(SPMCQueue* queue, void** value)
{
    uint64_t readIdx, newReadIdx;
    void *rval;
    do {
        readIdx = LOAD_R_IDX(queue, memory_order_relaxed);
        // If the queue is not empty
        uint64_t writeIdxCache = LOAD_W_CACHE(queue);
        if (readIdx >= writeIdxCache) {
            // Update the cached index and retry
            writeIdxCache = LOAD_W_IDX(queue, memory_order_acquire);
            UPDATE_W_CACHE(queue, writeIdxCache);
            if(readIdx == writeIdxCache) {
                // Queue was empty
                return 0;
            }
            SPMC_ASSERT(readIdx < writeIdxCache);
        }
        newReadIdx = readIdx + 1;
        rval  = queue->slots[readIdx & queue->mask];
    } while (!UPDATE_R_IDX(queue, readIdx, newReadIdx));
    *value = rval;
    return true;
}

size_t
try_pop_many(SPMCQueue* queue, void** values, size_t howmany)
{
    uint64_t readIdx, newReadIdx;

    do {
        readIdx = LOAD_R_IDX(queue, memory_order_relaxed);
        // If the queue is not empty
        uint64_t writeIdxCache = LOAD_W_CACHE(queue);
        if (readIdx >= writeIdxCache) {
            // Update the cached index and retry
            writeIdxCache = LOAD_W_IDX(queue, memory_order_acquire);
            UPDATE_W_CACHE(queue, writeIdxCache);
            if(readIdx == writeIdxCache) {
                // Queue was empty
                return 0;
            }
            SPMC_ASSERT(readIdx < writeIdxCache);
        }
        newReadIdx = readIdx + howmany;
        if (newReadIdx > writeIdxCache)
            newReadIdx = writeIdxCache;
        size_t count = (size_t)(newReadIdx - readIdx);
        size_t start = (size_t)(readIdx & queue->mask);
        size_t first_n = queue->capacity - start;

        if (count <= first_n) {
            memcpy(values, &queue->slots[start], count * sizeof(values[0]));
        } else {
            memcpy(values, &queue->slots[start], first_n * sizeof(values[0]));
            memcpy(values + first_n, &queue->slots[0],
              (count - first_n) * sizeof(values[0]));
        }
    } while (!UPDATE_R_IDX(queue, readIdx, newReadIdx));
    return (newReadIdx - readIdx);
}
