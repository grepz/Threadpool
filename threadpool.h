#ifndef THREADPOOL_H
#define THREADPOOL_H

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

#include <pthread.h>

#include "list.h"

typedef void (*dispatcher_func_p)(void *);

typedef struct __thread_pool thread_pool_t;

typedef struct __jobs_queue_node jobs_queue_node_t;
typedef struct __jobs_queue jobs_queue_t;

struct __thread_pool
{
  pthread_t *threads;
  int threads_num;
  int live;

  pthread_mutex_t mutex;
  pthread_cond_t  job_posted;
  pthread_cond_t  job_taken;

  int state;

  jobs_queue_t *jobs_queue;
};

struct __jobs_queue
{
  uint32_t size;
  list_head_t list;
};

struct __jobs_queue_node
{
  dispatcher_func_p do_func;
  void *func_args;
  dispatcher_func_p cleanup_func;
  void *cleanup_args;

  int id;
  list_node_t node;
};

#define JOBS_AVAIL(queue)                       \
  ((queue)->size != 0) ? 1 : 0

jobs_queue_t *create_jobs_queue(void);
void jobs_queue_init(jobs_queue_t *queue);
void jobs_queue_put(jobs_queue_t *queue, jobs_queue_node_t *job);
void jobs_queue_get(jobs_queue_t *queue, dispatcher_func_p *f1, void **args1,
                    dispatcher_func_p *f2, void **args2);

thread_pool_t *create_thread_pool(int threads_num);
void kill_thread_pool(thread_pool_t *pool);
void shotdead_thread_pool(thread_pool_t *pool);

//void thread_pool_addjob(thread_pool_t *cur_pool,
//                        dispatcher_func_p do_func,void *args);
#define thread_pool_addjob(pool,do_func,do_args)                           \
  thread_pool_addjob_cleanup(pool, do_func, do_args, NULL, NULL)
void thread_pool_addjob_cleanup(thread_pool_t *cur_pool,
                                dispatcher_func_p do_func, void *do_args,
                                dispatcher_func_p cleanup_func,
                                void *cleanup_args);


#endif
