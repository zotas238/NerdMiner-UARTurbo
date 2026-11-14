#include "displayDriver.h"

#ifdef T_DISPLAY

#include <TFT_eSPI.h>
#include "media/images_320_170.h"
#include "media/myFonts.h"
#include "media/Free_Fonts.h"
#include "version.h"
#include "monitor.h"
#include "OpenFontRender.h"
#include "rotation.h"
#include "utils.h"
#include <WiFi.h>
#include <esp_timer.h>

#define WIDTH 340
#define HEIGHT 170

TFT_eSPI tft = TFT_eSPI();                  // Invoke library, pins defined in User_Setup.h
#ifndef NM_TURBO
OpenFontRender render;
TFT_eSprite background = TFT_eSprite(&tft); // Invoke library sprite
#endif

#ifdef NM_TURBO
//Turbo: plain TFT_eSPI text redrawn every few seconds instead of the image + FreeType
//screens, which took a frame copy from flash and font rendering every second
extern uint32_t Mhashes;
extern uint32_t hashes;
#include "mining.h"
#include "stratum.h"

//Redraw period in seconds (0 = screen frozen), settable over the wire (W <s>). Each redraw
//takes CPU time on core 1 from the software miner, so keep it rare.
volatile uint32_t g_redraw_s = 12;
//Last and longest redraw in microseconds, and the number of redraws, for the status line
volatile uint32_t g_draw_us = 0, g_draw_us_max = 0, g_draws = 0;
static SemaphoreHandle_t s_tft_lock = NULL;

void tDisplay_Init(void)
{
#ifdef PIN_ENABLE5V
  pinMode(PIN_ENABLE5V, OUTPUT);
  digitalWrite(PIN_ENABLE5V, HIGH);
#endif
  tft.init();
  tft.setRotation(ROTATION_90);
  tft.setSwapBytes(true);
  s_tft_lock = xSemaphoreCreateMutex();
}

//Screenshot: read the frame back from the controller and hand it out raw, 3 bytes per pixel
//as the ST7789 sends them (decoded by wired/tools/screenshot.py). Returns the byte count.
uint32_t turbo_screen_dump(void (*emit)(uint32_t off, const uint8_t *b, size_t n))
{
  if (!s_tft_lock)
    return 0;
  xSemaphoreTake(s_tft_lock, portMAX_DELAY);
  const uint32_t w = tft.width(), h = tft.height(), total = w * h * 3;
  static uint8_t buf[480];
  tft.readRawBegin(0, 0, w, h);
  for (uint32_t off = 0; off < total; off += sizeof(buf))
  {
    const uint32_t n = total - off < sizeof(buf) ? total - off : sizeof(buf);
    for (uint32_t i = 0; i < n; ++i)
      buf[i] = tft.readRawByte();
    emit(off, buf, n);
  }
  tft.readRawEnd();
  xSemaphoreGive(s_tft_lock);
  return total;
}

