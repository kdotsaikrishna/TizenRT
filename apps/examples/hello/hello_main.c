/****************************************************************************
 *
 * Copyright 2016 Samsung Electronics All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the License.
 *
 ****************************************************************************/
/****************************************************************************
 * examples/hello/hello_main.c
 *
 *   Copyright (C) 2008, 2011-2012 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <tinyara/config.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <aifw/aifw.h>

/****************************************************************************
 * hello_main
 ****************************************************************************/
#include <semaphore.h>
#include <signal.h>
//#include <memory>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <errno.h>
#include "aifw/aifw_log.h"

typedef void *(*aifw_timer_callback)(void *);
typedef enum {
	AIFW_TIMER_FAIL = -1,		/**<  fail				*/
	AIFW_TIMER_SUCCESS = 0,		/**<  success				*/
	AIFW_TIMER_INVALID_ARGS = 1,		/**<  invalid parameter (argument)	*/
} aifw_timer_result;

struct h_timer{
    timer_t id;
    aifw_timer_callback function;
    void *function_args;
    unsigned int interval;
    bool enable;
    sem_t semaphore;
    sem_t exitSemaphore;
    pthread_t timerThread;
};
struct h_timer *gTimer;
uint16_t gInterval = 2000;
#define AIFW_TIMER_SIGNAL 17

static void aifw_timer_cb(int signo, siginfo_t *info, void *ucontext);

static void *aifw_timerthread_cb(void *parameter)
{
	sigset_t sigset;
	struct sigaction act;
	struct sigevent notify;
	struct itimerspec its;
	timer_t timerId;
	int status;

	if (!parameter) {
		AIFW_LOGE("aifw_timerthread_cb: invalid argument");
		return NULL;
	}

	uint32_t interval_secs = ((struct h_timer *)parameter)->interval / 1000;
	uint64_t interval_nsecs = (((struct h_timer *)parameter)->interval % 1000) * 1000000;

	AIFW_LOGV("aifw_timerthread_cb: Initializing semaphore to 0");
	sem_init(&(((struct h_timer *)parameter)->semaphore), 0, 0);

	AIFW_LOGV("Initializing exit semaphore to 0");
	sem_init(&(((struct h_timer *)parameter)->exitSemaphore), 0, 0);

	/* Start waiter thread  */
	AIFW_LOGV("aifw_timerthread_cb: Unmasking signal %d", AIFW_TIMER_SIGNAL);
	(void)sigemptyset(&sigset);
	(void)sigaddset(&sigset, AIFW_TIMER_SIGNAL);
	status = sigprocmask(SIG_UNBLOCK, &sigset, NULL);
	if (status != OK) {
		AIFW_LOGE("aifw_timerthread_cb: ERROR sigprocmask failed, status=%d", status);
		return NULL;
	}

	AIFW_LOGV("aifw_timerthread_cb: Registering signal handler");
	act.sa_sigaction = aifw_timer_cb;
	act.sa_flags = SA_SIGINFO;

	(void)sigfillset(&act.sa_mask);
	(void)sigdelset(&act.sa_mask, AIFW_TIMER_SIGNAL);
	status = sigaction(AIFW_TIMER_SIGNAL, &act, NULL);
	if (status != OK) {
		AIFW_LOGE("aifw_timerthread_cb: ERROR sigaction failed, status=%d", status);
		return NULL;
	}

	/* Create the POSIX timer */
	AIFW_LOGV("aifw_timerthread_cb: Creating timer");
	notify.sigev_notify = SIGEV_SIGNAL;
	notify.sigev_signo = AIFW_TIMER_SIGNAL;
	notify.sigev_value.sival_ptr = parameter;
	status = timer_create(CLOCK_REALTIME, &notify, &(((struct h_timer *)parameter)->id));
	timerId = ((struct h_timer *)parameter)->id;
	if (status != OK) {
		AIFW_LOGE("aifw_timerthread_cb: timer_create failed, errno=%d", errno);
		goto errorout;
	}

	/* Start the POSIX timer */
	AIFW_LOGV("aifw_timerthread_cb: Starting timer");
	AIFW_LOGV("aifw_timerthread_cb: %d %d %lld", ((struct h_timer *)parameter)->interval, interval_secs, interval_nsecs);
	its.it_value.tv_sec = interval_secs;
	its.it_value.tv_nsec = interval_nsecs;
	its.it_interval.tv_sec = interval_secs;
	its.it_interval.tv_nsec = interval_nsecs;

	status = timer_settime(((struct h_timer *)parameter)->id, 0, &its, NULL);
	if (status != OK) {
		AIFW_LOGE("aifw_timerthread_cb: timer_settime failed, errno=%d", errno);
		goto errorout;
	}
	AIFW_LOGV("aifw_timerthread_cb: exiting function");
	((struct h_timer *)parameter)->enable = true;
	/* Take the semaphore */
	while (1) {
		AIFW_LOGV("aifw_timerthread_cb: Waiting on semaphore");
		status = sem_wait(&(((struct h_timer *)parameter)->semaphore));
		if (status != 0) {
			int error = errno;
			if (error == EINTR) {
				AIFW_LOGV("aifw_timerthread_cb: sem_wait() successfully interrupted by signal");
			} else {
				AIFW_LOGE("aifw_timerthread_cb: ERROR sem_wait failed, errno=%d", error);
			}
		} else {
			AIFW_LOGI("aifw_timerthread_cb: ERROR awakened with no error!");
			break;
		}
	}
errorout:
	/* Delete the timer */
	AIFW_LOGV("aifw_timerthread_cb: Deleting timer");
	status = timer_delete(timerId);
	if (status != OK) {
		AIFW_LOGE("aifw_timerthread_cb: timer_delete failed, errno=%d", errno);
	}
	/* Detach the signal handler */
	act.sa_handler = SIG_DFL;
	status = sigaction(AIFW_TIMER_SIGNAL, &act, NULL);
	AIFW_LOGV("aifw_timerthread_cb: sem_destroy");
	sem_destroy(&(((struct h_timer *)parameter)->semaphore));
	sem_post(&(((struct h_timer *)parameter)->exitSemaphore));
	AIFW_LOGV("aifw_timerthread_cb: done");
	return NULL;
}

