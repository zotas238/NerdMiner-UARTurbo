//BBPLL overclock for the wired build. Everything clocked from the PLL scales with it
//(CPU, APB, SHA engine, flash/PSRAM, USB, Wi-Fi); the wired build keeps Wi-Fi off and
//talks over UART0 clocked from the 40 MHz crystal, so only the CPU side matters.
//
//v2 (2026-11-03): v1 only rewrote the divider of the running PLL, and the board hung at the
//first step (div 9, interrupt WDT). This does what ESP-IDF's rtc_clk does when it changes the
//PLL frequency: MSPI (flash + PSRAM) into its low-speed mode first (its timing tuning is for
//exactly 80 MHz), CPU onto the crystal, PLL powered down and up so it calibrates again, every
//BBPLL analog register written as IDF writes them for 480 MHz except the divider, then wait
//for calibration end and lock before the CPU goes back on it. Returning to div 8 restores
//MSPI high-speed mode. If the new divider won't lock it goes back to 8; if that fails too it
//stays on the crystal (slow, but alive and still on the wire).
#include <Arduino.h>
#include <esp_attr.h>
#include <soc/system_reg.h>
#include <soc/rtc_cntl_reg.h>
#include <esp_spi_flash.h>
#include "turbo_pll.h"
#include <esp_cpu.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

extern "C" void rom_i2c_writeReg(uint8_t block, uint8_t host_id, uint8_t reg_add, uint8_t data);
extern "C" uint8_t rom_i2c_readReg(uint8_t block, uint8_t host_id, uint8_t reg_add);
extern "C" void rom_i2c_writeReg_Mask(uint8_t block, uint8_t host_id, uint8_t reg_add, uint8_t msb, uint8_t lsb, uint8_t data);
extern "C" uint8_t rom_i2c_readReg_Mask(uint8_t block, uint8_t host_id, uint8_t reg_add, uint8_t msb, uint8_t lsb);
extern "C" void spi_timing_change_speed_mode_cache_safe(bool switch_down);

#define I2C_BBPLL 0x66
#define I2C_BBPLL_HOSTID 1
#define BBPLL_DIV_REG 3
#define PLL_MAGIC 0x504C4C21
// IDF values for 480 MHz from a 40 MHz crystal (rtc_clk_bbpll_configure, esp32s3)
#define BB_DIV_REF 0
#define BB_DCHGP 5
#define BB_DCUR 3
#define BB_DBIAS 2

RTC_NOINIT_ATTR static uint32_t s_pll_magic;
RTC_NOINIT_ATTR static uint32_t s_pll_try;
RTC_NOINIT_ATTR static volatile uint32_t s_stage;
static uint32_t s_last_stage = 0;
#define STAGE(n) (s_stage = (n))
static DRAM_ATTR volatile uint8_t s_div = 8;
static uint8_t s_last_try = 0;
static bool s_mspi_low = false;
static uint32_t s_last_cal = 0, s_last_lock = 0, s_last_fallback = 0;
static DRAM_ATTR uint8_t s_mode = 0;
static DRAM_ATTR uint8_t s_last_r8 = 0;   // reg 8 right after the last overclock attempt: lock 7, cal end 6, cal ovf 5, udf 4, cap 3:0
uint8_t turbo_pll_last_r8() { return s_last_r8; }
static DRAM_ATTR uint8_t s_ref = 0;        // BBPLL reference divider: VCO = 40 MHz * (div + 4) / (ref + 1)
static DRAM_ATTR uint8_t s_cur_ref = 0;
static DRAM_ATTR uint8_t s_boot_bb[16];    // BBPLL analog registers as the bootloader left them (stock 480 MHz)
struct knob_t { uint8_t reg, msb, lsb, val; };
static DRAM_ATTR knob_t s_knob[6];
static DRAM_ATTR volatile uint8_t s_nknob = 0;
static uint8_t s_boot_vdig = 0xff;
// the core voltage the chip came up with, kept across panic/software resets (which don't reset the
// regulator), so a raised value can't become the next boot's baseline
RTC_NOINIT_ATTR static uint32_t s_vdig_magic;
RTC_NOINIT_ATTR static uint32_t s_vdig_orig;
#define VDIG_MAGIC 0x56444947         // core voltage code (I2C_DIG_REG reg 6 bits 4:0) at boot
#define I2C_DIG_REG 0x6D
#define I2C_DIG_REG_HOSTID 1

