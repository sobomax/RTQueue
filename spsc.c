#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <inttypes.h>

#include "SPSCQueue.h"

#define NUM_SECONDS 10
#define QUEUE_SIZE 4096
#define WRKR_BATCH_SIZE 8

typedef struct {
    SPSCQueue* queue;
    uint64_t count;
    uint64_t chksum;
} WorkerArgs;

#define unlikely(expr) __builtin_expect(!!(expr), 0)
#define likely(expr) __builtin_expect(!!(expr), 1)

void* worker_thread(void* arg) {
    WorkerArgs* args = (WorkerArgs*) arg;
    SPSCQueue* queue = args->queue;
    void* values[WRKR_BATCH_SIZE] = {};
    uint64_t last_value = 0;
    struct timespec delay = {};
    int sleepcycles = 0;

    while (1) {
        size_t n = try_pop_many(queue, values, WRKR_BATCH_SIZE);
        if (likely(n > 0)) {
            for (size_t i = 0; i < n; i++) {
                if (unlikely(values[i] == (void*)-1)) {
                    goto out;
                }
                uint64_t current_value = (uint64_t)values[i];
                if (unlikely(current_value <= last_value)) {
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

        for (volatile size_t i = 0; unlikely(i < (sleepcycles / QUEUE_SIZE)); i++) {
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
    double stime = timespec2dtime(&st) + NUM_SECONDS;
    double etime = 0;
    uintptr_t i = 0;
    uintptr_t disc = 0;
    uint64_t chksum = 0;
    for (;;) {
        while (unlikely(!try_push(queue, (void*) i + 1))) {
            struct timespec delay = {.tv_nsec = 1};
            nanosleep(&delay, NULL);
            void *junk;
            if (try_pop(queue, &junk)) {
                chksum -= (uintptr_t)junk;
                disc++;
            }
        }
        chksum += i + 1;
        if (unlikely((((1 << 16) - 1) & i) == 0)) {
            clock_gettime(CLOCK_MONOTONIC, &et);
            etime = timespec2dtime(&et);
            if (etime >= stime)
                break;
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
    double ttime = etime - stime + NUM_SECONDS;
    printf("Sent %" PRIu64 " + %" PRIu64 ", received %" PRIu64 " messages in %.5f seconds\n", i - disc, disc, args.count, ttime);
    printf("PPS is %.3f MPPS, packet loss rate %.4f%%\n", 1e-6 * (double)(i - disc) / ttime, 100.0 * (double)disc / (double)i);

    destroy_queue(queue);
    return 0;
}
