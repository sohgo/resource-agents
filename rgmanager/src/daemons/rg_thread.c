/*
  Copyright Red Hat, Inc. 2004

  This program is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by the
  Free Software Foundation; either version 2, or (at your option) any
  later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; see the file COPYING.  If not, write to the
  Free Software Foundation, Inc.,  675 Mass Ave, Cambridge, 
  MA 02139, USA.
*/
#include <resgroup.h>
#include <rg_locks.h>
#include <gettid.h>
#include <rg_queue.h>
#include <assert.h>
#include <message.h>

/**
 * Resource thread list entry.
 */
typedef struct __resthread {
	list_head();
	pthread_t	rt_thread;		/** Thread identifier */
	int		rt_request;		/** Current pending operation */
	int		rt_status;		/** Used for init */
	char		rt_name[256];		/** RG name */
	request_t	**rt_queue;		/** RG event queue */
	pthread_mutex_t	*rt_queue_mutex;	/** Mutex for event queue */
	pthread_cond_t	*rt_queue_cond;		/** pthread cond */
} resthread_t;


/**
 * Resource thread queue head.
 */
static resthread_t *resthread_list = NULL;
static pthread_mutex_t reslist_mutex = PTHREAD_MUTEX_INITIALIZER;


static resthread_t *find_resthread_byname(const char *resgroupname);
static int spawn_if_needed(const char *resgroupname);
int rt_enqueue_request(const char *resgroupname, int request,
		       msgctx_t *response_ctx, int max, uint64_t target,
		       int arg0, int arg1);


/**
  SIGUSR1 output
 */
void
dump_threads(void)
{
	resthread_t *rt;

	printf("+++ BEGIN Thread dump\n");
	pthread_mutex_lock(&reslist_mutex);
	list_do(&resthread_list, rt) {
		printf("TID %d group %s (@ %p) request %d\n",
		       (int)rt->rt_thread,
		       rt->rt_name, rt, rt->rt_request);
	} while (!list_done(&resthread_list, rt));
	pthread_mutex_unlock(&reslist_mutex);
	printf("--- END Thread dump\n");
}


static int 
wait_initialize(const char *name)
{
	resthread_t *t;

	while (1) {
		pthread_mutex_lock(&reslist_mutex);
		t = find_resthread_byname(name);

		if (!t) {
			pthread_mutex_unlock(&reslist_mutex);
			return -1;
		}

		if (t->rt_status != RG_STATE_UNINITIALIZED)  {
			pthread_mutex_unlock(&reslist_mutex);
			return 0;
		}

		pthread_mutex_unlock(&reslist_mutex);
		usleep(50000);
	}

	assert(0);
}


static void
rg_sighandler_setup(void)
{
	block_all_signals();
	unblock_signal(SIGCHLD);
}


static void
purge_status_checks(request_t **list)
{
	request_t *curr;
	
	if (!list)
		return;

	list_do(list, curr) {
		if (curr->rr_request != RG_STATUS)
			continue;

		list_remove(list, curr);
		rq_free(curr);
		curr = *list;
	} while (!list_done(list, curr));
}


static void
purge_all(request_t **list)
{
	request_t *curr;
	
	if (!list)
		return;

	if (!*list)
		return;

	while((curr = *list)) {

		list_remove(list, curr);
		dprintf("Removed request %d\n", curr->rr_request);
		if (curr->rr_resp_ctx) {
			send_response(RG_EABORT, curr);
		}
		rq_free(curr);
	}
}