void turbo_pll_boot()
{
  const uint8_t v_now = rom_i2c_readReg_Mask(I2C_DIG_REG, I2C_DIG_REG_HOSTID, 6, 4, 0);
  if (s_vdig_magic != VDIG_MAGIC || esp_reset_reason() == ESP_RST_POWERON || s_vdig_orig > 31)
  {
    s_vdig_magic = VDIG_MAGIC;
    s_vdig_orig = v_now;
  }
  s_boot_vdig = (uint8_t)s_vdig_orig;
  if (v_now != s_boot_vdig)
    rom_i2c_writeReg_Mask(I2C_DIG_REG, I2C_DIG_REG_HOSTID, 6, 4, 0, s_boot_vdig);
  for (int r = 0; r < 16; ++r) s_boot_bb[r] = rom_i2c_readReg(I2C_BBPLL, I2C_BBPLL_HOSTID, r);
  s_last_try = (s_pll_magic == PLL_MAGIC) ? (uint8_t)s_pll_try : 0;
  s_last_stage = (s_pll_magic == PLL_MAGIC) ? s_stage : 0;
  s_stage = 0;
  s_pll_magic = PLL_MAGIC;
  s_pll_try = 0;
  s_div = rom_i2c_readReg(I2C_BBPLL, I2C_BBPLL_HOSTID, BBPLL_DIV_REG);
}

uint8_t turbo_pll_get() { return s_div; }
uint8_t turbo_pll_last_try() { return s_last_try; }
uint32_t turbo_pll_last_stage() { return s_last_stage; }
void turbo_pll_diag(uint32_t *cal, uint32_t *lock, uint32_t *fallback)
{
  *cal = s_last_cal; *lock = s_last_lock; *fallback = s_last_fallback;
}

static inline uint32_t IRAM_ATTR ccount()
{
  uint32_t c;
  asm volatile("rsr %0, ccount" : "=a"(c));
  return c;
}

static inline void IRAM_ATTR spin(uint32_t cyc)
{
  const uint32_t t = ccount();
  while (ccount() - t < cyc) {}
}

//Powers the PLL down and up, programs it for divider div and waits (on the crystal, 40 MHz
//ccount) for calibration end and lock. Returns bit 0 = cal end, bit 1 = lock.
static uint32_t IRAM_ATTR bbpll_program(uint8_t div, uint32_t base)
{
  STAGE(base + 1);
  SET_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_BB_I2C_FORCE_PD | RTC_CNTL_BBPLL_FORCE_PD | RTC_CNTL_BBPLL_I2C_FORCE_PD);
  spin(400);   // 10 us
  STAGE(base + 2);
  CLEAR_PERI_REG_MASK(RTC_CNTL_OPTIONS0_REG, RTC_CNTL_BB_I2C_FORCE_PD | RTC_CNTL_BBPLL_FORCE_PD | RTC_CNTL_BBPLL_I2C_FORCE_PD);
  spin(400);
  STAGE(base + 3);
  // reg 2: dchgp + ref divider, ENB_FCAL (bit 7) clear = frequency calibration on
  rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, 2, (BB_DCHGP << 4) | BB_DIV_REF);
  rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, 3, div);
  rom_i2c_writeReg_Mask(I2C_BBPLL, I2C_BBPLL_HOSTID, 5, 2, 0, 0);   // DR1
  rom_i2c_writeReg_Mask(I2C_BBPLL, I2C_BBPLL_HOSTID, 5, 6, 4, 0);   // DR3
  rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, 6, (1 << 6) | (3 << 4) | BB_DCUR);   // DLREF_SEL, DHREF_SEL, DCUR
  rom_i2c_writeReg_Mask(I2C_BBPLL, I2C_BBPLL_HOSTID, 9, 1, 0, BB_DBIAS);   // VCO_DBIAS
  STAGE(base + 4);
  uint32_t r = 0;
  const uint32_t t0 = ccount();
  while (ccount() - t0 < 400000)   // 10 ms
  {
    const uint8_t s = rom_i2c_readReg(I2C_BBPLL, I2C_BBPLL_HOSTID, 8);
    if ((s & 0x40) && (s & 0x80)) { r = 3; break; }
  }
  if (!r)
  {
    const uint8_t s = rom_i2c_readReg(I2C_BBPLL, I2C_BBPLL_HOSTID, 8);
    r = ((s >> 6) & 1) | (((s >> 7) & 1) << 1);
  }
  STAGE(base + 5 + r * 1000);
  spin(8000);   // 200 us more to settle
  return r;
}

