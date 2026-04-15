#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "SPMCQueue.h"

static void
expect_pop_many(SPMCQueue* queue, uintptr_t start, size_t count)
{
    void* values[16] = {0};
    size_t total = 0;

    while (total < count) {
        size_t popped = try_pop_many(queue, values, count - total);

        assert(popped > 0);
        for (size_t i = 0; i < popped; i++) {
            assert((uintptr_t)values[i] == start + total + i);
        }
        total += popped;
    }
}

static void
test_try_push_many_basic(void)
{
    SPMCQueue* queue = create_queue(8);
    void* values[] = {(void*)1, (void*)2, (void*)3};

    assert(queue != NULL);
    assert(try_push_many(queue, values, 0) == 0);
    assert(try_push_many(queue, values, 3) == 3);
    expect_pop_many(queue, 1, 3);
    destroy_queue(queue);
}

static void
test_try_push_many_partial_refresh_and_wrap(void)
{
    SPMCQueue* queue = create_queue(8);
    void* first[] = {
        (void*)1, (void*)2, (void*)3, (void*)4,
        (void*)5, (void*)6, (void*)7, (void*)8,
    };
    void* second[] = {(void*)9, (void*)10, (void*)11, (void*)12, (void*)13};

    assert(queue != NULL);
    assert(try_push_many(queue, first, 8) == 8);
    expect_pop_many(queue, 1, 5);

    assert(try_push_many(queue, second, 5) == 5);
    expect_pop_many(queue, 6, 8);
    destroy_queue(queue);
}

static void
test_try_push_many_partial_when_full(void)
{
    SPMCQueue* queue = create_queue(4);
    void* first[] = {(void*)1, (void*)2, (void*)3};
    void* second[] = {(void*)4, (void*)5, (void*)6};

    assert(queue != NULL);
    assert(try_push_many(queue, first, 3) == 3);
    assert(try_push_many(queue, second, 3) == 1);
    expect_pop_many(queue, 1, 4);
    assert(try_pop_many(queue, second, 1) == 0);
    destroy_queue(queue);
}

int
main(void)
{
    test_try_push_many_basic();
    test_try_push_many_partial_refresh_and_wrap();
    test_try_push_many_partial_when_full();
    return 0;
}
