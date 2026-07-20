#define _POSIX_C_SOURCE 200809L

#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "ztree_main.h"

typedef struct
{
    ztree_t *t;
} thread_arg;

static int *all_keys;
static int total_keys;

/* Per-op insert (load-phase) latency, in ns, indexed by the op's unique
 * work_cursor idx (each idx written by exactly one thread — no locking).
 * NULL = latency measurement off (default). Enable with CTREE_LAT=1. */
static uint64_t *g_lat_ns = NULL;

static _Atomic int progress_count = 0;
static _Atomic int work_cursor = 0;
static _Atomic int verify_cursor = 0;
static _Atomic int found_cnt = 0;
static _Atomic int missing_cnt = 0;
static _Atomic bool monitor_stop = false;

/* Per-second throughput sampler: every 1s, log ops completed in that second.
 * Writes CSV only when CTREE_TPUT_PATH is set; silent otherwise. */
static void *tput_monitor(void *arg)
{
    (void)arg;
    const char *path = getenv("CTREE_TPUT_PATH");
    if (!path || !*path)
        return NULL;
    FILE *fp = fopen(path, "w");
    if (!fp)
        return NULL;
    fprintf(fp, "sec,ops_this_sec,cumulative\n");
    fflush(fp);
    int prev = 0, sec = 0;
    while (!atomic_load(&monitor_stop))
    {
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        int cur = atomic_load(&progress_count);
        sec++;
        fprintf(fp, "%d,%d,%d\n", sec, cur - prev, cur);
        fflush(fp);
        prev = cur;
    }
    fclose(fp);
    return NULL;
}

static void *worker(void *arg)
{
    thread_arg *a = (thread_arg *)arg;

    int total = total_keys;
    int interval = total / 100;
    if (interval < 1)
    {
        interval = 1;
    }

    const int lat_on = (g_lat_ns != NULL);

    int idx;
    while ((idx = atomic_fetch_add(&work_cursor, 1)) < total)
    {
        int key = all_keys[idx];
        char buf[120];
        snprintf(buf, sizeof(buf), "value-%d", key);
        if (lat_on)
        {
            struct timespec s, e;
            clock_gettime(CLOCK_MONOTONIC, &s);
            cow_insert(a->t, key, buf);
            clock_gettime(CLOCK_MONOTONIC, &e);
            g_lat_ns[idx] = (uint64_t)(e.tv_sec - s.tv_sec) * 1000000000ULL
                          + (uint64_t)(e.tv_nsec - s.tv_nsec);
        }
        else
        {
            cow_insert(a->t, key, buf);
        }

        int current = atomic_fetch_add(&progress_count, 1) + 1;
        if (current % interval == 0 || current == total)
        {
            printf("\r> Progress: %d / %d (%.1f%%)   ", current, total,
                   (double)current / total * 100.0);
            fflush(stdout);
        }
    }

    return NULL;
}

/* Post-insert integrity check: every inserted key must be findable. */
static void *verifier(void *arg)
{
    thread_arg *a = (thread_arg *)arg;
    int idx;
    while ((idx = atomic_fetch_add(&verify_cursor, 1)) < total_keys)
    {
        ztree_record *r = ztree_find(a->t, all_keys[idx]);
        if (r) { atomic_fetch_add(&found_cnt, 1); free(r); }
        else   { atomic_fetch_add(&missing_cnt, 1); }
    }
    return NULL;
}

static void reset_device(const char *dev_path)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "sudo nvme zns reset-zone -a %s", dev_path);
    int rc = system(cmd);
    if (rc == -1)
    {
        perror("system(nvme reset-zone)");
    }
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

/* value at percentile p (0<p<1) of the sorted g_lat_ns[0..n), in us
 * (nearest-rank: element ceil(p*n)-1, 0-based). */
static double lat_pct_us(size_t n, double p)
{
    size_t k = (size_t)ceil(p * (double)n);
    if (k) k--;
    if (k >= n) k = n - 1;
    return (double)g_lat_ns[k] / 1e3;
}

/* Report per-op insert (load) latency: mean/stddev + p50/p90/p99/p99.9/p99.99,
 * all in microseconds. Sorts g_lat_ns in place. Appends a row to CTREE_LAT_CSV
 * if set (header written once), and writes a CDF table to CTREE_LAT_CDF. */
