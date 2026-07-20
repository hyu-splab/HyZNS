// SPDX-License-Identifier: MIT
#include <sys/time.h>
#include <sched.h>
#include <sys/mman.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>  // for sleep()
#include <time.h>

#include "bench.h"
#include "cpupol.h"
#include "rdtsc.h"

static struct bench *running_bench;

static uint64_t usec(void)
{
        struct timeval tv;
        gettimeofday(&tv, 0);
        return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static void print_time_from_usec(uint64_t usec_time) {
    time_t sec = usec_time / 1000000;    // 초로 변환
    suseconds_t usec = usec_time % 1000000; // 남은 마이크로초

    struct tm tm_info;
    localtime_r(&sec, &tm_info);

    printf("%04d-%02d-%02d %02d:%02d:%02d.%03ld ",
        tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
        tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
        usec / 1000); // 밀리초로 변환
}

static void *stats_printer(void *arg) {
    struct bench *bench = (struct bench *)arg;

    uint64_t start_us = usec();
    uint64_t last_cum = 0;
    uint64_t last_report_finish_us = start_us; // 마지막 출력 시간을 저장

    while (!bench->stop) {
        usleep(1000000);

        uint64_t now = usec();
        uint64_t usecs_since_last = now - last_report_finish_us; // 마지막 출력 이후 경과한 시간

        print_time_from_usec(now);

        // 누적 작업 수 계산
        uint64_t cum_works = 0;
        for (int i = 0; i < bench->ncpu; i++){
                cum_works += atomic_load(&bench->workers[i].atomic_works);
        }

        uint64_t interval_ops = cum_works - last_cum;
        double interval_secs = (double) usecs_since_last / 1000000; // 실제 경과된 시간 (초 단위)
        double total_secs = (double)(now - start_us) / 1000000; // 총 경과 시간

        printf(",Interval_Works: %lu ,Interval_Sec: %lf ,Interval_Rate(ops/s): %.2f ,"
                "Total_Works: %lu ,Total_Secs: %lf ,Total_Rate(ops/s): %.2f\n",
                interval_ops, interval_secs, interval_ops / interval_secs,
                cum_works, total_secs, cum_works / total_secs);
        fflush(stdout);

        last_cum = cum_works;
        last_report_finish_us = now;

    }

    return NULL;
}

static inline void nop_pause(void)
{
        __asm __volatile("pause");
}

static inline void wmb(void)
{
        __asm__ __volatile__("sfence":::"memory");
}

static int setaffinity(int c)
{
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(c, &cpuset);
        return sched_setaffinity(0, sizeof(cpuset), &cpuset);
}

struct bench *alloc_bench(int ncpu, int nbg)
{
        struct bench *bench; 
        struct worker *worker;
        void *shmem;
        int shmem_size = sizeof(*bench) + sizeof(*worker) * ncpu;
        int i;
        
        /* alloc shared memory using mmap */
        shmem = mmap(0, shmem_size, PROT_READ | PROT_WRITE, 
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        if (shmem == MAP_FAILED)
                return NULL;
        memset(shmem, 0, shmem_size);

        /* init. */ 
        bench = (struct bench *)shmem;
        bench->ncpu = ncpu; 
        bench->nbg  = nbg;
        bench->workers = (struct worker*)(shmem + sizeof(*bench));
        for (i = 0; i < ncpu; ++i) {
                worker = &bench->workers[i];
                worker->bench = bench;
                worker->id = seq_cores[i];
		worker->is_bg = i >= (ncpu - nbg);
        }

        return bench;
}

static void sighandler(int x)
{
        running_bench->stop = 1;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"

static void worker_main(void *arg)
{
        struct worker *worker = (struct worker*)arg;
        struct bench *bench = worker->bench;
        uint64_t s_clk = 1, s_us = 1;
        uint64_t e_clk = 0, e_us = 0;
        int err = 0;

        /* set affinity */ 
        setaffinity(worker->id);

        /* pre-work */
        if (bench->ops.pre_work) {
                err = bench->ops.pre_work(worker);
                if (err) goto err_out;
        }

        /* wait for start signal */ 
        worker->ready = 1;
        if (worker->id) {
                while (!bench->start)
                        nop_pause();
        }
	else {
		/* are all workers ready? */
		int i;
		for (i = 1; i < bench->ncpu; i++) {
			struct worker *w = &bench->workers[i];
			while (!w->ready)
				nop_pause();
		}
		/* make things more deterministic */
		sync();

		/* start performance profiling */
		if (bench->profile_start_cmd[0])
			system(bench->profile_start_cmd);

                /* ok, before running, set timer */
                if (signal(SIGALRM, sighandler) == SIG_ERR) {
                        err = errno;
                        goto err_out;
                }
                running_bench = bench;
                alarm(bench->duration);
                bench->start = 1;
                wmb();
        }

        /* start time */
        s_clk = rdtsc_beg();
        s_us = usec();
        
        pthread_t stats_thread;
        if (worker->id == 0 && bench->periodic_output) {
                int rc = pthread_create(&stats_thread, NULL, stats_printer, bench);
                if (rc) {
                        fprintf(stderr, "Failed to create stats thread: %s\n", strerror(rc));
                }
        }

        /* main work */
        if (bench->ops.main_work) {
                err = bench->ops.main_work(worker);
                if (err && err != ENOSPC)
                        goto err_out;
        }

        if (worker->id == 0 && bench->periodic_output) {
                // bench->stop 은 이미 signal handler 에 의해 1 로 설정된 상태
                pthread_join(stats_thread, NULL);
        }

        /* end time */ 
        e_clk = rdtsc_end();
        e_us = usec();

	/* stop performance profiling */
        if (!worker->id && bench->profile_stop_cmd[0])
		system(bench->profile_stop_cmd);

        /* post-work */ 
        if (bench->ops.post_work)
                err = bench->ops.post_work(worker);
err_out:
        worker->ret = err;
        worker->usecs = e_us - s_us;
        wmb();
        worker->clocks = e_clk - s_clk;
}

static void wait(struct bench *bench)
{
        int i;
        for (i = 0; i < bench->ncpu; i++) {
                struct worker *w = &bench->workers[i];
                while (!w->clocks)
                        nop_pause();
        }
}

void run_bench(struct bench *bench)
{
        int i;
	for (i = 1; i < bench->ncpu; ++i) {
		/**
		 * fork() is intentionally used instead of pthread
		 * to avoid known scalability bottlenecks 
		 * of linux virtual memory subsystem. 
		 */ 
		pid_t p = fork();
		if (p < 0)
			bench->workers[i].ret = errno;
		else if (!p) {
			worker_main(&bench->workers[i]);
			exit(0);
		}
	}
	worker_main(&bench->workers[0]);
	wait(bench);
}

void report_bench(struct bench *bench, FILE *out)
{
	static char *empty_str = "";
        uint64_t total_usecs = 0;
        double   total_works = 0.0;
        double   avg_secs;
	char *profile_name, *profile_data;
        int i, n_fg_cpu;

        /* if report_bench is overloaded */ 
        if (bench->ops.report_bench) {
                bench->ops.report_bench(bench, out);
                return;
        }

        /* default report_bench impl. */
        for (i = 0; i < bench->ncpu; ++i) {
                struct worker *w = &bench->workers[i];
		if (w->is_bg) continue;
                total_usecs += w->usecs;
                total_works += w->works;
        }
	n_fg_cpu = bench->ncpu - bench->nbg;
        avg_secs = (double)total_usecs/(double)n_fg_cpu/1000000.0;

	/* get profiling result */ 
	profile_name = profile_data = empty_str;
	if (bench->profile_stat_file[0]) {
		FILE *fp = fopen(bench->profile_stat_file, "r");
		size_t len;
		
		if (fp) {
			profile_name = profile_data = NULL;
			getline(&profile_name, &len, fp);
			getline(&profile_data, &len, fp);
			fclose(fp);
		}
	}

        fprintf(out, "# ncpu secs works works/sec %s\n", profile_name);
        fprintf(out, "%d %f %f %f %s\n", 
                n_fg_cpu, avg_secs, total_works, total_works/avg_secs, profile_data);

	if (profile_name != empty_str)
		free(profile_name);
	if (profile_data != empty_str)
		free(profile_data);
}

#pragma GCC diagnostic pop
