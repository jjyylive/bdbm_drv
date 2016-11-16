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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#include "libmemio.h"

static void __dm_intr_handler (bdbm_drv_info_t* bdi, bdbm_llm_req_t* r);

bdbm_llm_inf_t _bdbm_llm_inf = {
	.ptr_private = NULL,
	.create = NULL,
	.destroy = NULL,
	.make_req = NULL,
	.make_reqs = NULL,
	.flush = NULL,
	/* 'dm' calls 'end_req' automatically
	 * when it gets acks from devices */
	.end_req = __dm_intr_handler, 
};

static bdbm_llm_req_t* __memio_alloc_llm_req (memio_t* mio);
static void __memio_free_llm_req (memio_t* mio, bdbm_llm_req_t* r);

static void __dm_intr_handler (
	bdbm_drv_info_t* bdi, 
	bdbm_llm_req_t* r)
{
	/* it is called by an interrupt handler */
	__memio_free_llm_req ((memio_t*)bdi->private_data, r);
}

static int __memio_init_llm_reqs (memio_t* mio)
{
	int ret = 0;
	if ((mio->rr = (bdbm_llm_req_t*)bdbm_zmalloc (
			sizeof (bdbm_llm_req_t) * mio->nr_punits)) == NULL) {
		bdbm_error ("bdbm_zmalloc () failed");
		ret = -1;
	} else {
		int i = 0;
		for (i = 0; i < mio->nr_punits; i++) {
			mio->rr[i].done = (bdbm_sema_t*)bdbm_malloc (sizeof (bdbm_sema_t));
			bdbm_sema_init (mio->rr[i].done); /* start with unlock */
		}
	}
	return ret;
}

memio_t* memio_open ()
{
	bdbm_drv_info_t* bdi = NULL;
	bdbm_dm_inf_t* dm = NULL;
	memio_t* mio = NULL;
	int ret;

	/* allocate a memio data structure */
	if ((mio = (memio_t*)bdbm_zmalloc (sizeof (memio_t))) == NULL) {
		bdbm_error ("bdbm_zmalloc() failed");
		return NULL;
	}
	bdi = &mio->bdi;

	/* initialize a device manager */
	if (bdbm_dm_init (bdi) != 0) {
		bdbm_error ("bdbm_dm_init() failed");
		goto fail;
	}

	/* get the device manager interface and assign it to bdi */
	if ((dm = bdbm_dm_get_inf (bdi)) == NULL) {
		bdbm_error ("bdbm_dm_get_inf() failed");
		goto fail;
	}
	bdi->ptr_dm_inf = dm;

	/* probe the device to see if it is working now */
	if ((ret = dm->probe (bdi, &bdi->parm_dev)) != 0) {
		bdbm_error ("dm->probe was NULL or probe() failed (%p, %d)", 
			dm->probe, ret);
		goto fail;
	}
	mio->nr_punits = 64; /* FIXME: it must be set according to device parameters */
	mio->io_size = 8192;
	mio->trim_lbas = (1 << 14);
	mio->trim_size = mio->trim_lbas * mio->io_size;

	/* setup some internal values according to 
	 * the device's organization */
	if ((ret = __memio_init_llm_reqs (mio)) != 0) {
		bdbm_error ("__memio_init_llm_reqs () failed (%d)", 
			ret);
		goto fail;
	}

	/* setup function points; this is just to handle responses from the device */
	bdi->ptr_llm_inf = &_bdbm_llm_inf;

	/* assign rf to bdi's private_data */
	bdi->private_data = (void*)mio;

	/* ok! open the device so that I/Os will be sent to it */
	if ((ret = dm->open (bdi)) != 0) {
		bdbm_error ("dm->open was NULL or open failed (%p. %d)", 
			dm->open, ret);
		goto fail;
	}

	return mio;

fail:
	if (mio)
		bdbm_free (mio);

	return NULL;
}

static bdbm_llm_req_t* __memio_alloc_llm_req (memio_t* mio)
{
	int i = 0;
	bdbm_llm_req_t* r = NULL;

	/* get available llm_req */
	do {
		for (i = 0; i < mio->nr_punits; i++) { /* <= FIXME: use the linked-list instead of loop! */
			if (!bdbm_sema_try_lock (mio->rr[i].done))
				continue;
			r = (bdbm_llm_req_t*)&mio->rr[i];
			r->tag = i;
			break;
		}
	} while (!r); /* <= FIXME: use the event instead of loop! */

	return r;
}

static void __memio_free_llm_req (memio_t* mio, bdbm_llm_req_t* r)
{
	/* release semaphore */
	r->tag = -1;
	bdbm_sema_unlock (r->done);
}

static void __memio_check_alignment (uint64_t length, uint64_t alignment)
{
	if ((length % alignment) != 0) {
		bdbm_error ("alignment error occurs (length = %d, alignment = %d)",
			length, alignment);
		exit (-1);
	}
}

