#ifndef __FR__
#define __FR__

#include "arch.h"

#define FR_STRIDE       4096
#define FR_ITEMS         256
#define FR_BUFF_N          1
#define CHECK_COUNT        6
#define FR_BUFF_SIZE     (FR_STRIDE*FR_ITEMS)
#define FR_HIT_THRESHOLD_DEFAULT 200
#define FR_ITERATIONS   100000
#define FR_CALIBRATE_ITERATIONS 1000

#ifndef FR_CALIBRATE
#define FR_CALIBRATE 1
#endif

static unsigned char *fr_buff[FR_BUFF_N];

static unsigned long long fr_histogram[FR_ITEMS];

static uint64_t fr_hit_threshold = FR_HIT_THRESHOLD_DEFAULT;

#define fr_store arch_store_memory
#define fr_load arch_access_memory
#define fr_item_to_addr(I, N) (fr_buff[N]+(I)*FR_STRIDE)

static inline void fr_flush_item(unsigned char item)
{
    for (int i=0; i<FR_BUFF_N; i++)
        arch_flush(fr_item_to_addr(item, i));
}

#define fr_load_item(I) \
    do { \
        for (int i=0; i<FR_BUFF_N; i++) \
            fr_load(fr_item_to_addr(I, i)); \
    } while (0)


static inline uint64_t fr_reload_item_single(unsigned char item, int frN)
{
  uint64_t time = arch_get_timing();

  fr_load(fr_item_to_addr(item, frN));
  arch_memory_barrier();

  time = arch_get_timing() - time;
  return time;
}

static inline uint64_t fr_reload_item(unsigned char item)
{
  uint64_t time = arch_get_timing();

  #pragma unroll
  for (int i=0; i<FR_BUFF_N; i++) {
    fr_load(fr_item_to_addr(item, i));
    arch_memory_barrier();
  }

  time = arch_get_timing() - time;

  fr_flush_item(item);
  arch_memory_barrier();

  return time;
}

static inline void fr_flush()
{
    for (int i=0; i<FR_ITEMS; i++)
        fr_flush_item(i);
    arch_memory_barrier();
}

static inline void fr_reset()
{
    int i, j;
    memset(fr_histogram, 0, sizeof(fr_histogram));
    for (i=0;i<FR_BUFF_N;i++) {
        memset(fr_buff[i], 1, FR_BUFF_SIZE);
    }
    fr_flush();
}

static inline void fr_init()
{
    int i;
    for (i=0;i<FR_BUFF_N;i++) {
        fr_buff[i] = mmap(0, FR_BUFF_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANON, -1, 0);
        assert(fr_buff[i]);
    }
    fr_reset();
}

static inline void fr_reload_items(int start, int end)
{
    if (start < 0 || start >= FR_ITEMS)
        start = 0;
    if (end < 0 || end >= FR_ITEMS)
        end = FR_ITEMS-1;
    for (int i=start; i<=end; i++) {
#if 1
        int cache_hits = 0;
        uint64_t t;
        for (int f=0; f<FR_BUFF_N; f++) {
            t = fr_reload_item_single(i, f);
            if(t < fr_hit_threshold)
                cache_hits++;
        }

        fr_flush_item(i);
        arch_memory_barrier();

        if(cache_hits == FR_BUFF_N)
            fr_histogram[i]++;
#else
        uint64_t t = fr_reload_item(i);
        if (t < fr_hit_threshold)
            fr_histogram[i]++;
#endif
    }
    arch_memory_barrier();
}

static inline void fr_reload(int start, int end)
{
    fr_reload_items(0, FR_ITEMS-1);
}

static inline int fr_histogram_stats(unsigned char *min_item,
    unsigned long long *min_hits, unsigned long long *total_hits)
{
    *min_item = 0;
    *min_hits = ULLONG_MAX-1;
    *total_hits = 0;
    for (int i=0; i<FR_ITEMS; i++) {
        unsigned long long h = fr_histogram[i];
        *total_hits += h;
        if (h == 0 || h >= *min_hits)
            continue;
        *min_hits = h;
        *min_item = i;
    }

    return *total_hits > 0;
}

static inline void fr_histogram_dump()
{
    unsigned char min_item;
    unsigned long long min_hits;
    unsigned long long total_hits;

    printf("fr_histogram_dump:\n");
    int ret = fr_histogram_stats(&min_item, &min_hits, &total_hits);
    if (!ret) {
        printf("  <empty>\n");
        return;
    }
    do {
        float perc = (((float)min_hits)/FR_ITERATIONS)*100;
        unsigned char min_char = min_item > 32 && min_item < 126 ? min_item : '?';
        printf("  '%c' (0x%02x): %5.2f%% %s\n", min_char, min_item, perc, min_item == '-' ? "<----" : "");
        fr_histogram[min_item] = 0;
    } while (fr_histogram_stats(&min_item, &min_hits, &total_hits));
}

static inline void fr_calibrate(int indep)
{
    unsigned long i;
    uint64_t t, max_hit_t = 0;
    uint64_t min_miss_t = 1000 * 1000;
    uint64_t min_hit_t = 1000 * 1000;

    uint64_t time;
    fr_reset();
    for (i=0; i<FR_CALIBRATE_ITERATIONS; i++) {
        fr_store(fr_item_to_addr('A', 0));
        arch_memory_barrier();
        time = arch_get_timing();
        fr_store(fr_item_to_addr('A', 0));
        arch_memory_barrier();
        time = arch_get_timing() - time;
        if(time > max_hit_t)
            max_hit_t = time;
        if(time < min_hit_t)
            min_hit_t = time;
        sched_yield();
    }
#if 0
    for (i=0; i<FR_CALIBRATE_ITERATIONS; i++) {
        fr_load_item('A');
        t = fr_reload_item('A');
        if (t > max_hit_t)
            max_hit_t = t;
        sched_yield();
    }
    for (i=0; i<FR_CALIBRATE_ITERATIONS; i++) {
        t = fr_reload_item('A');
        if (t < min_miss_t)
            min_miss_t = t;
        sched_yield();
    }
#endif
    fr_reset();

//    printf("fr_calibrate: max_hit_t=%lu, min_miss_t=%lu\n", max_hit_t, min_miss_t);
    printf("fr_calibrate: max_hit_t=%lu, min_hit_t=%lu\n", max_hit_t, min_hit_t);
    if(indep)
        fr_hit_threshold = 200;
    else
        fr_hit_threshold = FR_BUFF_N*64;
    printf("fr_calibrate: fr_hit_threshold=%lu\n", fr_hit_threshold);
}

#endif /* __FR__ */
