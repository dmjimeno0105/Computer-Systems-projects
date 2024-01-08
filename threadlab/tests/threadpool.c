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

/**
 * threadpool.h
 *
 * A work-stealing, fork-join thread pool.
 */

/*
 * Opaque forward declarations. The actual definitions of these
 * types will be local to your threadpool.c implementation.
 */

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
    pthread_barrier_t barrier;
    bool is_shutdown;
};

struct worker
{
    struct list local_tasks_list;
    pthread_mutex_t local_lock;
    pthread_t th;
    struct list_elem elem;
    char pad[32];
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
    char pad[16];
    // lock for future maybe not for worker / and a lock for global
};

// custom declerations
static void *thread_function(void *args);
static bool workers_lists_empty(struct thread_pool *pool);
/* thread-local variables */

static thread_local struct worker *curr_worker = NULL;

static void *thread_function(void *args)
{
    struct thread_pool *pool = (struct thread_pool *)args;
    pthread_mutex_lock(&pool->global_lock);
    curr_worker = malloc(sizeof(struct worker));
    list_init(&curr_worker->local_tasks_list);
    pthread_mutex_init(&curr_worker->local_lock, NULL);
    curr_worker->th = pthread_self();
    list_push_front(&pool->workers, &curr_worker->elem);
    pthread_mutex_unlock(&pool->global_lock);
    pthread_barrier_wait(&pool->barrier);

    pthread_mutex_lock(&pool->global_lock);
    while (!pool->is_shutdown)
    {
        struct future *future = NULL;

        while (!pool->is_shutdown && list_empty(&pool->global_tasks_list) && workers_lists_empty(pool))
        {
            pthread_cond_wait(&pool->condition, &pool->global_lock);
        }

        // search global list for a task to work on
        if (!list_empty(&pool->global_tasks_list) && !pool->is_shutdown)
        {
            future = list_entry(list_pop_back(&pool->global_tasks_list), struct future, elem);
            future->state = RUNNING;
            pthread_mutex_unlock(&pool->global_lock);
            future->result = future->task(future->pool, future->args);
            pthread_mutex_lock(&pool->global_lock);
            future->state = FINISHED;
            pthread_cond_broadcast(&pool->condition);
        }
        // search other workers' lists
        else
        {
            pthread_mutex_unlock(&pool->global_lock);
            for (struct list_elem *e = list_begin(&pool->workers); e != list_end(&pool->workers); e = list_next(e))
            {
                struct worker *worker = list_entry(e, struct worker, elem);
                pthread_mutex_lock(&worker->local_lock);
                if (!list_empty(&worker->local_tasks_list))
                {
                    future = list_entry(list_pop_back(&worker->local_tasks_list), struct future, elem);
                    future->state = RUNNING;
                    pthread_mutex_unlock(&worker->local_lock);
                    future->result = future->task(future->pool, future->args);
                    pthread_mutex_lock(&worker->local_lock);
                    future->state = FINISHED;
                    pthread_cond_broadcast(&pool->condition);

                    pthread_mutex_unlock(&worker->local_lock);
                    break;
                }
                pthread_mutex_unlock(&worker->local_lock);
            }
            pthread_mutex_lock(&pool->global_lock);
        }

        // pthread_mutex_lock(&pool->global_lock);
        // if (!pool->is_shutdown) {
        //     pthread_mutex_unlock(&pool->global_lock);
        //     break;
        // }
        // pthread_mutex_unlock(&pool->global_lock);
    }
    pthread_mutex_unlock(&pool->global_lock);

    return NULL;
}

/**
 * Returns true if all workers tasks lists are empty
 */
static bool workers_lists_empty(struct thread_pool *pool)
{
    // TODO was in the process of implementing a circular workers queue so that every worker thread when iterating through, would not lock each other out

    //bool first_half_circle_queue_empty = true;
    //bool second_half_circle_queue_empty = true;

    for (struct list_elem *e = &curr_worker->elem; e != list_end(&pool->workers); e = list_next(e))
    {
        if(list_tail((&pool->workers)) == list_next(e)){
            if()
        }
        struct worker *worker = list_entry(e, struct worker, elem);
        pthread_mutex_lock(&worker->local_lock);
        if (!list_empty(&worker->local_tasks_list))
        {
            pthread_mutex_unlock(&worker->local_lock);
            //first_half_circle_queue_empty = false;
        }
        pthread_mutex_unlock(&worker->local_lock);
    }
    // if (first_half_circle_queue_empty) {
    //     for (struct list_elem *e = list_begin(&pool->workers); e != &curr_worker->elem; e = list_next(e))
    //     {
    //         struct worker *worker = list_entry(e, struct worker, elem);
    //         pthread_mutex_lock(&worker->local_lock);
    //         if (!list_empty(&worker->local_tasks_list))
    //         {
    //             pthread_mutex_unlock(&worker->local_lock);
    //             second_half_circle_queue_empty = false;
    //         }
    //         pthread_mutex_unlock(&worker->local_lock);
    //     }
    // }

    return first_half_circle_queue_empty && second_half_circle_queue_empty;
}

