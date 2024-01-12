#ifndef __ARCH__
#define __ARCH__

#include <time.h>

/* 
 * Based on: https://github.com/IAIK/armageddon.
 */

static inline void arch_flush(void* address)
{
  asm volatile ("DC CIVAC, %0" :: "r"(address));
  asm volatile ("DSB ISH");
  asm volatile ("ISB");
}


static inline void
arch_access_memory_byte(void* pointer)
{
  volatile uint8_t value;
  asm volatile ("LDRB %w0, [%1]\n\t"
      : "=r" (value)
      : "r" (pointer)
      );
}


static inline void
arch_access_memory(void* pointer)
{
  volatile uint64_t value;
  asm volatile ("LDR %0, [%1]\n\t"
      : "=r" (value)
      : "r" (pointer)
      );
}

static inline void
arch_store_memory_byte(void* pointer)
{
    volatile uint8_t value;
    asm volatile ("STRB %w0, [%1]\n\t"
        : "=r" (value)
        : "r" (pointer)
    );
}

static inline void
arch_store_memory(void* pointer)
{
  volatile uint64_t value;
  asm volatile ("STR %0, [%1]\n\t"
      : "=r" (value)
      : "r" (pointer)
      );
}


static inline void
arch_memory_barrier(void)
{
  asm volatile ("DSB SY");
  asm volatile ("ISB");
}

static inline uint64_t
get_monotonic_time(void)
{
  struct timespec t1;
  clock_gettime(CLOCK_MONOTONIC, &t1);
  return t1.tv_sec * 1000*1000*1000ULL + t1.tv_nsec;
}

static inline uint64_t
arch_get_timing()
{
  uint64_t result = 0;

  arch_memory_barrier();

  result = get_monotonic_time();

  arch_memory_barrier();

  return result;
}

#endif /* __ARCH__ */