void tDisplay_MinerScreen(unsigned long mElapsed)
{
  static uint32_t calls = 0;
  static uint32_t last_ms = 0;
  static uint64_t last_hashes = 0;
  uint32_t now = millis();
  if (calls > 0 && (g_redraw_s == 0 || now - last_ms < g_redraw_s * 1000))
    return;
  calls++;

  uint64_t total = (uint64_t)Mhashes * 1000000ULL + hashes;
  float khs = 0;
  if (last_ms && now > last_ms && total >= last_hashes)
    khs = (float)(total - last_hashes) / (float)(now - last_ms);
  last_ms = now;
  last_hashes = total;

  if (s_tft_lock)
    xSemaphoreTake(s_tft_lock, portMAX_DELAY);
  const int64_t t0 = esp_timer_get_time();
  if (calls == 1)
    tft.fillScreen(TFT_BLACK);

  char line[48];
  tft.setTextColor(TFT_ORANGE, TFT_BLACK);
  tft.setTextPadding(0);
  tft.drawString("NerdMiner UARTurbo", 8, 6, 2);
  //signal strength only means something in Wi-Fi mode
  tft.setTextPadding(110);
  if (g_wired)
    line[0] = 0;
  else
    snprintf(line, sizeof(line), "%d dBm", (int)WiFi.RSSI());
  tft.setTextDatum(TR_DATUM);
  tft.drawString(line, 312, 6, 2);
  tft.setTextDatum(TL_DATUM);

  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  snprintf(line, sizeof(line), "%.1f", khs);
  tft.setTextPadding(200);
  tft.drawString(line, 8, 30, 7);
  tft.setTextPadding(0);
  tft.drawString("kH/s", 214, 56, 4);

  char ever[16] = {0}, session[16] = {0};
  suffix_string(best_ever, ever, sizeof(ever), 0);
  suffix_string(best_session, session, sizeof(session), 0);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.setTextPadding(312);
  snprintf(line, sizeof(line), "Best ever  %s", ever);
  tft.drawString(line, 8, 88, 4);
  snprintf(line, sizeof(line), "Best session  %s", session);
  tft.drawString(line, 8, 114, 4);
  tft.setTextPadding(150);
  snprintf(line, sizeof(line), "Temp  %.0f C", temperatureRead());
  tft.drawString(line, 8, 140, 4);
  //block height, right-aligned on the bottom line; drawn only when it changes (~every 10 min)
  static uint32_t shown_height = 0;
  const uint32_t bh = g_block_height;
  if (bh && bh != shown_height)
  {
    shown_height = bh;
    snprintf(line, sizeof(line), "Block %lu", (unsigned long)bh);
    tft.setTextPadding(150);
    tft.setTextDatum(TR_DATUM);
    tft.drawString(line, 312, 140, 4);
    tft.setTextDatum(TL_DATUM);
  }

  const uint32_t us = (uint32_t)(esp_timer_get_time() - t0);
  if (s_tft_lock)
    xSemaphoreGive(s_tft_lock);
  g_draw_us = us;
  if (us > g_draw_us_max) g_draw_us_max = us;
  g_draws++;
}

#else

void tDisplay_Init(void)
{
      //Init pin 15 to eneble 5V external power (LilyGo bug)
#ifdef PIN_ENABLE5V
    pinMode(PIN_ENABLE5V, OUTPUT);
    digitalWrite(PIN_ENABLE5V, HIGH);
#endif
  
  tft.init();
  #ifdef LILYGO_S3_T_EMBED
  tft.setRotation(ROTATION_270);
  #else
  tft.setRotation(ROTATION_90);
  #endif
  tft.setSwapBytes(true);                 // Swap the colour byte order when rendering
  background.createSprite(WIDTH, HEIGHT); // Background Sprite
  background.setSwapBytes(true);
  render.setDrawer(background);  // Link drawing object to background instance (so font will be rendered on background)
  render.setLineSpaceRatio(0.9); // Espaciado entre texto

  // Load the font and check it can be read OK
  // if (render.loadFont(NotoSans_Bold, sizeof(NotoSans_Bold))) {
  if (render.loadFont(DigitalNumbers, sizeof(DigitalNumbers)))
  {
    Serial.println("Initialise error");
    return;
  }
}

#endif //NM_TURBO

void tDisplay_AlternateScreenState(void)
{
  int screen_state = digitalRead(TFT_BL);
  Serial.println("Switching display state");
  digitalWrite(TFT_BL, !screen_state);
}

void tDisplay_AlternateRotation(void)
{
  tft.setRotation( flipRotation(tft.getRotation()) );
}

