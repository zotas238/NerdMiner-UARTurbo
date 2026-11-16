//Turbo HW miner: the ESP32-S3 SHA engine driven directly (see turbo_hw.cpp)
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../mining_job.h"

//Self-tests and picks the engine; call from the HW miner task before mining
void turbo_hw_init();
//Mines job into result (nonce_count = nonces done; stops early when cur_job changes)
void turbo_hw_job(const JobRequest &job, JobResult &result, volatile uint8_t &cur_job);
//"t4", "t1" or "stock"
const char *turbo_hw_mode_name();
//Status JSON fields (no braces) for the status port
int turbo_hw_status(char *buf, size_t len);
//Call once the board has run a while: resets the crash counter
void turbo_crash_guard_stable();

extern volatile uint32_t g_hw_hashes;
//Relay U: bus/split-burst benchmark, one line into buf (holds the SHA engine < 1 s)
int turbo_bus_bench(char *buf, size_t len);
int turbo_stress(uint32_t k, uint32_t reps, uint32_t flags, char *buf, size_t len);
uint32_t turbo_stress_last();
int turbo_exp_bench(char *buf, size_t len);
int turbo_exp_bench_core0(char *buf, size_t len, int kind = 0);
void turbo_hw_recal_request();
void turbo_t5_waits(uint32_t wa, uint32_t wb, uint32_t wl, uint32_t wn, uint32_t *out);

//v52 feed mode: core 1 does the first hash of its own nonces (turbo_sw_first_asm) and
//leaves the digests in this ring; the HW core runs only the second hash on them after
//each batch. Single producer (core 1, head), single consumer (core 0, tail).
#define TURBO_FEED_N 128   //v53: was 64
struct TurboFeedE
{
  uint32_t d[8];
  uint32_t nonce;
  uint32_t tag;   //job id & 0xFF
};
extern TurboFeedE g_feed[TURBO_FEED_N];
extern volatile uint32_t g_feed_head, g_feed_tail;
extern volatile uint32_t g_feed_on;   //t5, self-test passed, not switched off by relay f
extern volatile uint32_t g_fd_made;   //first hashes core 1 put in the ring
//relay f <0|1> [min] [max] [flags] (-1 / 0 keep); one line of feed state into buf
int turbo_feed_cmd(int want, uint32_t mn, uint32_t mx, int flags, char *buf, size_t len);
