// Copyright 2015, Google, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This is required on Mac OS X for getting PRI* macros #defined.
#define __STDC_FORMAT_MACROS

#include <assert.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>


const size_t mem_size = 1 << 30;
static int toggles;
static int rounds;
static uint8_t pattern; //0xff
static char *g_mem;

static char *pick_addr() {
  size_t offset = ((uint64_t)rand() << 12) % mem_size;
  return g_mem + offset;
}

class Timer {
  struct timeval start_time_;

 public:
  Timer() {
    // Note that we use gettimeofday() (with microsecond resolution)
    // rather than clock_gettime() (with nanosecond resolution) so
    // that this works on Mac OS X, because OS X doesn't provide
    // clock_gettime() and we don't really need nanosecond resolution.
    int rc = gettimeofday(&start_time_, NULL);
    assert(rc == 0);
  }

  double get_diff() {
    struct timeval end_time;
    int rc = gettimeofday(&end_time, NULL);
    assert(rc == 0);
    return (end_time.tv_sec - start_time_.tv_sec
            + (double) (end_time.tv_usec - start_time_.tv_usec) / 1e6);
  }
};

static void toggle(int iterations, int addr_count) {
  Timer timer;
  for (int j = 0; j < iterations; j++) {
    uint32_t *addrs[addr_count];
    for (int a = 0; a < addr_count; a++)
      addrs[a] = (uint32_t *) pick_addr();

    uint32_t sum = 0;
    for (int i = 0; i < toggles; i++) {
      for (int a = 0; a < addr_count; a++)
        sum += *addrs[a] + 1;
      for (int a = 0; a < addr_count; a++)
        asm volatile("clflush (%0)" : : "r" (addrs[a]) : "memory");
    }

    // Sanity check.  We don't expect this to fail, because reading
    // these rows refreshes them.
    // Skip sanity check unless 0xFF. 0x00 was giving me issues.
    if (pattern == 0xFF && sum != 0) {
      printf("error: sum=%x\n", sum);
      exit(1);
    }
  }

  // Print statistics derived from the time and number of accesses.
  double time_taken = timer.get_diff();
  printf("  Took %.1f ms per address set\n",
         time_taken / iterations * 1e3);
  printf("  Took %g sec in total for %i address sets\n",
         time_taken, iterations);
  int memory_accesses = iterations * addr_count * toggles;
  printf("  Took %.3f nanosec per memory access (for %i memory accesses)\n",
         time_taken / memory_accesses * 1e9,
         memory_accesses);
  int refresh_period_ms = 64;
  printf("  This gives %i accesses per address per %i ms refresh period\n",
         (int) (refresh_period_ms * 1e-3 * iterations * toggles / time_taken),
         refresh_period_ms);
}

void main_prog() {
  g_mem = (char *) mmap(NULL, mem_size, PROT_READ | PROT_WRITE,
                        MAP_ANON | MAP_PRIVATE, -1, 0);
  assert(g_mem != MAP_FAILED);

  printf("clear\n");
  memset(g_mem, pattern, mem_size);
  
  Timer t;
  for (int iter = 0; iter < rounds; iter++) {
    printf("Round %i/%i\n", iter+1, rounds);
    toggle(10, 8); 

    // Scan every byte against the user pattern:
    size_t flips = 0;
    for (size_t i = 0; i < mem_size; i++) {
      if ((uint8_t)g_mem[i] != pattern) {
        printf("Flip @ 0x%zx: was 0x%02X now 0x%02X\n",
               i, pattern, (uint8_t)g_mem[i]);
        flips++;
      }
    }
    if (flips) {
      // At least one real Rowhammer bit-flip
      exit(1);
    }
  }
}


int main(int argc, char *argv[]) {
  if (argc != 4) {
    fprintf(stderr, "Usage: %s <pattern> <toggles> <rounds>\n", argv[0]);
    return 1;
  }

  // Parse inputs
  pattern = strtol(argv[1], NULL, 0);
  toggles = atoi(argv[2]);
  rounds = atoi(argv[3]);

  pid_t child = fork();
  if (child == 0) {
    main_prog();
  }

  int status;
  waitpid(child, &status, 0);
  return WEXITSTATUS(status);
}