#ifndef NM_TURBO
void tDisplay_MinerScreen(unsigned long mElapsed)
{
  mining_data data = getMiningData(mElapsed);

  // Print background screen
  background.pushImage(0, 0, MinerWidth, MinerHeight, MinerScreen);

  Serial.printf(">>> Completed %s share(s), %s Khashes, avg. hashrate %s KH/s\n",
                data.completedShares.c_str(), data.totalKHashes.c_str(), data.currentHashRate.c_str());

  // Hashrate
  render.setFontSize(35);
  render.setCursor(19, 118);
  render.setFontColor(TFT_BLACK);

  render.rdrawString(data.currentHashRate.c_str(), 118, 114, TFT_BLACK);
  // Total hashes
  render.setFontSize(18);
  render.rdrawString(data.totalMHashes.c_str(), 268, 138, TFT_BLACK);
  // Block templates
  render.setFontSize(18);
  render.drawString(data.templates.c_str(), 186, 20, 0xDEDB);
  // Best diff
  render.drawString(data.bestDiff.c_str(), 186, 48, 0xDEDB);
  // 32Bit shares
  render.setFontSize(18);
  render.drawString(data.completedShares.c_str(), 186, 76, 0xDEDB);
  // Hores
  render.setFontSize(14);
  render.rdrawString(data.timeMining.c_str(), 315, 104, 0xDEDB);

  // Valid Blocks
  render.setFontSize(24);
  render.drawString(data.valids.c_str(), 285, 56, 0xDEDB);

  // Print Temp
  render.setFontSize(10);
  render.rdrawString(data.temp.c_str(), 239, 1, TFT_BLACK);

  render.setFontSize(4);
  render.rdrawString(String(0).c_str(), 244, 3, TFT_BLACK);

  // Print Hour
  render.setFontSize(10);
  render.rdrawString(data.currentTime.c_str(), 286, 1, TFT_BLACK);

  // Push prepared background to screen
  background.pushSprite(0, 0);
}

void tDisplay_ClockScreen(unsigned long mElapsed)
{
  clock_data data = getClockData(mElapsed);

  // Print background screen
  background.pushImage(0, 0, minerClockWidth, minerClockHeight, minerClockScreen);

  Serial.printf(">>> Completed %s share(s), %s Khashes, avg. hashrate %s KH/s\n",
                data.completedShares.c_str(), data.totalKHashes.c_str(), data.currentHashRate.c_str());

  // Hashrate
  render.setFontSize(25);
  render.setCursor(19, 122);
  render.setFontColor(TFT_BLACK);
  render.rdrawString(data.currentHashRate.c_str(), 94, 129, TFT_BLACK);

  // Print BTC Price
  background.setFreeFont(FSSB9);
  background.setTextSize(1);
  background.setTextDatum(TL_DATUM);
  background.setTextColor(TFT_BLACK);
  background.drawString(data.btcPrice.c_str(), 202, 3, GFXFF);

  // Print BlockHeight
  render.setFontSize(18);
  render.rdrawString(data.blockHeight.c_str(), 254, 140, TFT_BLACK);

  // Print Hour
  background.setFreeFont(FF23);
  background.setTextSize(2);
  background.setTextColor(0xDEDB, TFT_BLACK);

  background.drawString(data.currentTime.c_str(), 130, 50, GFXFF);

  // Push prepared background to screen
  background.pushSprite(0, 0);
}

void tDisplay_GlobalHashScreen(unsigned long mElapsed)
{
  coin_data data = getCoinData(mElapsed);

  // Print background screen
  background.pushImage(0, 0, globalHashWidth, globalHashHeight, globalHashScreen);

  Serial.printf(">>> Completed %s share(s), %s Khashes, avg. hashrate %s KH/s\n",
                data.completedShares.c_str(), data.totalKHashes.c_str(), data.currentHashRate.c_str());

  // Print BTC Price
  background.setFreeFont(FSSB9);
  background.setTextSize(1);
  background.setTextDatum(TL_DATUM);
  background.setTextColor(TFT_BLACK);
  background.drawString(data.btcPrice.c_str(), 198, 3, GFXFF);

  // Print Hour
  background.setFreeFont(FSSB9);
  background.setTextSize(1);
  background.setTextDatum(TL_DATUM);
  background.setTextColor(TFT_BLACK);
  background.drawString(data.currentTime.c_str(), 268, 3, GFXFF);

  // Print Last Pool Block
  background.setFreeFont(FSS9);
  background.setTextDatum(TR_DATUM);
  background.setTextColor(0x9C92);
  background.drawString(data.halfHourFee.c_str(), 302, 52, GFXFF);

  // Print Difficulty
  background.setFreeFont(FSS9);
  background.setTextDatum(TR_DATUM);
  background.setTextColor(0x9C92);
  background.drawString(data.netwrokDifficulty.c_str(), 302, 88, GFXFF);

  // Print Global Hashrate
  render.setFontSize(17);
  render.rdrawString(data.globalHashRate.c_str(), 274, 145, TFT_BLACK);

  // Print BlockHeight
  render.setFontSize(28);
  render.rdrawString(data.blockHeight.c_str(), 140, 104, 0xDEDB);

  // Draw percentage rectangle
  int x2 = 2 + (138 * data.progressPercent / 100);
  background.fillRect(2, 149, x2, 168, 0xDEDB);

  // Print Remaining BLocks
  background.setTextFont(FONT2);
  background.setTextSize(1);
  background.setTextDatum(MC_DATUM);
  background.setTextColor(TFT_BLACK);
  background.drawString(data.remainingBlocks.c_str(), 72, 159, FONT2);

  // Push prepared background to screen
  background.pushSprite(0, 0);
}