static int __memio_do_io (memio_t* mio, int dir, uint64_t lba, uint64_t len, uint8_t* data)
{
	bdbm_llm_req_t* r = NULL;
	bdbm_dm_inf_t* dm = mio->bdi.ptr_dm_inf;
	uint8_t* cur_buf = data;
	uint64_t cur_lba = lba;
	uint64_t sent = 0;
	int ret;
	int cnt=0;
	
	/* see if LBA alignment is correct */
	__memio_check_alignment (len, mio->io_size);

	/* fill up logaddr; note that phyaddr is not used here */
	while (cur_lba < lba + (len/mio->io_size)) {
		if((++cnt)%64 == 0) bdbm_thread_nanosleep(100);
		/* get an empty llm_req */
		r = __memio_alloc_llm_req (mio);

		bdbm_bug_on (!r);

		/* setup llm_req */
		r->req_type = (dir == 0) ? REQTYPE_READ : REQTYPE_WRITE;
		r->logaddr.lpa[0] = cur_lba;
		r->fmain.kp_ptr[0] = cur_buf;

		/* send I/O requets to the device */
		if ((ret = dm->make_req (&mio->bdi, r)) != 0) {
			bdbm_error ("dm->make_req() failed (ret = %d)", ret);
			bdbm_bug_on (1);
		}

		/* go the next */
		cur_lba += 1;
		cur_buf += mio->io_size;
		sent += mio->io_size;
	}

	/* return the length of bytes transferred */
	return sent;
}

void memio_wait (memio_t* mio)
{
	int i, j=0;
	bdbm_dm_inf_t* dm = mio->bdi.ptr_dm_inf;
	for (i = 0; i < mio->nr_punits; ) {
		if (!bdbm_sema_try_lock (mio->rr[i].done)){
			if ( ++j == 500000 ) {
				bdbm_msg ("timeout at tag:%d, reissue command", mio->rr[i].tag);
				dm->make_req (&mio->bdi, mio->rr + i);
				j=0;
			}
			continue;
		}
		bdbm_sema_unlock (mio->rr[i].done);
		i++;
	}
}

int memio_read (memio_t* mio, uint64_t lba, uint64_t len, uint8_t* data)
{
//	if ( len > 8192*128 ) 
//		bdbm_msg ("memio_read: %zd, %zd", lba, len);
	return __memio_do_io (mio, 0, lba, len, data);
}

int memio_write (memio_t* mio, uint64_t lba, uint64_t len, uint8_t* data)
{
	//bdbm_msg ("memio_write: %zd, %zd", lba, len);
	return __memio_do_io (mio, 1, lba, len, data);
}

int memio_trim (memio_t* mio, uint64_t lba, uint64_t len)
{
	bdbm_llm_req_t* r = NULL;
	bdbm_dm_inf_t* dm = mio->bdi.ptr_dm_inf;
	uint64_t cur_lba = lba;
	uint64_t sent = 0;
	int ret, i;

//	bdbm_msg ("memio_trim: %llu, %llu", lba, len);

	/* see if LBA alignment is correct */
	__memio_check_alignment (lba, mio->trim_lbas);
	__memio_check_alignment (len, mio->trim_size);

	/* fill up logaddr; note that phyaddr is not used here */
	while (cur_lba < lba + (len/mio->io_size)) {
		//bdbm_msg ("segment #: %d", cur_lba / mio->trim_lbas);
		for (i = 0; i < mio->nr_punits; i++) {
			/* get an empty llm_req */
			r = __memio_alloc_llm_req (mio);

			bdbm_bug_on (!r);

			/* setup llm_req */
			//bdbm_msg ("  -- blk #: %d", i);
			r->req_type = REQTYPE_GC_ERASE;
			r->logaddr.lpa[0] = cur_lba + i;
			r->fmain.kp_ptr[0] = NULL;	/* no data; it must be NULL */

			/* send I/O requets to the device */
			if ((ret = dm->make_req (&mio->bdi, r)) != 0) {
				bdbm_error ("dm->make_req() failed (ret = %d)", ret);
				bdbm_bug_on (1);
			}
		}

		/* go the next */
		cur_lba += mio->trim_lbas;
		sent += mio->trim_size;
	}

	/* return the length of bytes transferred */
	return sent;
}

void memio_close (memio_t* mio)
{
	bdbm_drv_info_t* bdi = NULL;
	bdbm_dm_inf_t* dm = NULL;
	int i;

	/* mio is available? */
	if (!mio) return;

	/* get pointers for dm and bdi */
	bdi = &mio->bdi;
	dm = bdi->ptr_dm_inf;

	/* wait for all the on-going jobs to finish */
	bdbm_msg ("Wait for all the on-going jobs to finish...");
	if (mio->rr) {
		for (i = 0; i < mio->nr_punits; i++)
			if (mio->rr[i].done)
				bdbm_sema_lock (mio->rr[i].done);
	}

	/* close the device interface */
	bdi->ptr_dm_inf->close (bdi);

	/* close the device module */
	bdbm_dm_exit (&mio->bdi);

	/* free allocated memory */
	if (mio->rr) {
		for (i = 0; i < mio->nr_punits; i++)
			if (mio->rr[i].done)
				bdbm_free (mio->rr[i].done);
		bdbm_free (mio->rr);
	}
	bdbm_free (mio);
}