static void report_latency(int num_threads)
{
    size_t n = (size_t)total_keys;
    if (!g_lat_ns || n == 0) return;

    double sum = 0.0;
    for (size_t i = 0; i < n; i++) sum += (double)g_lat_ns[i];
    double mean = sum / (double)n;
    double var = 0.0;
    for (size_t i = 0; i < n; i++) { double d = (double)g_lat_ns[i] - mean; var += d * d; }
    double sd = sqrt(var / (double)n);

    qsort(g_lat_ns, n, sizeof(uint64_t), cmp_u64);
    double p50   = lat_pct_us(n, 0.50);
    double p90   = lat_pct_us(n, 0.90);
    double p99   = lat_pct_us(n, 0.99);
    double p999  = lat_pct_us(n, 0.999);
    double p9999 = lat_pct_us(n, 0.9999);
    double mean_us = mean / 1e3, sd_us = sd / 1e3;

    printf("Latency(us) load: mean=%.3f stddev=%.3f p50=%.3f p90=%.3f "
           "p99=%.3f p99.9=%.3f p99.99=%.3f  (n=%zu)\n",
           mean_us, sd_us, p50, p90, p99, p999, p9999, n);

    const char *csv = getenv("CTREE_LAT_CSV");
    if (csv && *csv)
    {
        FILE *fp = fopen(csv, "a");
        if (fp)
        {
            fseek(fp, 0, SEEK_END);
            if (ftell(fp) == 0)
                fprintf(fp, "threads,mean_us,stddev_us,p50_us,p90_us,"
                            "p99_us,p99.9_us,p99.99_us,count\n");
            fprintf(fp, "%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%zu\n",
                    num_threads, mean_us, sd_us, p50, p90, p99, p999, p9999, n);
            fclose(fp);
        }
        else
        {
            fprintf(stderr, "WARNING: cannot open CTREE_LAT_CSV=%s\n", csv);
        }
    }

    /* CDF-ready dump (CTREE_LAT_CDF=<file>): latency_us vs cumulative prob from
     * the full sorted per-op distribution. Body sampled every 0.1%, tail refined
     * geometrically to p99.999 + max, so a CDF (incl. tail) is directly plottable
     * without a decoder. ~1020 rows/run, monotonic in cum_prob. */
    const char *cdf = getenv("CTREE_LAT_CDF");
    if (cdf && *cdf)
    {
        FILE *fp = fopen(cdf, "w");
        if (fp)
        {
            fprintf(fp, "latency_us,cum_prob\n");
            for (size_t i = 0; i < 1000; i++)          /* body: p = 0.000..0.999 */
            {
                size_t k = (size_t)((double)i / 1000.0 * (double)n);
                if (k >= n) k = n - 1;
                fprintf(fp, "%.3f,%.6f\n",
                        (double)g_lat_ns[k] / 1e3, (double)(k + 1) / (double)n);
            }
            for (double d = 3.0; d <= 5.01; d += 0.1)  /* tail: p = 1-10^-d */
            {
                double p = 1.0 - pow(10.0, -d);
                size_t k = (size_t)(p * (double)n);
                if (k >= n) k = n - 1;
                fprintf(fp, "%.3f,%.6f\n",
                        (double)g_lat_ns[k] / 1e3, (double)(k + 1) / (double)n);
            }
            fprintf(fp, "%.3f,%.6f\n", (double)g_lat_ns[n - 1] / 1e3, 1.0);  /* max */
            fclose(fp);
        }
        else
        {
            fprintf(stderr, "WARNING: cannot open CTREE_LAT_CDF=%s\n", cdf);
        }
    }
}

