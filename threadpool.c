#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <pthread.h>

#include "threadpool.h"

static void *__cycle_thread(void * pool);

jobs_queue_t *create_jobs_queue(void)
{
  jobs_queue_t *queue;

  queue = malloc(sizeof *queue);
  if (!queue)
    return NULL;

  jobs_queue_init(queue);

  return queue;
}

void jobs_queue_init(jobs_queue_t *queue)
{
  queue->size = 0;
  list_init_head(&queue->list);
}

inline void jobs_queue_put(jobs_queue_t *queue,
                           jobs_queue_node_t *job)
{
  list_add2tail(&queue->list, &job->node);
  queue->size ++;
}

inline void jobs_queue_get(jobs_queue_t *queue,
                           dispatcher_func_p *f1, void **args1,
                           dispatcher_func_p *f2, void **args2)
{
  list_node_t *node;
  jobs_queue_node_t *entry;

  /* This should not happend in any way
   * Rethink code to remove this shit.
   */
  if (!JOBS_AVAIL(queue)) {
    *f1 = NULL;
    *f2 = NULL;
    *args1 = NULL;
    *args2 = NULL;
    *((int *)0x666) = 111;
    return;
  }

  node = list_node_first(&queue->list);
  entry = container_of(node, jobs_queue_node_t, node);

  *f1    = entry->do_func;
  *args1 = entry->func_args;
  *f2    = entry->cleanup_func;
  *args2 = entry->cleanup_args;

  list_del(&entry->node);
  free(entry);

  queue->size --;
}

thread_pool_t *create_thread_pool(int threads_num)
{
  int i;
  thread_pool_t *pool;
  pthread_mutexattr_t mattr;

  if (threads_num <= 0)
    return NULL;

  if (!(pool = malloc(sizeof *pool)))
    return NULL;

  pool->state = 1;
  pool->threads_num = threads_num;

  pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_ERRORCHECK_NP);
  pthread_mutex_init(&pool->mutex, &mattr);
  pthread_cond_init(&pool->job_posted, NULL);
  pthread_cond_init(&pool->job_taken, NULL);

  if (!(pool->threads = malloc(sizeof(pthread_t) * threads_num))) {
    free(pool);
    return NULL;
  }

  if (!(pool->jobs_queue = create_jobs_queue())) {
    free(pool->threads);
    free(pool);
    return NULL;
  }

  for (i = 0; i < pool->threads_num; i++) {
    if (pthread_create(pool->threads + i, NULL,
                       __cycle_thread, (void *)pool)) {
      exit(EXIT_FAILURE);
    }

    pool->live = i + 1;
    pthread_detach(pool->threads[i]);
  }

  return pool;
}

static inline void __cleanup_handler(void *arg)
{
  pthread_mutex_unlock((pthread_mutex_t *)arg);
}

void kill_thread_pool(thread_pool_t *pool)
{
  int type;

  pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &type);
  pthread_cleanup_push(__cleanup_handler, (void *)&(pool->mutex));

  if (pthread_mutex_lock(&(pool->mutex))) {
    exit(EXIT_FAILURE);
  }

  pool->state = 0;

  while (pool->live > 0) {
    pthread_cond_signal(&(pool->job_posted));
    pthread_cond_wait(&(pool->job_taken), &(pool->mutex));
  }

  free(pool->threads);

  pthread_cleanup_pop(0);
  if (pthread_mutex_unlock(&(pool->mutex))) {
    exit(EXIT_FAILURE);
  }
  if (pthread_mutex_destroy(&(pool->mutex))) {
    exit(EXIT_FAILURE);
  }

  if (pthread_cond_destroy(&(pool->job_posted))) {
    exit(EXIT_FAILURE);
  }

  if (0 != pthread_cond_destroy(&(pool->job_taken))) {
    exit(EXIT_FAILURE);
  }

  /*TODO: clear queue here */
  free(pool->jobs_queue);

  free(pool);
}