static void *
resgroup_thread_main(void *arg)
{
	pthread_mutex_t my_queue_mutex;
	pthread_cond_t my_queue_cond;
	request_t *my_queue = NULL;
	int newowner = 0;
	char myname[256];
	resthread_t *myself;
	request_t *req;
	uint32_t ret = RG_FAIL, error = 0;

	rg_inc_threads();

	strncpy(myname, arg, 256);
	dprintf("Thread %s (tid %d) starting\n",myname,gettid());

	pthread_mutex_init(&my_queue_mutex, NULL);
	pthread_mutex_lock(&my_queue_mutex);
	pthread_cond_init(&my_queue_cond, NULL);

	/*
	 * Wait until we're inserted.  The code herein should never be 
	 * reached.
	 */
	while (1) {
		pthread_mutex_lock(&reslist_mutex);

		myself = find_resthread_byname(myname);
		if (myself)
			break;

		pthread_mutex_unlock(&reslist_mutex);
		usleep(250000);
	}

	myself->rt_queue = &my_queue;
	myself->rt_queue_mutex = &my_queue_mutex;
	myself->rt_queue_cond = &my_queue_cond;
	myself->rt_status = RG_STATE_STARTED; /* Ok, we're ready to go */
	rg_sighandler_setup();

	/* Wait for first event */
	pthread_mutex_unlock(&reslist_mutex);

	/* My mutex is still held */
	pthread_cond_wait(&my_queue_cond, &my_queue_mutex);
	pthread_mutex_unlock(&my_queue_mutex);

	while(1) {
		pthread_mutex_lock(&reslist_mutex);
 		pthread_mutex_lock(&my_queue_mutex);
		if ((req = rq_next_request(&my_queue)) == NULL) {
			/* We're done.  No more requests.
			   We're about to kill our thread, so exit the
			   loop with the lock held. */
			break;
		}
		pthread_mutex_unlock(&my_queue_mutex);
		pthread_mutex_unlock(&reslist_mutex);

		ret = RG_FAIL;
		error = 0;

		dprintf("Processing request %s, resource group %s\n",
			rg_req_str(req->rr_request), myname);

		/* find ourselves. */
		pthread_mutex_lock(&reslist_mutex);
		myself = find_resthread_byname(myname);
		assert(myself);
		myself->rt_request = req->rr_request;
		if (req->rr_request == RG_STOP_EXITING)
			myself->rt_status = RG_STATE_STOPPING;
		pthread_mutex_unlock(&reslist_mutex);

		switch(req->rr_request) {
		case RG_START_REMOTE:
		case RG_START_RECOVER:
			error = handle_start_remote_req(myname,
							req->rr_request);
			break;

		case RG_START:
		case RG_ENABLE:
			error = handle_start_req(myname, req->rr_request,
						 &newowner);
			break;

		case RG_RELOCATE:
			/* Relocate requests are user requests and must be
			   forwarded */
			error = handle_relocate_req(myname, RG_START_REMOTE,
   						    req->rr_target,
   						    &newowner);
			if (error == RG_EFORWARD)
				ret = RG_NONE;
			break;

		case RG_INIT:
			/* Stop without changing shared state of it */
			error = group_op(myname, RG_STOP);

			pthread_mutex_lock(&my_queue_mutex);
			purge_all(&my_queue);
			pthread_mutex_unlock(&my_queue_mutex);

			if (error == 0)
				ret = RG_SUCCESS;
			else
				ret = RG_FAIL;
			break;

		case RG_CONDSTOP:
			/* CONDSTOP doesn't change RG state by itself */
			group_op(myname, RG_CONDSTOP);
			break;

		case RG_CONDSTART:
			/* CONDSTART doesn't change RG state by itself */
			group_op(myname, RG_CONDSTART);
			break;

		case RG_STOP:
		case RG_STOP_USER:
			/* Disable and user stop requests need to be
			   forwarded; they're user requests */
			error = svc_stop(myname, req->rr_request);

			if (error == 0) {
				ret = RG_SUCCESS;

				pthread_mutex_lock(&my_queue_mutex);
				purge_status_checks(&my_queue);
				pthread_mutex_unlock(&my_queue_mutex);
			} else if (error == RG_EFORWARD) {
				ret = RG_NONE;
				break;
			} else {
				/*
				 * Bad news. 
				 */
				ret = RG_FAIL;
			}

			break;

		case RG_STOP_EXITING:
			/* We're out of here. Don't allow starts anymore */
			error = svc_stop(myname, RG_STOP);

			if (error == 0) {
				ret = RG_SUCCESS;

			} else if (error == RG_EFORWARD) {
				ret = RG_NONE;
				break;
			} else {
				/*
				 * Bad news. 
				 */
				ret = RG_FAIL;
			}

			pthread_mutex_lock(&my_queue_mutex);
			purge_all(&my_queue);
			pthread_mutex_unlock(&my_queue_mutex);

			break;


		case RG_DISABLE:
			/* Disable and user stop requests need to be
			   forwarded; they're user requests */
			error = svc_disable(myname);

			if (error == 0) {
				ret = RG_SUCCESS;

				pthread_mutex_lock(&my_queue_mutex);
				purge_status_checks(&my_queue);
				pthread_mutex_unlock(&my_queue_mutex);
			} else if (error == RG_EFORWARD) {
				ret = RG_NONE;
				break;
			} else {
				/*
				 * Bad news. 
				 */
				ret = RG_FAIL;
			}

			break;

		case RG_RESTART:
			error = svc_stop(myname, RG_STOP_USER);

			if (error == 0) {
				pthread_mutex_lock(&my_queue_mutex);
				purge_status_checks(&my_queue);
				pthread_mutex_unlock(&my_queue_mutex);

				error = handle_start_req(myname,
							 req->rr_request,
							 &newowner);
				break;

			} else if (error == RG_EFORWARD) {
				ret = RG_NONE;
				break;
			} else {
				/*
				 * Bad news. 
				 */
				ret = RG_FAIL;
			}

			break;

		case RG_STATUS:
			/* Need to make sure we don't check status of
			   resource groups we don't own */
			error = svc_status(myname);

			/* Recover dead service */
			if (error == 0) {
				ret = RG_SUCCESS;
				break;
			}

			error = svc_stop(myname, RG_STOP_RECOVER);
			if (error == 0) {
				error = handle_recover_req(myname, &newowner);
				if (error == 0)
					ret = RG_SUCCESS;
			}

			break;

		default:
			printf("Unhandled request %d\n", req->rr_request);
			ret = RG_NONE;
			break;
		}

		pthread_mutex_lock(&reslist_mutex);
		myself = find_resthread_byname(myname);
		myself->rt_request = RG_NONE;
		pthread_mutex_unlock(&reslist_mutex);

		if (error == RG_EFORWARD) {
			/* Forward_request frees this and closes the
			   file descriptor, so we can just move on
			   with life. */
			forward_request(req);
			continue;
		}

		if (ret != RG_NONE && rg_initialized() &&
		    (req->rr_resp_ctx)) {
			send_response(error, req);
		}

		
		rq_free(req);
	}

	/* reslist_mutex and my_queue_mutex held */
	myself = find_resthread_byname(myname);

	if (!myself) {
		dprintf("I don't exist...\n");
		raise(SIGSEGV);
	}

	pthread_mutex_destroy(&my_queue_mutex);
	list_remove(&resthread_list, myself);
	free(myself);

	pthread_mutex_unlock(&reslist_mutex);

	dprintf("RGth %s (tid %d): No more requests"
		"; exiting.\n", myname, gettid());

	/* Thread's outta here */
	rg_dec_threads();
	pthread_exit((void *)NULL);
}