/* Create a new thread pool with no more than n threads. */
struct thread_pool *thread_pool_new(int nthreads)
{
    struct thread_pool *pool = malloc(sizeof(struct thread_pool));
    pool->is_shutdown = false;
    list_init(&pool->global_tasks_list);
    list_init(&pool->workers);
    pthread_mutex_init(&pool->global_lock, NULL);
    pthread_cond_init(&pool->condition, NULL);
    pthread_barrier_init(&pool->barrier, NULL, nthreads);
    pthread_t workers[nthreads];
    
    for (int i = 0; i < nthreads; i++)
    {
        pthread_create(workers + i, NULL, &thread_function, pool);
    }

    return pool;
}

/*
 * Shutdown this thread pool in an orderly fashion.
 * Tasks that have been submitted but not executed may or
 * may not be executed.
 *
 * Deallocate the thread pool object before returning.
 */
void thread_pool_shutdown_and_destroy(struct thread_pool *pool)
{
    pthread_mutex_lock(&pool->global_lock);
    pool->is_shutdown = true;
    pthread_cond_broadcast(&pool->condition);

    while (!list_empty(&pool->workers))
    {
        struct worker *worker = list_entry(list_back(&pool->workers), struct worker, elem);
        pthread_mutex_unlock(&pool->global_lock);
        pthread_join(worker->th, NULL);
        pthread_mutex_lock(&pool->global_lock);
        list_pop_back(&pool->workers);
        pthread_mutex_destroy(&worker->local_lock);
        free(worker);
    }

    pthread_mutex_unlock(&pool->global_lock);
    pthread_mutex_destroy(&pool->global_lock);
    pthread_cond_destroy(&pool->condition);
    pthread_barrier_destroy(&pool->barrier);
    free(pool);
}

/*
 * Submit a fork join task to the thread pool and return a
 * future.  The returned future can be used in future_get()
 * to obtain the result.
 * 'pool' - the pool to which to submit
 * 'task' - the task to be submitted.
 * 'data' - data to be passed to the task's function
 *
 * Returns a future representing this computation.
 */
struct future *thread_pool_submit(struct thread_pool *pool, fork_join_task_t task, void *data)
{
    // main thread
    if (curr_worker == NULL)
    {
        struct future *future = malloc(sizeof(struct future));
        pthread_mutex_lock(&pool->global_lock);
        future->task = task;
        future->args = data;
        future->state = WAITING;
        future->pool = pool;
        future->worker = curr_worker;
        pthread_cond_init(&future->condition, NULL);
        
        list_push_front(&pool->global_tasks_list, &future->elem);

        pthread_mutex_unlock(&pool->global_lock);
        pthread_cond_signal(&pool->condition);

        return future;
    }
    // worker thread
    else
    {
        struct future *future = malloc(sizeof(struct future));
        future->worker = curr_worker;
        pthread_mutex_lock(&future->worker->local_lock);
        future->task = task;
        future->args = data;
        future->state = WAITING;
        future->pool = pool;
        pthread_cond_init(&future->condition, NULL);

        list_push_front(&curr_worker->local_tasks_list, &future->elem);

        pthread_mutex_unlock(&future->worker->local_lock);
        pthread_cond_signal(&pool->condition);

        return future;
    }
}

/* Make sure that the thread pool has completed the execution
 * of the fork join task this future represents.
 *
 * Returns the value returned by this task.
 */
void *future_get(struct future *future)
{
    if (future->worker == NULL) {
        pthread_mutex_lock(&future->pool->global_lock);
        while (future->state != FINISHED)
        {
            pthread_cond_wait(&future->pool->condition, &future->pool->global_lock);
        }
        pthread_mutex_unlock(&future->pool->global_lock);
        return future->result;
    }
    else {
        pthread_mutex_lock(&future->worker->local_lock);
        if (future->state == WAITING)
        {
            list_remove(&future->elem);
            future->state = RUNNING;
            pthread_mutex_unlock(&future->worker->local_lock);
            future->result = future->task(future->pool, future->args);
            pthread_mutex_lock(&future->worker->local_lock);
            future->state = FINISHED;
            pthread_cond_broadcast(&future->pool->condition);
        }
        else if(future->state == RUNNING) {
            // keep popping from the queue that is working on the current future
            if (!list_empty(&future->worker->local_tasks_list)) {
                future = list_entry(list_pop_front(&future->worker->local_tasks_list), struct future, elem);
                future->state = RUNNING;
                pthread_mutex_unlock(&future->worker->local_lock);
                future->result = future->task(future->pool, future->args);
                pthread_mutex_lock(&future->worker->local_lock);
                future->state = FINISHED;
                pthread_cond_broadcast(&future->pool->condition);
            }
        }
        else
        {
            while (future->state != FINISHED)
            {
                pthread_cond_wait(&future->pool->condition, &future->worker->local_lock);
            }
        }
        pthread_mutex_unlock(&future->worker->local_lock);
        return future->result;
    }
}

/* Deallocate this future.  Must be called after future_get() */
void future_free(struct future *future)
{
    free(future);
}
