#pragma once

struct SPSCQueue;

typedef struct SPSCQueue SPSCQueue;

SPSCQueue* create_queue(size_t capacity);
void destroy_queue(SPSCQueue* queue);

bool try_push(SPSCQueue* queue, void* value);
bool try_pop(SPSCQueue* queue, void** value);
