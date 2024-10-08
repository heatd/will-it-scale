/*
 * Copyright (C) 2010 Anton Blanchard <anton@au.ibm.com>, IBM
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */
#define _GNU_SOURCE
#include <sched.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#if HAVE_HWLOC
#include <hwloc.h>
#endif
#include <sys/types.h>
#include <signal.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

#if HAVE_GETOPT_LONG
#include <getopt.h>
#endif

#define MAX_TASKS 2048
#define MAX_CACHELINE_SIZE 256
#define WARMUP_ITERATIONS 5

extern char *testcase_description;
extern void __attribute__((weak)) testcase_prepare(unsigned long nr_tasks) { }
extern void __attribute__((weak)) testcase_cleanup(void) { }
extern void *testcase(unsigned long long *iterations, unsigned long nr);

static char *initialise_shared_area(unsigned long size)
{
	char template[] = "/tmp/shared_area_XXXXXX";
	int fd;
	char *m;
	int page_size = getpagesize();

	/* Align to page boundary */
	size = (size + page_size-1) & ~(page_size-1);

	fd = mkstemp(template);
	if (fd < 0) {
		perror("mkstemp");
		exit(1);
	}

	if (ftruncate(fd, size) == -1) {
		perror("ftruncate");
		unlink(template);
		exit(1);
	}

	m = mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	if (m == MAP_FAILED) {
		perror("mmap");
		unlink(template);
		exit(1);
	}

	memset(m, 0, size);

	unlink(template);

	return m;
}

static void usage(char *command)
{
	printf("Usage: %s [options]\n\n", command);
	printf("\t-s, --iterations ITERATIONS\tNumber of iterations to run\n");
	printf("\t-t, --tasks TASKS\t\tNumber of threads or processes to run\n");
	printf("\t-m, --smt-affinitize\t\tAffinitize tasks on SMT threads (default cores)\n");
	printf("\t-n, --no-affinity\t\tNo affinity\n");
#if !HAVE_GETOPT_LONG
	printf("Note: This platform does not support long options\n");
#endif
	exit(1);
}

struct args
{
	void *(*func)(unsigned long long *iterations, unsigned long nr);
	unsigned long long *arg1;
	unsigned long arg2;
	int poll_fd;
#if HAVE_HWLOC
	hwloc_topology_t topology;
	hwloc_cpuset_t cpuset;
#endif
};

static void *testcase_trampoline(void *p)
{
	struct args *args = p;
	struct pollfd pfd = { args->poll_fd, POLLIN, 0 };
	int ret;

	do {
		ret = poll(&pfd, 1, -1);
	} while ((ret == -1) && (errno == EINTR));

	return args->func(args->arg1, args->arg2);
}

static bool use_affinity = true;

#ifdef THREADS

#include <pthread.h>
#include <assert.h>

static pthread_t threads[2*MAX_TASKS];
static int nr_threads;

static pid_t thread_controller = 0;

void new_task(void *(func)(void *), void *arg)
{
	pthread_create(&threads[nr_threads++], NULL, func, arg);
}

static void *pre_trampoline(void *p)
{
	struct args *args = p;

#if HAVE_HWLOC
	if (use_affinity && hwloc_set_thread_cpubind(args->topology, pthread_self(), args->cpuset, 0) < 0) {
		perror("hwloc_set_thread_cpubind");
		exit(1);
	}
#endif

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	return testcase_trampoline(args);
}

void new_task_affinity(struct args *args)
{
	pthread_attr_t attr;

	pthread_attr_init(&attr);

	pthread_create(&threads[nr_threads++], &attr, pre_trampoline, args);

	pthread_attr_destroy(&attr);
}

/* All threads will die when we exit */
static void kill_tasks(void)
{
	int i;

	for (i = 0; i < nr_threads; i++) {
		pthread_cancel(threads[i]);
	}

	for (i = 0; i < nr_threads; i++) {
		pthread_join(threads[i], NULL);
	}
}

#else
#include <signal.h>

static int pids[2*MAX_TASKS];
static int nr_pids;

/* Watchdog used to make sure all children exit when the parent does */
static int parent_pid;
static void watchdog(int junk)
{
	if (kill(parent_pid, 0) == -1)
		exit(0);

	alarm(1);
}