void tDisplay_BTCprice(unsigned long mElapsed)
{
  clock_data data = getClockData(mElapsed);
  data.currentDate ="01/12/2023";
  
  //if(data.currentDate.indexOf("12/2023")>) { tDisplay_ChristmasContent(data); return; }

  // Print background screen
  background.pushImage(0, 0, priceScreenWidth, priceScreenHeight, priceScreen);

  Serial.printf(">>> Completed %s share(s), %s Khashes, avg. hashrate %s KH/s\n",
                data.completedShares.c_str(), data.totalKHashes.c_str(), data.currentHashRate.c_str());

  // Hashrate
  render.setFontSize(25);
  render.setCursor(19, 122);
  render.setFontColor(TFT_BLACK);
  render.rdrawString(data.currentHashRate.c_str(), 94, 129, TFT_BLACK);

  // Print BlockHeight
  render.setFontSize(18);
  render.rdrawString(data.blockHeight.c_str(), 254, 138, TFT_WHITE);

  // Print Hour
  
  background.setFreeFont(FSSB9);
  background.setTextSize(1);
  background.setTextDatum(TL_DATUM);
  background.setTextColor(TFT_BLACK);
  background.drawString(data.currentTime.c_str(), 222, 3, GFXFF);

  // Print BTC Price 
  background.setFreeFont(FF24);
  background.setTextDatum(TR_DATUM);
  background.setTextSize(1);
  background.setTextColor(0xDEDB, TFT_BLACK);
  background.drawString(data.btcPrice.c_str(), 300, 58, GFXFF);

  // Push prepared background to screen
  background.pushSprite(0, 0);
}

#endif //NM_TURBO

void tDisplay_LoadingScreen(void)
{
  tft.fillScreen(TFT_BLACK);
  tft.pushImage(0, 0, initWidth, initHeight, initScreen);
  tft.setTextColor(TFT_BLACK);
  tft.drawString(CURRENT_VERSION, 24, 147, FONT2);
}

void tDisplay_SetupScreen(void)
{
  tft.pushImage(0, 0, setupModeWidth, setupModeHeight, setupModeScreen);
}

void tDisplay_AnimateCurrentScreen(unsigned long frame)
{
}

void tDisplay_DoLedStuff(unsigned long frame)
{
}

#ifdef NM_TURBO
CyclicScreenFunction tDisplayCyclicScreens[] = {tDisplay_MinerScreen};
#else
CyclicScreenFunction tDisplayCyclicScreens[] = {tDisplay_MinerScreen, tDisplay_ClockScreen, tDisplay_GlobalHashScreen, tDisplay_BTCprice};
#endif

DisplayDriver tDisplayDriver = {
    tDisplay_Init,
    tDisplay_AlternateScreenState,
    tDisplay_AlternateRotation,
    tDisplay_LoadingScreen,
    tDisplay_SetupScreen,
    tDisplayCyclicScreens,
    tDisplay_AnimateCurrentScreen,
    tDisplay_DoLedStuff,
    SCREENS_ARRAY_SIZE(tDisplayCyclicScreens),
    0,
    WIDTH,
    HEIGHT};
#endif
