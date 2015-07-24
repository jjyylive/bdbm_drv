/*
The MIT License (MIT)

Copyright (c) 2014-2015 CSAIL, MIT

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#ifndef _BLUEDBM_THREAD_H
#define _BLUEDBM_THREAD_H

/*
 * Thread Implementation for Kernel Mode 
 */
#if defined(KERNEL_MODE)

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/sched.h>

#include "platform.h"

typedef struct {
	/* thread management */
	bdbm_mutex thread_done;
	wait_queue_head_t wq;
	wait_queue_t* wait;
	struct task_struct* thread;
	
	/* user management */
	void* user_data;
	int (*user_threadfn)(void *data);
} bdbm_thread_t;

#elif defined(USER_MODE)

#include <stdint.h>

#define SIGKILL	0xCCCC

typedef struct {

} bdbm_thread_t;

#endif /* USER_MODE */

bdbm_thread_t* bdbm_thread_create (int (*threadfn)(void *data), void* data, char* name);
int bdbm_thread_schedule (bdbm_thread_t* k);
void bdbm_thread_wakeup (bdbm_thread_t* k);
void bdbm_thread_stop (bdbm_thread_t* k);
void bdbm_thread_msleep (uint32_t ms);
void bdbm_thread_yield (void);

#endif /* _BLUEDBM_THREAD_H */