/**
 * Start a resgroup thread.
 */
static int
spawn_resgroup_thread(const char *name)
{
        pthread_attr_t attrs;
	resthread_t *newthread = NULL;
	int ret = 0;

        pthread_attr_init(&attrs);
        pthread_attr_setinheritsched(&attrs, PTHREAD_INHERIT_SCHED);
        pthread_attr_setdetachstate(&attrs, PTHREAD_CREATE_DETACHED);

	newthread = malloc(sizeof(*newthread));
	if (!newthread)
		return -1;
	memset(newthread, 0, sizeof(*newthread));

	newthread->rt_status = RG_STATE_UNINITIALIZED;
	strncpy(newthread->rt_name, name, sizeof(newthread->rt_name));

	ret = pthread_create(&newthread->rt_thread, &attrs,
			     resgroup_thread_main, (void *)name);
	pthread_attr_destroy(&attrs);

	if (ret != 0) {
		free(newthread);
		return ret;
	}

	list_insert(&resthread_list, newthread);

	return 0;
}


/**
  Spawn a resource group thread if necessary
 */
int
spawn_if_needed(const char *resgroupname)
{
	int ret;
	resthread_t *resgroup = NULL;

retry:
	pthread_mutex_lock(&reslist_mutex);
	while (resgroup == NULL) {
		resgroup = find_resthread_byname(resgroupname);
		if (resgroup != NULL)
			break;

		ret = spawn_resgroup_thread(resgroupname);
		if (ret == 0)
			continue;
		pthread_mutex_unlock(&reslist_mutex);

		return ret;
	}

	ret = (resgroup->rt_status == RG_STATE_STOPPING);

	pthread_mutex_unlock(&reslist_mutex);
	if (wait_initialize(resgroupname) < 0) {
		goto retry;
	}

	return ret;
}