void new_task(void *(func)(void *), void *arg)
{
	int pid;

	parent_pid = getpid();

	pid = fork();
	if (pid < 0) {
		perror("fork");
		exit(1);
	}

	if (!pid) {
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = watchdog;
		sa.sa_flags = SA_RESTART;
		sigaction(SIGALRM, &sa, NULL);
		alarm(1);

		func(arg);
	}

	pids[nr_pids++] = pid;
}

void new_task_affinity(struct args *args)
{
	int pid;

	parent_pid = getpid();

	pid = fork();
	if (pid < 0) {
		perror("fork");
		exit(1);
	}

	if (!pid) {
		struct sigaction sa;

		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = watchdog;
		sa.sa_flags = SA_RESTART;
		sigaction(SIGALRM, &sa, NULL);
		alarm(1);

		testcase_trampoline(args);
	}

	pids[nr_pids++] = pid;
}


static void kill_tasks(void)
{
	int i;

	for (i = 0; i < nr_pids; i++)
		kill(pids[i], SIGTERM);
	for (i = 0; i < nr_pids; i++)
		waitpid(pids[i], NULL, 0);
}
#endif

static sig_atomic_t should_bail = 0;

static void sigint_handler(int sig)
{
#ifdef THREADS
	kill(thread_controller, SIGTERM);
#else
	should_bail = 1;
#endif
}

#if HAVE_GETOPT_LONG
static const struct option long_options[] = {
    {"help", no_argument, NULL, 'h'},
	{"iterations", required_argument, NULL, 's'},
	{"tasks", required_argument, NULL, 't'},
	{"smt-affinitize", no_argument, NULL, 'm'},
	{"no-affinity", no_argument, NULL, 'n'},
	{}
};

#define GETOPT(argc, argv, shortopts) getopt_long((argc), (argv), (shortopts), long_options, NULL)
#else
#define GETOPT(argc, argv, shortopts) getopt((argc), (argv), (shortopts))
#endif

