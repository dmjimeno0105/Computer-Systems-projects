#define _GNU_SOURCE
#include <pthread.h>
#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <threads.h>

#include "threadpool.h"
#include "list.h"

void main()
{
    enum status
    {
        WAITING,
        RUNNING,
        FINISHED
    };

    struct thread_pool
    {
        struct list global_tasks_list;
        struct list workers; // will each contain their own (deque) task list
        pthread_mutex_t global_lock;
        pthread_cond_t condition;
        bool is_shutdown;
    };

    struct worker
    {
        struct list local_tasks_list;
        pthread_mutex_t local_lock;
        pthread_t th;
        struct list_elem elem;
    };

    struct future
    {
        fork_join_task_t task; // typedef of a function pointer type that you will execute
        void *args;            // the data from thread_pool_submit
        struct worker *worker;
        void *result;             // will store task result once it completes execution
        enum status state;        // only change when fork/join task is finished or changed
        struct thread_pool *pool; // reference back to pool containing this future
        pthread_cond_t condition;
        struct list_elem elem;

        // lock for future maybe not for worker / and a lock for global
    };

    printf("thread_pool: %d\n", sizeof(struct thread_pool));
    printf("worker: %d\n", sizeof(struct worker));
    printf("future: %d\n", sizeof(struct future));
    printf("thread_pool: %d\n", sizeof(struct thread_pool) % 64);
    printf("worker: %d\n", sizeof(struct worker) % 64);
    printf("future: %d\n", sizeof(struct future) % 64);
}