/**
 * Call with mutex locked.
 */
static resthread_t *
find_resthread_byname(const char *resgroupname)
{
	resthread_t *curr = NULL;

	if (!resthread_list)
		return NULL;

	list_do(&resthread_list, curr) {
		if (!strncmp(resgroupname, curr->rt_name,
		    sizeof(curr->rt_name)))
			return curr;
	} while (!list_done(&resthread_list, curr));

	return NULL;
}


/**
 * queues a request for a resgroup.
 *
 * @param resgroupname		Service name to perform operations on
 * @param request		Request to perform
 * @param response_ctx		Send response to this file descriptor when
 *				this request completes.
 * @param max			Don't insert this request if there already
 * 				are this many requests of this type in the
 *				queue.
 * @param arg			Argument to the decoder.
 * @param arglen		Length of argument.
 * @return			-1 on failure, 0 on success, or 1 if
 *				the request was dropped.
 * @see rq_queue_request
 */
int
rt_enqueue_request(const char *resgroupname, int request, 
		   msgctx_t *response_ctx,
   		   int max, uint64_t target, int arg0, int arg1)
{
	request_t *curr;
	int count = 0, ret;
	resthread_t *resgroup;

	if (spawn_if_needed(resgroupname) != 0) {
		/* Usually, we get here if the thread is killing
		   stuff.  This prevents us from queueing START requests
		   while we're exiting */
		return -1;
	}

	pthread_mutex_lock(&reslist_mutex);
	resgroup = find_resthread_byname(resgroupname);
	if (resgroup == NULL) {
		/* DOOOOM */
		pthread_mutex_unlock(&reslist_mutex);
		return -1;
	}

	/* Main mutex held */
	if (resgroup->rt_request == request)
		count++;

	pthread_mutex_lock(resgroup->rt_queue_mutex);

	if (request == RG_INIT) {
		/* If we're initializing it, zap the queue if there
		   is one */
		purge_all(resgroup->rt_queue);
	} else {
		if (max) {
			list_do(resgroup->rt_queue, curr) {
				if (curr->rr_request == request)
					count++;
			} while (!list_done(resgroup->rt_queue, curr));
	
			if (count >= max) {
				pthread_cond_broadcast(resgroup->rt_queue_cond);
				pthread_mutex_unlock(resgroup->rt_queue_mutex);
				pthread_mutex_unlock(&reslist_mutex);
				/*
				 * Maximum reached.
				 */
				return 1;
			}
		}
	}

	if (resgroup->rt_request == RG_RELOCATE) {
		switch(request) {
		case RG_RELOCATE:
		case RG_START_REMOTE:
		case RG_START_RECOVER:
		case RG_START:
		case RG_ENABLE:
			send_ret(response_ctx, resgroup->rt_name, RG_EDEADLCK,
				 request);
			break;
		}
		fprintf(stderr, "Failed to queue request: Would block\n");
		/* EWOULDBLOCK */
		pthread_mutex_unlock(resgroup->rt_queue_mutex);
		pthread_mutex_unlock(&reslist_mutex);
		return -1;
	}

	ret = rq_queue_request(resgroup->rt_queue, resgroup->rt_name,
			       request, 0, 0, response_ctx, 0, target,
			       arg0, arg1);
	pthread_cond_broadcast(resgroup->rt_queue_cond);
	pthread_mutex_unlock(resgroup->rt_queue_mutex);
	pthread_mutex_unlock(&reslist_mutex);

	if (ret < 0)
		return ret;

	dprintf("Queued request for %d for %s\n", request, resgroupname);
	
	return 0;	
}