//---------------------------------------------------------------------------------
//Stress kernels (relay N <kernel> <reps>): which part of the CPU fails first above 300 MHz.
//Needs the PLL at 640 with slow zones on (the Wired task, which runs this, is inside a zone, so
//the CPU sits at PLL/3). The reference result is computed at PLL/3, then both cores are frozen
//(interrupts off here, the other core stalled), the divider goes to PLL/2 for the kernel only,
//back to PLL/3, and the results are compared. s_stress_note survives a hang/crash reset.
#include <soc/system_reg.h>
#include <esp_cpu.h>
#include <soc/rtc_cntl_reg.h>

RTC_NOINIT_ATTR uint32_t s_stress_note;   // 0x57000000 | kernel << 8 | rep while at PLL/2
uint32_t turbo_stress_last() { uint32_t v = s_stress_note; s_stress_note = 0; return v; }

static DRAM_ATTR uint32_t s_st_a[1024], s_st_b[1024];
static uint32_t s_st_out1[256][8], s_st_out7[256], s_st_ref1[256][8], s_st_ref7[256];

static IRAM_ATTR __attribute__((noinline)) uint32_t st_mul(uint32_t n, uint32_t x)
{
  uint32_t h = 0;
  for (uint32_t i = 0; i < n; ++i)
  {
    uint32_t lo, hi;
    asm volatile("mull %0, %2, %3\n muluh %1, %2, %3\n" : "=&r"(lo), "=&r"(hi) : "r"(x), "r"(x ^ 0x9E3779B9));
    x = lo + hi + i;
    h ^= x;
  }
  return h;
}
static IRAM_ATTR __attribute__((noinline)) uint32_t st_alu(uint32_t n, uint32_t x)
{
  uint32_t a = x, b = ~x, c = 0x6a09e667;
  for (uint32_t i = 0; i < n; ++i)
  {
    a += (b >> 7 | b << 25) ^ (c >> 18 | c << 14) ^ (b >> 3);
    b += (a & c) ^ (~a & 0x5be0cd19) ^ i;
    c ^= a + b;
  }
  return a ^ b ^ c;
}
static IRAM_ATTR __attribute__((noinline)) uint32_t st_sram(uint32_t n, uint32_t x)
{
  uint32_t h = 0;
  for (uint32_t r = 0; r < n; ++r)
  {
    for (int i = 0; i < 1024; ++i) s_st_a[i] = x + i * 0x01000193u + r;
    for (int i = 0; i < 1024; ++i) s_st_b[1023 - i] = s_st_a[i] ^ (s_st_a[(i + 7) & 1023] >> 3);
    for (int i = 0; i < 1024; ++i) h = (h << 5 | h >> 27) ^ s_st_b[i];
  }
  return h;
}
//same loop as st_alu but in flash (instruction cache path)
static __attribute__((noinline)) uint32_t st_flash(uint32_t n, uint32_t x)
{
  uint32_t a = x, b = ~x, c = 0x6a09e667;
  for (uint32_t i = 0; i < n; ++i)
  {
    a += (b >> 7 | b << 25) ^ (c >> 18 | c << 14) ^ (b >> 3);
    b += (a & c) ^ (~a & 0x5be0cd19) ^ i;
    c ^= a + b;
    asm volatile("" ::: "memory");
  }
  return a ^ b ^ c;
}
static __attribute__((noinline)) uint32_t st_double(uint32_t n, uint32_t x)
{
  volatile double a = 1.0 + x * 1e-9, b = 0.999999 + x * 1e-12;
  for (uint32_t i = 0; i < n; ++i) { a = a * b + 1e-7; a = a / 1.0000003; b = b + a * 1e-15; }
  double r = a * 1e9 + b * 1e3;
  uint64_t u; memcpy(&u, &r, 8);
  return (uint32_t)u ^ (uint32_t)(u >> 32);
}
static __attribute__((noinline)) uint32_t st_printf(uint32_t n, uint32_t x)
{
  char b[48];
  uint32_t h = 0;
  for (uint32_t i = 0; i < n; ++i)
  {
    const int l = snprintf(b, sizeof(b), "%.6g %u", (x + i) * 0.000123456789, (unsigned)(x ^ i));
    for (int k = 0; k < l; ++k) h = h * 31 + b[k];
  }
  return h;
}
static __attribute__((noinline)) uint32_t st_mbed(uint32_t n, uint32_t x)
{
  uint8_t b[80] = {0}, o[32];
  uint32_t h = 0;
  for (uint32_t i = 0; i < n; ++i)
  {
    memcpy(b + 76, &i, 4); memcpy(b, &x, 4);
    mbedtls_sha256_ret(b, 80, o, 0);
    uint32_t w; memcpy(&w, o, 4); h ^= w + i;
  }
  return h;
}