static void aifw_timer_cb(int signo, siginfo_t *info, void *ucontext)
{
	AIFW_LOGV("aifw_timer_cb: Received signal %d", signo);
	if (signo != AIFW_TIMER_SIGNAL) {
		AIFW_LOGE("aifw_timer_cb: ERROR expected signo: %d", AIFW_TIMER_SIGNAL);
		return;
	}
	if (info->si_signo != AIFW_TIMER_SIGNAL) {
		AIFW_LOGE("aifw_timer_cb: ERROR expected si_signo=%d, got=%d", AIFW_TIMER_SIGNAL, info->si_signo);
		return;
	}
	if (info->si_code != SI_TIMER) {
		AIFW_LOGE("aifw_timer_cb: ERROR si_code=%d, expected SI_TIMER=%d", info->si_code, SI_TIMER);
		return;
	}
	AIFW_LOGV("aifw_timer_cb: si_code=%d (SI_TIMER)", info->si_code);
	struct h_timer *timer = (struct h_timer *)info->si_value.sival_ptr;
	timer->function(timer->function_args);
}

static void h_timerTaskHandler(void *args)
{
	char check_str1[100] = "0.783092\n";
	char check_str2[100] = "0.819210\0";
	char check_str3[100] = "1000.454355";
	float var1 = atof(check_str1);
	float var2 = atof(check_str2);
	float var3 = atof(check_str3);

	printf("\n\nh_timerTaskHandler %d %d %d %f %f %f %f\n", strlen(check_str1), strlen(check_str2), strlen(check_str3), var1, var2, var3, atof("1000.454355"));

	float f = 100.454F;
	float fv = 100.454;
	float f1 = 6545.454873F;
	float f2 = 6545.454873;
	printf("h_timerTaskHandler %.6f %f %f %f\n", f, fv, f1, f2);
}

aifw_timer_result h_aifw_timer_start(struct h_timer *timer)
{
	if (!timer) {
		AIFW_LOGE("Pointer to h_timer structure is NULL");
		return AIFW_TIMER_INVALID_ARGS;
	}
	AIFW_LOGV("Start Timer");
	int result = pthread_create(&(timer->timerThread), NULL, aifw_timerthread_cb, (void *)timer);
	if (result != 0) {
		AIFW_LOGE("ERROR Failed to start aifw_timerthread_cb");
		return AIFW_TIMER_FAIL;
	}
	AIFW_LOGV("Started aifw_timerthread_cb");
	return AIFW_TIMER_SUCCESS;
}

