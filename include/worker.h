#ifndef __WORKER_H__
#define __WORKER_H__

#include <taskqueue.h>

extern taskqueue_t *workqueue;
void worker_init();

#endif