static uint32_t IRAM_ATTR bbpll_live(uint8_t div, uint32_t base)
{
  STAGE(base + 7);
  if (div == 8)
  {
    for (int r = 4; r <= 6; ++r) rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, r, s_boot_bb[r]);
    rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, 1, s_boot_bb[1]);
    rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, 2, s_boot_bb[2]);
    s_cur_ref = s_boot_bb[2] & 0x0f;
  }
  else
    for (uint8_t k = 0; k < s_nknob; ++k)
      rom_i2c_writeReg_Mask(I2C_BBPLL, I2C_BBPLL_HOSTID, s_knob[k].reg, s_knob[k].msb, s_knob[k].lsb, s_knob[k].val);
  // bit 16: one step more VCO bias for the higher frequencies (stock 2), back to 2 at div 8
  rom_i2c_writeReg_Mask(I2C_BBPLL, I2C_BBPLL_HOSTID, 9, 1, 0, ((s_mode & 16) && div != 8) ? 3 : BB_DBIAS);
  const uint8_t ref = div == 8 ? 0 : s_ref;
  if (ref != s_cur_ref)
    rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, 2, (BB_DCHGP << 4) | ref);
  rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, 3, div);
  s_cur_ref = ref;
  if (s_mode & 8)
  {
    // restart the VCO frequency calibration: ENB_FCAL (reg 2 bit 7) up, then down
    rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, 2, (BB_DCHGP << 4) | ref | 0x80);
    spin(400);
    rom_i2c_writeReg(I2C_BBPLL, I2C_BBPLL_HOSTID, 2, (BB_DCHGP << 4) | ref);
  }
  uint32_t r = 0;
  const uint32_t t0 = ccount();
  while (ccount() - t0 < 400000)
    if (rom_i2c_readReg(I2C_BBPLL, I2C_BBPLL_HOSTID, 8) & 0x80) { r = 3; break; }
  if (div != 8) s_last_r8 = rom_i2c_readReg(I2C_BBPLL, I2C_BBPLL_HOSTID, 8);
  STAGE(base + 8 + r * 1000);
  spin(200000);   // 5 ms at 40 MHz: lock is reported before the loop has settled
  return r;
}
static inline uint32_t IRAM_ATTR program(uint8_t div, uint32_t base)
{
  return (s_mode & 1) ? bbpll_program(div, base) : bbpll_live(div, base);
}

//returns the status of the final programming; *fallback = 1 back on 8, 2 left on the crystal
static uint32_t IRAM_ATTR pll_switch(uint8_t div, bool dry, uint32_t *cyc, uint32_t *fallback)
{
  const uint32_t sys = REG_READ(SYSTEM_SYSCLK_CONF_REG);
  REG_WRITE(SYSTEM_SYSCLK_CONF_REG, sys & ~(SYSTEM_SOC_CLK_SEL_M | SYSTEM_PRE_DIV_CNT_M));   // CPU on the crystal
  const uint32_t t0 = ccount();
  STAGE(30);
  uint32_t st = program(div, 100);
  *fallback = 0;
  if (st != 3 || dry)
  {
    const uint32_t st8 = program(8, 200);
    if (st != 3) *fallback = st8 == 3 ? 1 : 2;
    if (st8 != 3) { *cyc = ccount() - t0; return st; }   // no PLL: stay on the crystal
    if (st != 3 || dry) div = 8;
  }
  *cyc = ccount() - t0;
  STAGE(40);
  if (s_mode & 32)
  {
    // onto the PLL at its slowest CPU divider first, then the real one
    const uint32_t per = REG_READ(SYSTEM_CPU_PER_CONF_REG);
    REG_WRITE(SYSTEM_CPU_PER_CONF_REG, per & ~SYSTEM_CPUPERIOD_SEL);
    spin(400);
    REG_WRITE(SYSTEM_SYSCLK_CONF_REG, sys);   // back on the PLL, CPU = PLL/6
    spin(20000);
    REG_WRITE(SYSTEM_CPU_PER_CONF_REG, per);   // CPU = PLL/2
    spin(20000);
  }
  else
    REG_WRITE(SYSTEM_SYSCLK_CONF_REG, sys);   // back on the PLL
  STAGE(41);
  s_div = div;
  return st;
}

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static bool turbo_pll_do(uint8_t div, bool dry, uint32_t *us_out, uint8_t mode, uint8_t ref)
{
  s_mode = mode;
  s_ref = ref;
  if (div < 4 || div > 40)
    return false;
  if (!dry)
    s_pll_try = div;   //survives a crash reset, so the next boot can report it
  STAGE(10);
  if (!s_mspi_low && !(mode & 4))
  {
    spi_timing_change_speed_mode_cache_safe(true);
    STAGE(11);
    s_mspi_low = true;
  }
  const spi_flash_guard_funcs_t *g = spi_flash_guard_get();
  uint32_t cyc = 0, fb = 0;
  uint32_t st;
  if (mode & 2)
  {
    STAGE(20);
    g->start();
    STAGE(21);
    st = pll_switch(div, dry, &cyc, &fb);
    STAGE(50);
    g->end();
    STAGE(51);
  }
  else
  {
    // no cooperative guard: interrupts off here, the other core stalled in hardware
    const int other = xPortGetCoreID() ^ 1;
    STAGE(22);
    portENTER_CRITICAL(&s_mux);
    esp_cpu_stall(other);
    STAGE(23);
    st = pll_switch(div, dry, &cyc, &fb);
    STAGE(52);
    esp_cpu_unstall(other);
    portEXIT_CRITICAL(&s_mux);
    STAGE(53);
  }
  (void)g;
  s_last_cal = st & 1; s_last_lock = (st >> 1) & 1; s_last_fallback = fb;
  if (s_div == 8 && fb != 2 && s_mspi_low)
  {
    spi_timing_change_speed_mode_cache_safe(false);
    s_mspi_low = false;
  }
  STAGE(60);
  if (us_out)
    *us_out = cyc / 40;
  if (s_div == 8)
    s_pll_try = 0;
  return st == 3;
}