aifw_timer_result h_aifw_timer_create(struct h_timer *timer, void *timer_function, void *function_args, unsigned int interval)
{
	if (!timer) {
		AIFW_LOGE("Pointer to h_timer structure is NULL");
		return AIFW_TIMER_INVALID_ARGS;
	}
	if (!timer_function) {
		AIFW_LOGE("Pointer to timer function is NULL");
		return AIFW_TIMER_INVALID_ARGS;
	}
	if (interval <= 0 || interval > 0x7FFFFFFF) {
		AIFW_LOGE("Value of timer interval is out of bounds");
		return AIFW_TIMER_INVALID_ARGS;
	}
	/* Fill values in timer structure */
	timer->function = (aifw_timer_callback)timer_function;
	timer->function_args = function_args;
	timer->interval = interval;
	timer->enable = false;
	return AIFW_TIMER_SUCCESS;
}

aifw_timer_result h_aifw_timer_change_interval(struct h_timer *timer, unsigned int interval)
{
	if (!timer) {
		AIFW_LOGE("Pointer to struct h_timer structure is NULL");
		return AIFW_TIMER_INVALID_ARGS;
	}
	if (interval <= 0) {
		AIFW_LOGE("Invalid argument interval: %d", interval);
		return AIFW_TIMER_INVALID_ARGS;
	}
	timer->interval = interval;
	if (!timer->enable) {
		AIFW_LOGV("Timer not started/enabled yet");
		return h_aifw_timer_start(timer);
	}
	struct itimerspec its;
	uint32_t interval_secs = interval / 1000;
	uint64_t interval_nsecs = (interval % 1000) * 1000000;
	AIFW_LOGV("setInterval: %d %d %lld", interval, interval_secs, interval_nsecs);
	its.it_value.tv_sec = interval_secs;
	its.it_value.tv_nsec = interval_nsecs;
	its.it_interval.tv_sec = interval_secs;
	its.it_interval.tv_nsec = interval_nsecs;
	int status = timer_settime(timer->id, 0, &its, NULL);
	if (status != OK) {
		AIFW_LOGE("setInterval: timer_settime failed, errno: %d", errno);
		return AIFW_TIMER_FAIL;
	}
	return AIFW_TIMER_SUCCESS;
}

AIFW_RESULT h_prepare(void)
{
	gTimer = (struct h_timer *)calloc(1, sizeof(struct h_timer));
	if (gTimer == NULL) {
		AIFW_LOGE("Memory allocation failed for timer");
		return AIFW_NO_MEM;
	}
	aifw_timer_result ret;
	/* create timer */
	ret = h_aifw_timer_create(gTimer, (void *)h_timerTaskHandler, NULL, gInterval);
	if (ret != AIFW_TIMER_SUCCESS) {
		AIFW_LOGE("Timer creation failed. ret: %d", ret);
		return AIFW_ERROR;
	}
	AIFW_LOGV("Timer created OK");

	ret = h_aifw_timer_change_interval(gTimer, (unsigned int)gInterval);
	if (ret != AIFW_TIMER_SUCCESS) {
		AIFW_LOGE("timer interval change failed=%d", ret);
		return AIFW_ERROR;
	}
	return AIFW_OK;
}

#ifdef CONFIG_BUILD_KERNEL
int main(int argc, FAR char *argv[])
#else
int hello_main(int argc, char *argv[])
#endif
{
	printf("Hello, World!!\n");

	char check_str1[100] = "0.783092\n";
	char check_str2[100] = "0.819210\0";
	char check_str3[100] = "1000.454355";
	float var1 = atof(check_str1);
	float var2 = atof(check_str2);
	float var3 = atof(check_str3);

	printf("\n\nhello_main %d %d %d %f %f %f %f\n", strlen(check_str1), strlen(check_str2), strlen(check_str3), var1, var2, var3, atof("1000.454355"));

	float f = 100.454F;
	float fv = 100.454;
	float f1 = 6545.454873F;
	float f2 = 6545.454873;
	printf("hello_main %.6f %f %f %f\n", f, fv, f1, f2);
	
	h_prepare();

	while(1) {
		sleep(100);
	}

	return 0;
}