static inline void IRAM_ATTR st_div(uint32_t sel)
{
  const uint32_t per = REG_READ(SYSTEM_CPU_PER_CONF_REG);
  REG_WRITE(SYSTEM_CPU_PER_CONF_REG, (per & ~SYSTEM_CPUPERIOD_SEL) | sel);
}

static uint32_t st_run(uint32_t k, uint32_t x, const uint32_t *mid, const uint32_t *w)
{
  switch (k)
  {
    case 0: { const uint32_t t = CCOUNT(); while (CCOUNT() - t < 3000000) {} return 0; }
    case 1: t5_all(mid, w, x, x + 256, 0, nullptr, nullptr, s_st_out1, s_st_out7);
            { uint32_t h = 0; for (int i = 0; i < 256; ++i) { for (int j = 0; j < 8; ++j) h = (h << 1 | h >> 31) ^ s_st_out1[i][j]; h ^= s_st_out7[i]; } return h; }
    case 2: return st_mul(400000, x);
    case 3: return st_alu(400000, x);
    case 4: return st_sram(40, x);
    case 5: return st_flash(400000, x);
    case 6: return st_double(3000, x);
    case 7: return st_printf(300, x);
    case 8: return st_mbed(300, x);
  }
  return 0;
}

//returns mismatches out of reps; *cyc = kernel cycles at PLL/2 (last rep)
static inline void st_feed()
{
  WRITE_PERI_REG(RTC_CNTL_WDTWPROTECT_REG, 0x50D83AA1);
  WRITE_PERI_REG(RTC_CNTL_WDTFEED_REG, RTC_CNTL_WDT_FEED);
  WRITE_PERI_REG(RTC_CNTL_WDTWPROTECT_REG, 0);
}
//flags bit 0: leave the other core running (it mines at PLL/2 too) instead of stalling it
int turbo_stress(uint32_t k, uint32_t reps, uint32_t flags, char *buf, size_t len)
{
  if (k > 7 || (k == 1 && (flags & 1)) || reps == 0 || reps > 5000) return snprintf(buf, len, "x stress bad args");
  const uint32_t per0 = REG_READ(SYSTEM_CPU_PER_CONF_REG) & SYSTEM_CPUPERIOD_SEL;
  uint8_t hdr[128];
  uint32_t mid[8], w[16];
  test_header(hdr, 11);
  memcpy(w, hdr + 64, sizeof(w));
  uint32_t bad = 0, cyc = 0, done = 0;
  static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
  for (uint32_t r = 0; r < reps; ++r)
  {
    const uint32_t x = 0x1234 + r * 7919;
    if (k == 1) { esp_sha_acquire_hardware(); hw_midstate(hdr, mid); }
    const uint32_t ref = st_run(k, x, mid, w);
    const int other = xPortGetCoreID() ^ 1;
    portENTER_CRITICAL(&mux);
    if (!(flags & 1)) esp_cpu_stall(other);
    s_stress_note = 0x57000000 | (flags << 20) | (k << 16) | (r & 0xffff);
    st_div(2);
    const uint32_t c0 = CCOUNT();
    const uint32_t got = st_run(k, x, mid, w);
    cyc = CCOUNT() - c0;
    st_div(per0);
    s_stress_note = 0;
    if (!(flags & 1)) esp_cpu_unstall(other);
    portEXIT_CRITICAL(&mux);
    st_feed();
    if (k == 1) esp_sha_release_hardware();
    if (got != ref) bad++;
    done++;
    vTaskDelay(1);
  }
  return snprintf(buf, len, "x stress k=%u f=%u reps=%u bad=%u cyc=%u per0=%u", (unsigned)k, (unsigned)flags, (unsigned)done, (unsigned)bad, (unsigned)cyc, (unsigned)per0);
}