static void zone_park_slow();
//mode bit 64 (zoned, for 320 MHz): core voltage first (vdig, clamped to boot..30), CPU onto PLL/3 so the
//switch lands there, VCO band forced above 600 MHz (it won't calibrate there), then slow zones on: the
//miners run at PLL/2, everything that formats/parses/does double math at PLL/3. Found 2026-11-14: at
//320 the CPU passes every kernel alone; it fails only with both cores on SRAM at once, and V29 fixes that.
bool turbo_pll_set(uint8_t div, uint32_t *us_out, uint8_t mode, uint8_t ref, uint8_t vdig)
{
  if (mode & 64)
  {
    if (vdig > s_boot_vdig && vdig <= 30) turbo_vdig_set(vdig);
    zone_park_slow();
    s_nknob = 0;
    if (div >= 12)
    {
      turbo_knob(1, 4, 4, 1);
      turbo_knob(1, 3, 0, 0);
    }
    if (div >= 13) turbo_knob(6, 2, 0, 0);   // DCUR 0: the only setting that locks 680
  }
  const bool ok = turbo_pll_do(div, false, us_out, mode, ref);
  if ((mode & 64) && s_div != 8) turbo_zone_set(true);
  if (s_div == 8) turbo_vdig_set(s_boot_vdig);   // stock clock: stock voltage
  return ok;
}
bool turbo_pll_dry(uint8_t div, uint32_t *us_out, uint8_t mode, uint8_t ref) { return turbo_pll_do(div, true, us_out, mode, ref); }
uint8_t turbo_pll_ref() { return s_cur_ref; }
uint32_t turbo_pll_mhz() { return 40u * (s_div + 4) / (s_cur_ref + 1) / 2; }
uint8_t turbo_vdig_boot() { return s_boot_vdig; }
uint8_t turbo_bb_reg(uint8_t r) { return rom_i2c_readReg(I2C_BBPLL, I2C_BBPLL_HOSTID, r); }
uint8_t turbo_bb_boot(uint8_t r) { return r < 16 ? s_boot_bb[r] : 0; }
//K with reg 0xff clears; the knobs apply on the next switch away from div 8 (regs 2, 4-6 and 9 only)
int turbo_knob(uint8_t reg, uint8_t msb, uint8_t lsb, uint8_t val)
{
  if (reg == 0xff) { s_nknob = 0; return 0; }
  if (!(reg == 1 || reg == 2 || (reg >= 4 && reg <= 6) || reg == 9) || msb > 7 || lsb > msb || s_nknob >= 6) return -1;
  s_knob[s_nknob] = {reg, msb, lsb, val};
  return ++s_nknob;
}
uint8_t turbo_vdig_get() { return rom_i2c_readReg_Mask(I2C_DIG_REG, I2C_DIG_REG_HOSTID, 6, 4, 0); }
uint8_t turbo_vdig_reg(uint8_t r) { return rom_i2c_readReg(I2C_DIG_REG, I2C_DIG_REG_HOSTID, r); }
//Capped at 30 (1.25 V: IDF's own DIG_DBIAS_240M for chips without a calibrated value), never 31 (~1.34 V);
//returns the value now set
uint8_t turbo_vdig_set(uint8_t v)
{
  if (s_boot_vdig == 0xff) return turbo_vdig_get();
  uint8_t cap = s_boot_vdig > 30 ? s_boot_vdig : 30;
  if (v > cap) v = cap;
  rom_i2c_writeReg_Mask(I2C_DIG_REG, I2C_DIG_REG_HOSTID, 6, 4, 0, v);
  return turbo_vdig_get();
}

