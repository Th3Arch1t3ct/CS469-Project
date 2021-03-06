//
// Created by arch1t3ct on 9/24/20.
//

#ifndef CS469_PROJECT_QUEUE_H
#define CS469_PROJECT_QUEUE_H

#include <malloc.h>

struct queue_root;
struct queue_head {
    struct queue_head *next;
    char *operation;
    struct queue_root* response_queue;
};

struct queue_root *ALLOC_QUEUE_ROOT();
void INIT_QUEUE_HEAD(struct queue_head *head, char* operation, struct queue_root *r_queue);
void queue_put(struct queue_head *new, struct queue_root *root);
struct queue_head *queue_get(struct queue_root *root);
void free_queue_message(struct queue_head *msg);

#endif //CS469_PROJECT_QUEUE_H