static void run_test(const char *dev_path, int num_threads)
{
    printf("Resetting ZNS device...\n");
    reset_device(dev_path);
    sleep(1);

    atomic_store(&progress_count, 0);
    atomic_store(&work_cursor, 0);
    printf("\n===== Running with %d threads =====\n", num_threads);

    cow_tree *t = cow_open(dev_path);
    if (!t)
    {
        perror("cow_open failed");
        exit(1);
    }

    pthread_t *threads = malloc(sizeof(*threads) * (size_t)num_threads);
    thread_arg *args = malloc(sizeof(*args) * (size_t)num_threads);
    if (!threads || !args)
    {
        perror("malloc threads/args failed");
        free(threads);
        free(args);
        cow_close(t);
        exit(1);
    }

    /* Optional per-op insert latency capture (CTREE_LAT=1). Allocated before
     * the timing window so the 8*total_keys buffer doesn't count as elapsed. */
    g_lat_ns = NULL;
    {
        const char *le = getenv("CTREE_LAT");
        if (le && atoi(le) != 0)
        {
            g_lat_ns = malloc(sizeof(uint64_t) * (size_t)total_keys);
            if (!g_lat_ns)
                fprintf(stderr, "WARNING: latency buffer alloc failed "
                        "(%zu B); latency disabled this run\n",
                        sizeof(uint64_t) * (size_t)total_keys);
        }
    }

    struct timespec start;
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    atomic_store(&monitor_stop, false);
    pthread_t monitor;
    pthread_create(&monitor, NULL, tput_monitor, NULL);

    for (int i = 0; i < num_threads; i++)
    {
        args[i].t = t;

        pthread_create(&threads[i], NULL, worker, &args[i]);
    }

    for (int i = 0; i < num_threads; i++)
    {
        pthread_join(threads[i], NULL);
    }
    atomic_store(&monitor_stop, true);
    pthread_join(monitor, NULL);
    printf("\n");

    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed =
        (end.tv_sec - start.tv_sec) +
        (end.tv_nsec - start.tv_nsec) / 1e9;

    double iops = total_keys / elapsed;

    printf("\nThreads: %d\n", num_threads);
    printf("Elapsed time: %.6f seconds\n", elapsed);
    printf("Average throughput: %.2f ops/sec\n", iops);

    if (g_lat_ns) report_latency(num_threads);

    /* ── Integrity verification (opt-in via VERIFY=1) ── */
    {
        const char *ve = getenv("VERIFY");
        if (ve && atoi(ve) != 0)
        {
            atomic_store(&verify_cursor, 0);
            atomic_store(&found_cnt, 0);
            atomic_store(&missing_cnt, 0);
            struct timespec vstart, vend;
            clock_gettime(CLOCK_MONOTONIC, &vstart);
            for (int i = 0; i < num_threads; i++)
                pthread_create(&threads[i], NULL, verifier, &args[i]);
            for (int i = 0; i < num_threads; i++)
                pthread_join(threads[i], NULL);
            clock_gettime(CLOCK_MONOTONIC, &vend);
            double velapsed =
                (vend.tv_sec - vstart.tv_sec) + (vend.tv_nsec - vstart.tv_nsec) / 1e9;
            int found = atomic_load(&found_cnt);
            int missing = atomic_load(&missing_cnt);
            printf("Verify: found %d / %d (%.2f%%)  missing %d  in %.2f s\n",
                   found, total_keys, 100.0 * found / total_keys, missing, velapsed);
        }
        else
        {
            printf("Verify: skipped (set VERIFY=1 to enable)\n");
        }
    }

    struct timespec close_start, close_end;
    clock_gettime(CLOCK_MONOTONIC, &close_start);
    cow_close(t);
    clock_gettime(CLOCK_MONOTONIC, &close_end);
    double close_elapsed =
        (close_end.tv_sec - close_start.tv_sec) +
        (close_end.tv_nsec - close_start.tv_nsec) / 1e9;
    printf("Elapsed (close): %.6f seconds\n", close_elapsed);
    printf("Elapsed (insert+close): %.6f seconds\n", elapsed + close_elapsed);

    free(threads);
    free(args);
    free(g_lat_ns);
    g_lat_ns = NULL;
}

int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        printf("Usage: %s <key_num> <mode> <device>\n", argv[0]);
        printf("mode: 0=all  1 2 4 8 16 32 64\n");
        return 1;
    }

    total_keys = atoi(argv[1]);
    int mode = atoi(argv[2]);
    const char *dev_path = argv[3];

    all_keys = malloc(sizeof(*all_keys) * (size_t)total_keys);
    if (!all_keys)
    {
        perror("malloc keys failed");
        return 1;
    }

    for (int i = 0; i < total_keys; i++)
    {
        all_keys[i] = i;
    }

    /* BENCH_SEQUENTIAL=1 skips the Fisher-Yates shuffle so threads insert
     * keys in ascending order — used for the sequential-insert comparison
     * across variants.  Default (unset / 0) keeps the original randomised
     * order so existing bench numbers remain comparable. */
    const char *seq_env = getenv("BENCH_SEQUENTIAL");
    int sequential = seq_env && seq_env[0] == '1';

    if (!sequential)
    {
        srand(54321);
        for (int i = total_keys - 1; i > 0; i--)
        {
            int j = rand() % (i + 1);
            int tmp = all_keys[i];
            all_keys[i] = all_keys[j];
            all_keys[j] = tmp;
        }
    }

    printf("%s key set generated (%d keys).\n",
           sequential ? "Sequential" : "Random", total_keys);

    int thread_counts[] = {1, 2, 4, 8, 16, 32, 64, 128, 256, 512};
    int num_configs = (int)(sizeof(thread_counts) / sizeof(thread_counts[0]));

    if (mode == 0)
    {
        for (int i = 0; i < num_configs; i++)
        {
            run_test(dev_path, thread_counts[i]);
        }
    }
    else
    {
        int valid = 0;
        for (int i = 0; i < num_configs; i++)
        {
            if (mode == thread_counts[i])
            {
                run_test(dev_path, mode);
                valid = 1;
                break;
            }
        }

        if (!valid)
        {
            printf("Invalid mode: %d\n", mode);
            printf("Allowed values: 0, 1, 2, 4, 8, 16, 32, 64\n");
            free(all_keys);
            return 1;
        }
    }

    free(all_keys);
    printf("\nAll tests completed.\n");
    return 0;
}