void shotdead_thread_pool(thread_pool_t *pool)
{
  int type, i;

  pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, &type);

  pthread_cleanup_push(__cleanup_handler, (void *)&(pool->mutex));
  pthread_mutex_lock(&(pool->mutex));

  for(i=0; i < pool->threads_num; i++) {
    if(0 != pthread_cancel(pool->threads[i])){
      exit(EXIT_FAILURE);
    }
  }

  free(pool->threads);

  pthread_cleanup_pop(0);

  if (pthread_mutex_destroy(&(pool->mutex))) {
    exit(EXIT_FAILURE);
  }

  if (pthread_cond_destroy(&(pool->job_posted))) {
    exit(EXIT_FAILURE);
  }

  if (pthread_cond_destroy(&(pool->job_taken))) {
    exit(EXIT_FAILURE);
  }

  list_node_t *iter, *save;
  jobs_queue_node_t *entry;

  list_for_each_safe(&pool->jobs_queue->list, iter, save) {
    entry = container_of(iter, jobs_queue_node_t, node);
    free(entry->func_args);
    list_del(&entry->node);
    free(entry);
    pool->jobs_queue->size --;
  }

  free(pool->jobs_queue);
  free(pool);
}

void thread_pool_addjob_cleanup(thread_pool_t *_pool,
                                dispatcher_func_p do_func, void *do_args,
                                dispatcher_func_p cleanup_func,
                                void *cleanup_args)
{
  jobs_queue_node_t *job;
  thread_pool_t *pool = _pool;

  pthread_cleanup_push(__cleanup_handler, (void *)&(pool->mutex));
  if (pthread_mutex_lock(&(pool->mutex))) {
    exit(EXIT_FAILURE);
  }

  job = malloc(sizeof *job);
  if (!job) {
    pthread_mutex_unlock(&pool->mutex);
    return;
  }

  job->do_func      = do_func;
  job->func_args    = do_args;
  job->cleanup_func = cleanup_func;
  job->cleanup_args = cleanup_args;
  job->id           = pool->jobs_queue->size + 1;

  jobs_queue_put(pool->jobs_queue, job);

  pthread_cond_signal(&(pool->job_posted));
  if (pthread_mutex_unlock(&(pool->mutex))) {
    exit(EXIT_FAILURE);
  }

  pthread_cleanup_pop(1);
}

static void *__cycle_thread(void * _pool)
{
  thread_pool_t *pool;
  dispatcher_func_p do_func, cleanup_func;
  void *do_args, *cleanup_args;

  pool = (thread_pool_t *)_pool;

  pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
  pthread_cleanup_push(__cleanup_handler, (void *)&(pool->mutex));

  fflush(stderr);
  if (pthread_mutex_lock(&(pool->mutex))) {
    exit(EXIT_FAILURE);
  }

  while (1) {
    while(!JOBS_AVAIL(pool->jobs_queue)) {
      pthread_cond_wait(&(pool->job_posted), &(pool->mutex));
    }

    if (pool->state == 0)
      break;

    jobs_queue_get(pool->jobs_queue, &do_func, &do_args,
                   &cleanup_func, &cleanup_args);

    pthread_cond_signal(&(pool->job_taken));
    if (pthread_mutex_unlock(&(pool->mutex))) {
      exit(EXIT_FAILURE);
    }

    if (cleanup_func) {
      pthread_cleanup_push(cleanup_func, cleanup_args);
      do_func(do_args);
      pthread_cleanup_pop(1);
    } else {
      do_func(do_args);
    }
// BUG: !!!
//    free(do_args);

    if (pthread_mutex_lock(&(pool->mutex))) {
      exit(EXIT_FAILURE);
    }
  }

  pool->live --;

  pthread_cond_signal(&(pool->job_taken));

  if (pthread_mutex_unlock(&(pool->mutex))) {
      exit(EXIT_FAILURE);
  }

  pthread_cleanup_pop(1);

  return NULL;
}