//I: read-modify-write one SYSTEM clock register (DR_REG_SYSTEM_BASE block only) with interrupts
//off and the other core stalled, for clock-tree experiments (CPUPERIOD_SEL, PLL_FREQ_SEL, ...)
static void IRAM_ATTR poke_inner(uint32_t addr, uint32_t mask, uint32_t val, uint32_t *before)
{
  const uint32_t b = REG_READ(addr);
  *before = b;
  REG_WRITE(addr, (b & ~mask) | (val & mask));
  spin(20000);
}
uint32_t turbo_sysreg_poke(uint32_t addr, uint32_t mask, uint32_t val, uint32_t *before)
{
  *before = 0xFFFFFFFF;
  if (addr < DR_REG_SYSTEM_BASE || addr >= DR_REG_SYSTEM_BASE + 0x1000 || (addr & 3)) return 0xFFFFFFFF;
  if (mask == 0) { *before = REG_READ(addr); return *before; }
  const int other = xPortGetCoreID() ^ 1;
  portENTER_CRITICAL(&s_mux);
  esp_cpu_stall(other);
  poke_inner(addr, mask, val, before);
  esp_cpu_unstall(other);
  portEXIT_CRITICAL(&s_mux);
  return REG_READ(addr);
}

//Slow zones: at 320 MHz the CPU fails in double-precision soft-float (ROM libgcc via _dtoa_r,
//the same spot as the ~daily 300 MHz crash) long before the SHA loops do. With zones on, any
//code that formats, parses or does double math runs with the CPU at PLL/3 and the miners run at
//PLL/2 the rest of the time. One depth counter for both cores (they share the CPU clock).
static portMUX_TYPE s_zmux = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR volatile uint8_t s_zone_on = 0;
static DRAM_ATTR volatile int32_t s_zone_depth = 0;
static DRAM_ATTR volatile uint32_t s_zone_n = 0;
#define ZONE_FAST 2   // CPUPERIOD_SEL: PLL/2
#define ZONE_SLOW 1   // PLL/3
static inline void IRAM_ATTR zone_write(uint32_t sel)
{
  const int other = xPortGetCoreID() ^ 1;
  esp_cpu_stall(other);
  const uint32_t per = REG_READ(SYSTEM_CPU_PER_CONF_REG);
  REG_WRITE(SYSTEM_CPU_PER_CONF_REG, (per & ~SYSTEM_CPUPERIOD_SEL) | sel);
  spin(40);
  esp_cpu_unstall(other);
  s_zone_n++;
}
void IRAM_ATTR turbo_zone_enter()
{
  portENTER_CRITICAL_SAFE(&s_zmux);
  if (s_zone_depth++ == 0 && s_zone_on) zone_write(ZONE_SLOW);
  portEXIT_CRITICAL_SAFE(&s_zmux);
}
void IRAM_ATTR turbo_zone_exit()
{
  portENTER_CRITICAL_SAFE(&s_zmux);
  if (--s_zone_depth == 0 && s_zone_on) zone_write(ZONE_FAST);
  if (s_zone_depth < 0) s_zone_depth = 0;
  portEXIT_CRITICAL_SAFE(&s_zmux);
}
//on: zones switch the clock from now on (the caller is inside a zone, so it goes slow now and fast at
//the exit). off: CPU back to PLL/2 for good.
void turbo_zone_set(bool on)
{
  portENTER_CRITICAL(&s_zmux);
  s_zone_on = on;
  zone_write(on && s_zone_depth > 0 ? ZONE_SLOW : ZONE_FAST);
  portEXIT_CRITICAL(&s_zmux);
}
static void zone_park_slow()
{
  portENTER_CRITICAL(&s_zmux);
  zone_write(ZONE_SLOW);
  portEXIT_CRITICAL(&s_zmux);
}
void turbo_zone_info(uint32_t *on, uint32_t *n, int32_t *depth) { *on = s_zone_on; *n = s_zone_n; *depth = s_zone_depth; }