int main(int argc, char *argv[])
{
	int opt;
	int opt_tasks = 1;
	int opt_iterations = 0;
	int iterations = 0;
	int i;
	char *m;
	static unsigned long long *results[MAX_TASKS];
#if HAVE_HWLOC
	hwloc_topology_t topology;
	int n;
#endif
	unsigned long long prev[MAX_TASKS] = {0, };
	unsigned long long total = 0;
	int fd[2];
	bool smt_affinity = false;
	struct args *args;
	bool verbose = false;

	while ((opt = GETOPT(argc, argv, "mt:s:hvn")) != -1) {
		switch (opt) {
			case 'm':
				smt_affinity = true;
				break;

			case 't':
				opt_tasks = atoi(optarg);
				if (opt_tasks > MAX_TASKS) {
					printf("tasks cannot exceed %d\n",
					       MAX_TASKS);
					return 1;
				}

				if (opt_tasks == 0) {
					printf("tasks cannot be 0\n");
					return 1;
				}
				break;

			case 's':
				opt_iterations = atoi(optarg);
				break;

			case 'v':
				verbose = true;
				break;

			case 'n':
				use_affinity = false;
				break;

			default:
				usage(argv[0]);
		}
	}

	if (optind < argc)
		usage(argv[0]);

	if (smt_affinity && (use_affinity == false))
		usage(argv[0]);

	m = initialise_shared_area(opt_tasks * MAX_CACHELINE_SIZE);
	for (i = 0; i < opt_tasks; i++)
		results[i] = (unsigned long long *)&m[i * MAX_CACHELINE_SIZE];

	if (pipe(fd) == -1) {
		perror("pipe");
		return 1;
	}

	struct sigaction sa;
	sa.sa_flags = SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sa.sa_handler = sigint_handler;
	sigaction(SIGINT, &sa, NULL);

	testcase_prepare(opt_tasks);

#ifdef THREADS
	thread_controller = fork();

	if (thread_controller < 0) {
		perror("fork");
		return 1;
	} else if (thread_controller > 0) {
		pid_t wpid = wait(NULL);
		assert(wpid == thread_controller);
		goto out;
	} else {
		/* We don't need to handle SIGINT here, it's counterproductive even,
		 * as tty semantics send SIGINT to every process in the process group.
		 */
		sa.sa_handler = SIG_IGN;
		sigaction(SIGINT, &sa, NULL);
	}
#endif

#if HAVE_HWLOC
	hwloc_topology_init(&topology);
	hwloc_topology_load(topology);

	n = hwloc_get_nbobjs_by_type(topology,
			smt_affinity ? HWLOC_OBJ_PU : HWLOC_OBJ_CORE);
	if (n == 0) {
		printf("No Cores/PUs found. Try %s -m flag\n",
		       smt_affinity ? "removing" : "adding");
		return 1;
	}
	if (n < 1) {
		perror("hwloc_get_nbobjs_by_type");
		return 1;
	}
#endif

	args = malloc(opt_tasks * sizeof(struct args));
	if (!args) {
		perror("malloc");
		return 1;
	}

	for (i = 0; i < opt_tasks; i++) {
#if HAVE_HWLOC
		hwloc_obj_t obj;
		hwloc_cpuset_t old_cpuset;
		int flags = 0;
#endif
		args[i].func = testcase;
		args[i].arg1 = results[i];
		args[i].arg2 = i;
		args[i].poll_fd = fd[0];

#if HAVE_HWLOC
		obj = hwloc_get_obj_by_type(topology,
				smt_affinity ? HWLOC_OBJ_PU : HWLOC_OBJ_CORE,
				i % n);

		if (hwloc_topology_dup(&args[i].topology, topology)) {
			perror("hwloc_topology_dup");
			return 1;
		}

		if (!(args[i].cpuset = hwloc_bitmap_dup(obj->cpuset))) {
			perror("hwloc_bitmap_dup");
			return 1;
		}

		old_cpuset = hwloc_bitmap_alloc();
		if (!old_cpuset) {
			perror("hwloc_bitmap_alloc");
			return 1;
		}
#ifdef THREADS
		flags |= HWLOC_CPUBIND_THREAD;
#endif
		if (hwloc_get_cpubind(topology, old_cpuset, flags) < 0) {
			perror("hwloc_get_cpubind");
			return 1;
		}

		if (use_affinity && hwloc_set_cpubind(topology, obj->cpuset, flags) < 0) {
			perror("hwloc_set_cpubind");
			return 1;
		}
#endif
		new_task_affinity(&args[i]);

#ifdef HAVE_HWLOC
		if (use_affinity && hwloc_set_cpubind(topology, old_cpuset, flags) < 0) {
			perror("hwloc_set_cpubind");
			return 1;
		}

		hwloc_bitmap_free(old_cpuset);
#endif
	}

	if (write(fd[1], &i, 1) != 1) {
		perror("write");
		return 1;
	}

#if HAVE_HWLOC
	hwloc_topology_destroy(topology);
#endif
	printf("testcase:%s\n", testcase_description);

	printf("warmup\n");

	while (!should_bail) {
		unsigned long long sum = 0, min = -1ULL, max = 0;

		sleep(1);

		for (i = 0; i < opt_tasks; i++) {
			unsigned long long val = *(results[i]);
			unsigned long long diff = val - prev[i];

			if (verbose)
				printf("%4d -> %llu\n", i, diff);

			if (diff < min)
				min = diff;

			if (diff > max)
				max = diff;

			sum += diff;
			prev[i] = val;
		}

		printf("min:%llu max:%llu total:%llu\n", min, max, sum);

		if (iterations == WARMUP_ITERATIONS)
			printf("measurement\n");

		if (iterations++ > WARMUP_ITERATIONS)
			total += sum;

		if (opt_iterations &&
		    (iterations > (opt_iterations + WARMUP_ITERATIONS))) {
			printf("average:%llu\n", total / opt_iterations);
#ifdef THREADS
			return 0;
#else
			break;
#endif
		}
	}

	kill_tasks();

#if HAVE_HWLOC
	for (i = 0; i < opt_tasks; i++) {
		hwloc_bitmap_free(args[i].cpuset);
		hwloc_topology_destroy(args[i].topology);
	}
#endif
	free(args);
#ifdef THREADS
out:
#endif

	testcase_cleanup();
	return 0;
}
