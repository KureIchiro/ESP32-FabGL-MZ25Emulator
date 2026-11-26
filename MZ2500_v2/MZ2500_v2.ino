/* ============================================================================
 *  SHARP MZ-2500 Emulator for ESP32 (Work in Progress)
 *  --- TTGO VGA32 / FabGL。画面は 640x400 16色(24kHz の MZ-2500 と同じ 55.5Hz) ---
 * ============================================================================
 *
 * 【クレジットと謝辞】
 *  - グラフィック/オーディオエンジン: Fabrizio Di Vittorio氏による FabGL
 *  - Z80 CPUコア: このシリーズのために書いた z80core(MIT ライセンス。待ちループの早送りは mz_spin.c)
 *  - 動きの手本: 武田俊也さんの Common Source Code Project(EmuZ-2500)、MAME mz2500.cpp
 *
 * 【必要なファイル（SDカード）】
 *  /MZ25/IPL.ROM   : IPL ROM (32KB)
 *  /MZ25/KANJI.ROM : 漢字 ROM (256KB。なくても動く: 漢字が出ないだけ)
 *  /MZ25/DICT.ROM  : 辞書 ROM (256KB。なくても動く)
 *  /MZ25/PHONE.ROM または FILE.ROM : ページ 3Ch〜3Fh の 32KB の ROM(なくても動く)
 *  /MZ25 の D88    : ディスクイメージ(/MZ25 の中のフォルダに置いてもよい。起動メニューで入れる)
 *
 * 【範囲】MZ-2500 モード(6MHz)。16 色・4 色・256 色の画面(256 色は 16 色の 2 色のタイルで出す)、
 *  4096 色のパレット(TTGO VGA32 は 64 色なので 64 色に丸まる)。カセットテープ・MZ-80B / MZ-2000 モードは入れない。
 *
 * 【操作】F12: 一時停止 / 再開(止めている間に F10 / F11 で FD1 / FD2 のディスクを入れ替え、Shift+F10 / F11 で
 *  SD へ書き戻し)。Shift+F12: リセット(ESP32 ごと)。F1〜F10 は MZ-2500 のファンクションキー(F10 は一時停止中だけ
 *  ディスクの入れ替え)、F11 = HELP。End = BREAK、PrintScreen = COPY、Home = CLR、Insert = INST、
 *  BackSpace / Delete = DEL、Alt = GRPH、CapsLock = LOCK、カタカナひらがな = カナ、無変換 / 変換 = KJ1 / KJ2、
 *  半角/全角 = ロゴのキー
 * ============================================================================
 */

#pragma GCC optimize("O3")

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <SPI.h>
#include <SD.h>
#if __has_include("emu_switch.h")
#include "emu_switch.h"
#define EMU_SWITCH 1
#else
#define EMU_SWITCH 0
#endif
#include <fabgl.h>
#include <vector>
#include <algorithm>
#include "soc/rtc_wdt.h"
#include "esp_task_wdt.h"
#ifndef BOARD_HAS_PSRAM
#include "esp32/spiram.h"
#include "esp_heap_caps.h"
#define ps_malloc(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#endif
#if defined(BOARD_HAS_PSRAM) && !defined(EMU_PSRAM_NO_ERRATA_FIX)
#define psramFixMissing() false
#else
#define psramFixMissing() (ESP.getChipRevision() < 3)
#endif
#include "z80core.h"
#include "mz.h"
#include "ym2203.h"

//#define USE_SERIAL_DEBUG

#define MZ_VGA_HZ 55
#if MZ_VGA_HZ == 55
#define MZ_VGA_VTOTAL 567
#elif MZ_VGA_HZ == 56
#define MZ_VGA_VTOTAL 562
#elif MZ_VGA_HZ == 57
#define MZ_VGA_VTOTAL 552
#elif MZ_VGA_HZ == 60
#define MZ_VGA_VTOTAL 524
#else
#error "MZ_VGA_HZ は 55 / 56 / 57 / 60 のどれか"
#endif
#define MZ_FRAME_US (800 * MZ_VGA_VTOTAL * 1000 / 25175)

#define MZ_DIR "/MZ25"
#define MZ_OPN_CLOCK 2000000

// TTGO VGA32
#define SD_SCLK 14
#define SD_MISO 2
#define SD_MOSI 12
#define SD_CS   13

// ORANGE ESPer
// #define SD_SCLK 14
// #define SD_MISO 35
// #define SD_MOSI 12
// #define SD_CS   13

// Switch Science FabGL
// #define SD_SCLK 14
// #define SD_MISO 35
// #define SD_MOSI 2
// #define SD_CS   13

struct D88Image { uint32_t ofs, size; char name[18]; };
static const int D88_MAX_IMAGES = 16;
struct MzKeyMap { uint8_t ps2; bool ext; uint8_t key; };

class MzVGA16 : public fabgl::VGA16Controller {
public:
  uint8_t **rowTable() { return (uint8_t **)m_viewPort; }
  static int scanLine() { return s_scanLine; }
};

MzVGA16 DisplayController;
fabgl::Canvas *canvas;
fabgl::SoundGenerator soundGenerator;
fabgl::PS2Controller PS2Controller;

volatile bool frameReady = false;
static volatile bool isPaused = false;
static volatile bool cpuStopped = false;
static volatile bool paletteForce = false;

static uint8_t shownPal[16][3];
static uint32_t shownPalGen = 0;
static void applyPalette() {
  uint8_t rgb[16][3];
  mzVideoPalette(rgb);
  for (int i = 0; i < 16; i++) {
    uint8_t c[3];
    for (int k = 0; k < 3; k++) c[k] = (uint8_t)(((rgb[i][k] * 3 + 127) / 255) * 85);
    DisplayController.setPaletteItem(i, fabgl::RGB888(c[0], c[1], c[2]));
    memcpy(shownPal[i], rgb[i], 3);
  }
  shownPalGen = mz_palette_gen;
}
static void menuPalette() {
  for (int i = 0; i < 16; i++) DisplayController.setPaletteItem(i, fabgl::RGB888((fabgl::Color)i));
}

static volatile uint8_t joyMsx[2] = { 0x3F, 0x3F };

static void updateJoystick(bool hold) {
  uint8_t j[2] = { 0x3F, 0x3F };
  if (hold) { j[0] = joyMsx[0]; j[1] = joyMsx[1]; }
  while (Serial1.available() > 0) {
    uint8_t b = Serial1.read();
    j[(b & 0x80) ? 1 : 0] = b & 0x3F;
  }
  joyMsx[0] = j[0];
  joyMsx[1] = j[1];
}

static int joyMenuPoll() {
  static uint8_t state = 0x7F, last = 0x7F;
  static uint32_t lastRx = 0, repeatAt = 0;
  uint32_t now = millis();
  while (Serial1.available() > 0) {
    uint8_t b = Serial1.read();
    if (!(b & 0x80)) { state = b & 0x7F; lastRx = now; }
  }
  if (now - lastRx > 100) state = 0x7F;
  uint8_t pressed = (uint8_t)(~state & 0x7F), prev = (uint8_t)(~last & 0x7F);
  uint8_t edge = pressed & ~prev;
  last = state;
  if (edge & 0x10) return 'A';
  if (edge & 0x20) return 'B';
  static const char dirs[4] = { 'U', 'D', 'L', 'R' };
  for (int i = 0; i < 4; i++) {
    if (!(pressed & (1 << i))) continue;
    if (edge & (1 << i)) { repeatAt = now + 400; return dirs[i]; }
    if ((int32_t)(now - repeatAt) >= 0) { repeatAt = now + 120; return dirs[i]; }
  }
  return 0;
}
static int joyMenuScancode(bool *e0) {
  *e0 = true;
  switch (joyMenuPoll()) {
    case 'U': return 0x75;
    case 'D': return 0x72;
    case 'L': return 0x7D;
    case 'R': return 0x7A;
    case 'A': *e0 = false; return 0x5A;
    case 'B': *e0 = false; return 0x76;
    default: return 0;
  }
}

#define MZK(r, b) ((uint8_t)(((r) << 3) | (b)))
static const char *const MZ_KEY_NAMES[MZ_KEY_ROWS][8] = {
  { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8" },
  { "F9", "F10", "NUM8", "NUM9", "NUM,", "NUM.", "NUM+", "NUM-" },
  { "NUM0", "NUM1", "NUM2", "NUM3", "NUM4", "NUM5", "NUM6", "NUM7" },
  { "TAB", "SPACE", "RETURN", "UP", "DOWN", "LEFT", "RIGHT", "BREAK" },
  { "/", "A", "B", "C", "D", "E", "F", "G" },
  { "H", "I", "J", "K", "L", "M", "N", "O" },
  { "P", "Q", "R", "S", "T", "U", "V", "W" },
  { "X", "Y", "Z", "^", "YEN", "_", ".", "," },
  { "0", "1", "2", "3", "4", "5", "6", "7" },
  { "8", "9", ":", ";", "-", "@", "[", "?" },
  { "]", "COPY", "CLR", "INST", "DEL", "ESC", "NUM*", "NUM/" },
  { "GRPH", "LOCK", "SHIFT", "KANA", "CTRL", "?", "?", "?" },
  { "KJ1", "KJ2", "?", "?", "?", "?", "?", "?" },
  { "LOGO", "HELP", "?", "?", "?", "?", "?", "?" },
};
static const char *mzKeyName(uint8_t k) { return (k >> 3) < MZ_KEY_ROWS ? MZ_KEY_NAMES[k >> 3][k & 7] : "?"; }

static const MzKeyMap keyMapTable[] = {
  { 0x05, false, MZK(0, 0) }, { 0x06, false, MZK(0, 1) }, { 0x04, false, MZK(0, 2) }, { 0x0C, false, MZK(0, 3) },
  { 0x03, false, MZK(0, 4) }, { 0x0B, false, MZK(0, 5) }, { 0x83, false, MZK(0, 6) }, { 0x0A, false, MZK(0, 7) },
  { 0x01, false, MZK(1, 0) }, { 0x09, false, MZK(1, 1) }, { 0x78, false, MZK(13, 1) },
  { 0x70, false, MZK(2, 0) }, { 0x69, false, MZK(2, 1) }, { 0x72, false, MZK(2, 2) }, { 0x7A, false, MZK(2, 3) },
  { 0x6B, false, MZK(2, 4) }, { 0x73, false, MZK(2, 5) }, { 0x74, false, MZK(2, 6) }, { 0x6C, false, MZK(2, 7) },
  { 0x75, false, MZK(1, 2) }, { 0x7D, false, MZK(1, 3) }, { 0x71, false, MZK(1, 5) }, { 0x79, false, MZK(1, 6) },
  { 0x7B, false, MZK(1, 7) }, { 0x7C, false, MZK(10, 6) }, { 0x4A, true, MZK(10, 7) }, { 0x5A, true, MZK(3, 2) },
  { 0x0D, false, MZK(3, 0) }, { 0x29, false, MZK(3, 1) }, { 0x5A, false, MZK(3, 2) },
  { 0x75, true, MZK(3, 3) }, { 0x72, true, MZK(3, 4) }, { 0x6B, true, MZK(3, 5) }, { 0x74, true, MZK(3, 6) },
  { 0x69, true, MZK(3, 7) },
  { 0x4A, false, MZK(4, 0) }, { 0x1C, false, MZK(4, 1) }, { 0x32, false, MZK(4, 2) }, { 0x21, false, MZK(4, 3) },
  { 0x23, false, MZK(4, 4) }, { 0x24, false, MZK(4, 5) }, { 0x2B, false, MZK(4, 6) }, { 0x34, false, MZK(4, 7) },
  { 0x33, false, MZK(5, 0) }, { 0x43, false, MZK(5, 1) }, { 0x3B, false, MZK(5, 2) }, { 0x42, false, MZK(5, 3) },
  { 0x4B, false, MZK(5, 4) }, { 0x3A, false, MZK(5, 5) }, { 0x31, false, MZK(5, 6) }, { 0x44, false, MZK(5, 7) },
  { 0x4D, false, MZK(6, 0) }, { 0x15, false, MZK(6, 1) }, { 0x2D, false, MZK(6, 2) }, { 0x1B, false, MZK(6, 3) },
  { 0x2C, false, MZK(6, 4) }, { 0x3C, false, MZK(6, 5) }, { 0x2A, false, MZK(6, 6) }, { 0x1D, false, MZK(6, 7) },
  { 0x22, false, MZK(7, 0) }, { 0x35, false, MZK(7, 1) }, { 0x1A, false, MZK(7, 2) },
  { 0x55, false, MZK(7, 3) }, { 0x6A, false, MZK(7, 4) }, { 0x51, false, MZK(7, 5) }, { 0x49, false, MZK(7, 6) },
  { 0x41, false, MZK(7, 7) },
  { 0x45, false, MZK(8, 0) }, { 0x16, false, MZK(8, 1) }, { 0x1E, false, MZK(8, 2) }, { 0x26, false, MZK(8, 3) },
  { 0x25, false, MZK(8, 4) }, { 0x2E, false, MZK(8, 5) }, { 0x36, false, MZK(8, 6) }, { 0x3D, false, MZK(8, 7) },
  { 0x3E, false, MZK(9, 0) }, { 0x46, false, MZK(9, 1) }, { 0x52, false, MZK(9, 2) }, { 0x4C, false, MZK(9, 3) },
  { 0x4E, false, MZK(9, 4) }, { 0x54, false, MZK(9, 5) }, { 0x5B, false, MZK(9, 6) }, { 0x5D, false, MZK(10, 0) },
  { 0x7C, true, MZK(10, 1) }, { 0x6C, true, MZK(10, 2) }, { 0x70, true, MZK(10, 3) }, { 0x66, false, MZK(10, 4) },
  { 0x71, true, MZK(10, 4) }, { 0x76, false, MZK(10, 5) },
  { 0x11, false, MZK(11, 0) }, { 0x11, true, MZK(11, 0) }, { 0x58, false, MZK(11, 1) }, { 0x12, false, MZK(11, 2) },
  { 0x59, false, MZK(11, 2) }, { 0x13, false, MZK(11, 3) }, { 0x14, false, MZK(11, 4) }, { 0x14, true, MZK(11, 4) },
  { 0x67, false, MZK(12, 0) }, { 0x64, false, MZK(12, 1) }, { 0x0E, false, MZK(13, 0) },
};
static const int keyMapCount = sizeof(keyMapTable) / sizeof(keyMapTable[0]);

static volatile uint8_t kbdMatrix[MZ_KEY_ROWS] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static bool joyToKeyOn = false;
static uint8_t joyButtonKey[3] = { MZK(3, 1), MZK(3, 2), MZK(10, 5) };
static const char *JOYKEY_FILE = MZ_DIR "/JOYKEY.CFG";

static void joyToKeys() {
  uint8_t joy[MZ_KEY_ROWS];
  memset(joy, 0xFF, sizeof(joy));
  if (joyToKeyOn) {
    static const uint8_t ten[10] = { MZK(2, 0), MZK(2, 1), MZK(2, 2), MZK(2, 3), MZK(2, 4),
                                     MZK(2, 5), MZK(2, 6), MZK(2, 7), MZK(1, 2), MZK(1, 3) };
    uint8_t m = joyMsx[0];
    bool up = !(m & 1), dn = !(m & 2), lt = !(m & 4), rt = !(m & 8);
    int n = -1;
    if (up) n = lt ? 7 : rt ? 9 : 8;
    else if (dn) n = lt ? 1 : rt ? 3 : 2;
    else if (lt) n = 4;
    else if (rt) n = 6;
    if (n >= 0) joy[ten[n] >> 3] &= (uint8_t)~(1 << (ten[n] & 7));
    for (int b = 0; b < 3; b++)
      if (!(m & (0x10 << b))) joy[joyButtonKey[b] >> 3] &= (uint8_t)~(1 << (joyButtonKey[b] & 7));
  }
  for (int r = 0; r < MZ_KEY_ROWS; r++) mz_key_matrix[r] = kbdMatrix[r] & joy[r];
  mz_joy[0] = joyToKeyOn ? 0x3F : joyMsx[0];
  mz_joy[1] = joyMsx[1];
}

static void loadJoyKeys() {
  File f = SD.open(JOYKEY_FILE, FILE_READ);
  if (!f) return;
  uint8_t b[4];
  if (f.read(b, 4) == 4) {
    joyToKeyOn = b[0] != 0;
    for (int i = 0; i < 3; i++) if ((b[i + 1] >> 3) < MZ_KEY_ROWS) joyButtonKey[i] = b[i + 1];
  }
  f.close();
}
static void saveJoyKeys() {
  File f = SD.open(JOYKEY_FILE, FILE_WRITE);
  if (!f) return;
  uint8_t b[4] = { (uint8_t)(joyToKeyOn ? 1 : 0), joyButtonKey[0], joyButtonKey[1], joyButtonKey[2] };
  f.write(b, 4);
  f.close();
}

static const int SND_BUFFER_SIZE = 512;
static const int OPN_QUEUE = 1024;
static volatile uint32_t *opnQueue = nullptr;
static volatile int opnQHead = 0, opnQTail = 0;
static volatile uint32_t opnEmuClock = 0;

static const uint8_t SPK_REG = 0xFF;
static void soundQueuePush(uint8_t reg, uint8_t val) {
  int next = (opnQHead + 1) % OPN_QUEUE;
  if (next == opnQTail || !opnQueue) return;
  uint32_t t = opnEmuClock + (uint32_t)mzLineCycle();
  opnQueue[opnQHead] = ((t >> 5) << 16) | ((uint32_t)reg << 8) | val;
  opnQHead = next;
}
void mzOpnWriteHook(uint8_t reg, uint8_t val) {
  if (reg < 0x10 || (reg >= 0x28 && reg <= 0xB6)) soundQueuePush(reg, val);
}
void mzBeepHook(bool on) { soundQueuePush(SPK_REG, on ? 0x80 : 0x00); }

class OpnGenerator : public fabgl::WaveformGenerator {
  Ym2203Fm *fm = nullptr;
  uint32_t psgClock = MZ_OPN_CLOCK / 2 / 16;
  int sndRate = 16384;
  uint32_t tickAccum = 0, tickStep = 0;
  uint8_t  regs[16] = { 0 };
  uint32_t tonePeriod[3] = { 1, 1, 1 }, noisePeriod = 1, envPeriod = 1;
  bool     holding = true, cont = false, alt = false, hold = false, att = false;
  uint32_t envCounter = 0, envStep = 0, envVol = 0;
  uint32_t toneCounter[3] = { 0, 0, 0 };
  uint32_t toneOut = 0, noiseCounter = 0, lfsr = 1, noiseOut = 0;
  const int16_t volTable[16] = { 0, 1, 1, 2, 3, 4, 6, 8, 11, 16, 22, 32, 45, 64, 90, 127 };
  int32_t  dcPrev = 0, dcOut = 0;
  #define FM_TREBLE_FIX 320
  int32_t  fmLp = 0;
  uint8_t  spk = 0;
  bool     singPend = false;
  int32_t  singArea = 0, singRem = 0;
  static const int SPK_AMP = 60;

  void spkWrite(uint8_t v, int32_t w) {
    bool oldOn = spk & 0x80, newOn = v & 0x80;
    spk = v;
    if (oldOn == newOn) return;
    if (!singPend) { singArea = oldOn ? 256 - w : 0; singRem = w; singPend = true; }
    else {
      if (w > singRem) w = singRem;
      if (oldOn) singArea += singRem - w;
      singRem = w;
    }
  }
  int16_t  buf[SND_BUFFER_SIZE];
  volatile int head = 0, tail = 0;
  int16_t  lastSample = 0;
  int32_t  fmBuf[64];

  void ssgWrite(uint8_t reg, uint8_t val) {
    reg &= 15;
    regs[reg] = val;
    switch (reg) {
      case 0: case 1: { uint32_t p = regs[0] | ((regs[1] & 0x0F) << 8); tonePeriod[0] = p ? p : 1; break; }
      case 2: case 3: { uint32_t p = regs[2] | ((regs[3] & 0x0F) << 8); tonePeriod[1] = p ? p : 1; break; }
      case 4: case 5: { uint32_t p = regs[4] | ((regs[5] & 0x0F) << 8); tonePeriod[2] = p ? p : 1; break; }
      case 6: noisePeriod = (regs[6] & 0x1F) ? (regs[6] & 0x1F) : 1; break;
      case 11: case 12: { uint32_t p = regs[11] | (regs[12] << 8); envPeriod = p ? p : 1; break; }
      case 13:
        envStep = 0; envCounter = 0;
        cont = val & 0x08; att = val & 0x04; alt = val & 0x02; hold = val & 0x01;
        envVol = att ? 0 : 15;
        holding = false;
        break;
    }
  }

  inline void stepEnvelope() {
    if (envStep < 15) {
      envStep++;
      envVol = att ? envStep : (15 - envStep);
    } else if (!cont) {
      holding = true; envVol = 0;
    } else if (hold) {
      holding = true;
      if (alt) att = !att;
      envVol = att ? 15 : 0;
    } else {
      envStep = 0;
      if (alt) att = !att;
      envVol = att ? 0 : 15;
    }
  }

  int ssgSample() {
    uint32_t r7 = regs[7], r8 = regs[8], r9 = regs[9], r10 = regs[10];
    uint32_t tp0 = tonePeriod[0], tp1 = tonePeriod[1], tp2 = tonePeriod[2], np = noisePeriod;
    tickAccum += tickStep;
    uint32_t ticks = tickAccum >> 16;
    tickAccum &= 0xFFFF;
    while (ticks--) {
      toneCounter[0] += 2; while (toneCounter[0] >= tp0) { toneCounter[0] -= tp0; toneOut ^= 1; }
      toneCounter[1] += 2; while (toneCounter[1] >= tp1) { toneCounter[1] -= tp1; toneOut ^= 2; }
      toneCounter[2] += 2; while (toneCounter[2] >= tp2) { toneCounter[2] -= tp2; toneOut ^= 4; }
      if (++noiseCounter >= np) {
        noiseCounter = 0;
        lfsr = (lfsr >> 1) | (((lfsr & 1) ^ ((lfsr >> 3) & 1)) << 16);
        noiseOut = lfsr & 1;
      }
      if (!holding && ++envCounter >= envPeriod) { envCounter = 0; stepEnvelope(); }
    }
    uint32_t mix = (toneOut | r7) & ((noiseOut ? 0x07 : 0) | (r7 >> 3));
    int volA = volTable[(r8 & 0x10) ? envVol : (r8 & 0x0F)];
    int volB = volTable[(r9 & 0x10) ? envVol : (r9 & 0x0F)];
    int volC = volTable[(r10 & 0x10) ? envVol : (r10 & 0x0F)];
    int s = 0;
    if (mix & 1) s += volA;
    if (mix & 2) s += volB;
    if (mix & 4) s += volC;
    return s;
  }

public:
  void begin(int rate) {
    opnQueue = (volatile uint32_t *)heap_caps_malloc(sizeof(uint32_t) * OPN_QUEUE, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (!opnQueue) opnQueue = (volatile uint32_t *)ps_malloc(sizeof(uint32_t) * OPN_QUEUE);
    fm = ym2203Create(MZ_OPN_CLOCK, rate);
    setSampleRate(rate);
  }
  void setSsgDivider(uint8_t reg) {
    static const uint8_t div[3] = { 4, 2, 1 };
    psgClock = MZ_OPN_CLOCK / 8 / div[reg - 0x2D];
    tickStep = (uint32_t)(((uint64_t)psgClock << 16) / sndRate);
  }
  void setSampleRate(int rate) {
    this->fabgl::WaveformGenerator::setSampleRate(rate);
    sndRate = rate > 0 ? rate : 16384;
    tickStep = (uint32_t)(((uint64_t)psgClock << 16) / (rate > 0 ? rate : 16384));
    clkPerSampleQ16 = (uint32_t)((((uint64_t)MZ_CYC_FRAME * 1000000u) << 16) / ((uint64_t)MZ_FRAME_US * (rate > 0 ? rate : 16384)));
  }

  void applyQueued(int32_t w = 256) {
    uint32_t e = opnQueue[opnQTail];
    opnQTail = (opnQTail + 1) % OPN_QUEUE;
    uint8_t r = (e >> 8) & 0xFF, v = e & 0xFF;
    if (r == SPK_REG) spkWrite(v, w);
    else if (r < 0x10) ssgWrite(r, v);
    else {
      if (r >= 0x2D && r <= 0x2F) setSsgDivider(r);
      ym2203WriteReg(fm, r, v);
    }
  }

  uint32_t sndClock = 0, sndFrac = 0, clkPerSampleQ16 = 325u << 16;
  void advanceClock(int n, uint32_t emu) {
    uint64_t f = sndFrac + (uint64_t)n * clkPerSampleQ16;
    sndClock += (uint32_t)(f >> 16);
    sndFrac = (uint32_t)(f & 0xFFFF);
    if ((int32_t)(sndClock - emu) > 0) { sndClock = emu; sndFrac = 0; }
  }

  void fillBuffer() {
    if (!fm) return;
    if (isPaused) {
      int space = (tail - head - 1 + SND_BUFFER_SIZE) % SND_BUFFER_SIZE;
      while (space-- > 0) { buf[head] = 0; head = (head + 1) % SND_BUFFER_SIZE; }
      return;
    }
    const uint32_t frameClk = MZ_CYC_FRAME;
    uint32_t emu = opnEmuClock;
    if ((int32_t)(emu - sndClock) > (int32_t)(frameClk + frameClk / 4)) {
      while (opnQTail != opnQHead) applyQueued();
      sndClock = emu - frameClk; sndFrac = 0;
    }
    int space = (tail - head - 1 + SND_BUFFER_SIZE) % SND_BUFFER_SIZE;
    while (space > 0) {
      int n = space > 64 ? 64 : space;
      while (opnQTail != opnQHead) {
        uint32_t t16 = opnQueue[opnQTail] >> 16;
        int32_t d = (int16_t)(t16 - (sndClock >> 5));
        if (d > 0) {
          if ((int16_t)(t16 - (emu >> 5)) <= 0) {
            int m = (int)((((uint64_t)d << 21) + clkPerSampleQ16 - 1) / clkPerSampleQ16);
            if (m < n) n = m;
          }
          break;
        }
        int32_t w = (int32_t)(((uint64_t)(-d) << 29) / clkPerSampleQ16);
        applyQueued(w > 256 ? 256 : w);
      }
      advanceClock(n, emu);
      ym2203Generate(fm, fmBuf, n);
      for (int i = 0; i < n; i++) {
#if FM_TREBLE_FIX
        fmLp += ((fmBuf[i] - fmLp) * FM_TREBLE_FIX) >> 8;
#else
        fmLp = fmBuf[i];
#endif
        int32_t x = ((fmLp >> 7) + ((ssgSample() * 68) >> 8)) << 8;
        if (spk | singPend) {
          int32_t lv;
          if (singPend) { lv = singArea + ((spk & 0x80) ? singRem : 0); singPend = false; }
          else lv = (spk & 0x80) ? 256 : 0;
          x += lv * SPK_AMP;
        }
        dcOut = x - dcPrev + dcOut - (dcOut >> 8);
        dcPrev = x;
        int32_t s = dcOut >> 8;
        if (s > 96) s = 96 + (s - 96) / 3; else if (s < -96) s = -96 + (s + 96) / 3;
        if (s > 127) s = 127; else if (s < -128) s = -128;
        buf[head] = (int16_t)s;
        head = (head + 1) % SND_BUFFER_SIZE;
      }
      space -= n;
    }
  }
  int getSample() override {
    if (head == tail) return lastSample;
    lastSample = buf[tail];
    tail = (tail + 1) % SND_BUFFER_SIZE;
    return lastSample;
  }
  void setFrequency(int value) override {}
};
OpnGenerator opnGen;

#define NUM_DRIVES 2
static MzDrive drives[MZ_NUM_DRIVES];
static String drivePath[NUM_DRIVES];
static uint32_t driveOfs[NUM_DRIVES];

static void flashStatus(const char *msg) { Serial.println(msg); }
static void showStatus(const char *msg);
static void writeBackD88(int d);
static void swapDisk(int d);
static const char *PAUSE_MSG = "PAUSED  F12:RESUME  F10/F11:FD1/FD2  SHIFT+F10/F11:WRITE BACK  SHIFT+F12:RESET";

static uint8_t kbdKept[32];
static int kbdKeptHead = 0, kbdKeptCount = 0;
static bool kbdLedByte(uint8_t b) {
  fabgl::PS2Controller::sendData(b, 0);
  for (uint32_t t0 = millis(); millis() - t0 < 500; ) {
    int r = fabgl::PS2Controller::getData(0, 10);
    if (r == 0xFA) return true;
    if (r >= 0 && r != 0xFE && kbdKeptCount < (int)sizeof(kbdKept)) kbdKept[(kbdKeptHead + kbdKeptCount++) % sizeof(kbdKept)] = (uint8_t)r;
  }
  return false;
}
static void kbdSetLeds(bool capsLock, bool scrollLock) {
  fabgl::PS2Controller::lock(0, -1);
  {
    fabgl::PS2PortAutoDisableRX mouseOff(1);
    if (kbdLedByte(0xED)) kbdLedByte((scrollLock ? 1 : 0) | (capsLock ? 4 : 0));
  }
  fabgl::PS2Controller::unlock(0);
}
static int kbdAvailable(fabgl::Keyboard *kb) { return kbdKeptCount + kb->scancodeAvailable(); }
static int kbdNextScancode(fabgl::Keyboard *kb) {
  if (kbdKeptCount > 0) {
    uint8_t b = kbdKept[kbdKeptHead];
    kbdKeptHead = (kbdKeptHead + 1) % (int)sizeof(kbdKept);
    kbdKeptCount--;
    return b;
  }
  return kb->getNextScancode(0);
}

static uint8_t hostDown[64];
static void hostKeySet(uint8_t ps2, bool ext, bool down) {
  const int h = (ext ? 256 : 0) | ps2;
  if (down) hostDown[h >> 3] |= (uint8_t)(1 << (h & 7));
  else      hostDown[h >> 3] &= (uint8_t)~(1 << (h & 7));
}
static bool hostKeyAny(uint8_t key) {
  for (int i = 0; i < keyMapCount; i++) {
    const int h = (keyMapTable[i].ext ? 256 : 0) | keyMapTable[i].ps2;
    if (keyMapTable[i].key == key && ((hostDown[h >> 3] >> (h & 7)) & 1)) return true;
  }
  return false;
}
static void updateKeyboard() {
  static bool isBreak = false, isE0 = false, isShift = false;
  static bool f12_held = false, f10_held = false, f11_held = false;
  static bool ledLock = false, ledKana = false, lockHeld = false, kanaHeld = false;
  bool ledChanged = false;

  static uint8_t deferRel[MZ_KEY_ROWS];
  for (int r = 0; r < MZ_KEY_ROWS; r++) if (deferRel[r]) { kbdMatrix[r] |= deferRel[r]; deferRel[r] = 0; }
  uint8_t pressedNow[MZ_KEY_ROWS] = { 0 };

  auto kb = PS2Controller.keyboard();
  if (!kb) return;
  int count = kbdAvailable(kb);
  if (count <= 0) return;

  while (count--) {
    int sc = kbdNextScancode(kb);
    if (sc == -1) break;
    if (sc == 0xF0) { isBreak = true; continue; }
    if (sc == 0xE0) { isE0 = true; continue; }
    if ((sc == 0x12 || sc == 0x59) && !isE0) isShift = !isBreak;
    if ((sc == 0x58 || sc == 0x13) && !isE0) {
      bool &held = (sc == 0x58) ? lockHeld : kanaHeld;
      if (!isBreak && !held && !isPaused) { bool &on = (sc == 0x58) ? ledLock : ledKana; on = !on; ledChanged = true; }
      held = !isBreak;
    }

    if (sc == 0x07 && !isE0) {
      if (!isBreak && !f12_held) {
        f12_held = true;
        if (isShift) esp_restart();
        isPaused = !isPaused;
        if (isPaused) {
          for (int i = 0; i < 20 && !cpuStopped; i++) vTaskDelay(pdMS_TO_TICKS(10));
          showStatus(PAUSE_MSG);
        }
        else { paletteForce = true; mzVideoInvalidate(); }
      } else if (isBreak) f12_held = false;
      isBreak = false; isE0 = false;
      continue;
    }
    if ((sc == 0x09 || sc == 0x78) && !isE0 && isPaused) {
      bool &held = (sc == 0x09) ? f10_held : f11_held;
      int d = (sc == 0x09) ? 0 : 1;
      if (!isBreak && !held) {
        held = true;
        if (isShift) writeBackD88(d);
        else {
          swapDisk(d);
          memset(hostDown, 0, sizeof(hostDown));
        }
      } else if (isBreak) held = false;
      isBreak = false; isE0 = false;
      continue;
    }

    for (int i = 0; i < keyMapCount; i++) {
      if (keyMapTable[i].ps2 == sc && keyMapTable[i].ext == isE0) {
        uint8_t k = keyMapTable[i].key, mask = (uint8_t)(1 << (k & 7));
        hostKeySet(sc, isE0, !isBreak);
        if (isBreak) {
          if (hostKeyAny(k)) break;
          if (pressedNow[k >> 3] & mask) deferRel[k >> 3] |= mask;
          else kbdMatrix[k >> 3] |= mask;
        } else {
          kbdMatrix[k >> 3] &= (uint8_t)~mask;
          pressedNow[k >> 3] |= mask;
          deferRel[k >> 3] &= (uint8_t)~mask;
        }
        break;
      }
    }
    isBreak = false; isE0 = false;
  }
  if (ledChanged) kbdSetLeds(ledLock, ledKana);
}

static bool loadFileToBuf(const char *path, uint8_t *buf, uint32_t maxSize) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  uint32_t sz = f.size();
  if (sz > maxSize) sz = maxSize;
  uint32_t done = 0;
  while (done < sz) {
    size_t chunk = min((size_t)4096, (size_t)(sz - done));
    size_t n = f.read(buf + done, chunk);
    if (n == 0) break;
    done += n;
  }
  f.close();
  return done > 0;
}

struct D88Item { String name; int kind; };
static std::vector<D88Item> d88Items;
static const char *ITEM_NONE = "<NONE>";

static void listD88Files(const String &path, bool canGoUp) {
  d88Items.clear();
  File dir = SD.open(path.c_str());
  if (!dir) return;
  std::vector<D88Item> dirs, files;
  File e;
  while ((e = dir.openNextFile())) {
    String name = e.name();
    int s = name.lastIndexOf('/');
    if (s >= 0) name = name.substring(s + 1);
    if (name.length() && name[0] != '.' && name != "System Volume Information") {
      if (e.isDirectory()) dirs.push_back({ name, 1 });
      else {
        String upper = name; upper.toUpperCase();
        if (upper.endsWith(".D88")) files.push_back({ name, 2 });
      }
    }
    e.close();
  }
  dir.close();
  auto byName = [](const D88Item &a, const D88Item &b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; };
  std::sort(dirs.begin(), dirs.end(), byName);
  std::sort(files.begin(), files.end(), byName);
  if (canGoUp) d88Items.push_back({ "..", 0 });
  d88Items.insert(d88Items.end(), dirs.begin(), dirs.end());
  d88Items.insert(d88Items.end(), files.begin(), files.end());
}

static int listD88Images(const String &path, D88Image *out, int max) {
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) return 0;
  uint32_t total = f.size(), o = 0;
  int n = 0;
  uint8_t h[0x20];
  while (n < max && o + 0x2B0 <= total) {
    if (!f.seek(o) || f.read(h, 0x20) != 0x20) break;
    uint32_t sz = h[0x1C] | (h[0x1D] << 8) | (h[0x1E] << 16) | ((uint32_t)h[0x1F] << 24);
    if (sz < 0x2B0 || o + sz > total) {
      if (n > 0) break;
      sz = total - o;
    }
    out[n].ofs = o;
    out[n].size = sz;
    for (int i = 0; i < 17; i++) out[n].name[i] = (h[i] >= 0x20 && h[i] < 0x7F) ? (char)h[i] : (h[i] ? '?' : 0);
    out[n].name[17] = 0;
    n++;
    o += sz;
  }
  f.close();
  return n;
}

static bool loadD88ToDrive(int d, const String &path, int image) {
  MzDrive &drv = drives[d];
  if (!drv.buffer) return false;
  D88Image imgs[D88_MAX_IMAGES];
  int n = listD88Images(path, imgs, D88_MAX_IMAGES);
  if (image < 0 || image >= n) return false;
  uint32_t sz = imgs[image].size;
  if (sz > MZ_D88_MAX) { Serial.printf("disk image too large: %u bytes\n", (unsigned)sz); return false; }
  File f = SD.open(path.c_str(), FILE_READ);
  if (!f) return false;
  f.seek(imgs[image].ofs);
  uint32_t done = 0;
  while (done < sz) {
    size_t r = f.read(drv.buffer + done, min((size_t)4096, (size_t)(sz - done)));
    if (r == 0) break;
    done += r;
  }
  f.close();
  bool ok = mzLoadD88(drv, drv.buffer, done);
  if (ok) {
    drivePath[d] = path;
    driveOfs[d] = imgs[image].ofs;
    int tracks = 0, sectors = 0;
    for (int t = 0; t < MZ_D88_TRACKS; t++) if (drv.trk_nsec[t]) { tracks++; sectors += drv.trk_nsec[t]; }
    Serial.printf("D88 %s [%d/%d]: %u bytes, %d tracks, %d sectors%s\n", path.c_str(), image + 1, n, (unsigned)done, tracks, sectors,
                  drv.protect ? " (write protected)" : "");
  } else drivePath[d] = "";
  return ok;
}

static void writeBackD88(int d) {
  if (drivePath[d] == "") { flashStatus("NO D88 LOADED"); showStatus("NO DISK"); return; }
  const MzDrive &drv = drives[d];
  if (!drv.has_d88_header || drv.size == 0) { flashStatus("D88 WRITEBACK FAILED"); showStatus("D88 WRITE BACK FAILED"); return; }
  File f = SD.open(drivePath[d].c_str(), "r+");
  bool ok = f && f.seek(driveOfs[d]) && f.write(drv.buffer, drv.size) == drv.size;
  if (f) f.close();
  flashStatus(ok ? "D88 WRITTEN BACK OK" : "D88 WRITEBACK FAILED");
  showStatus(ok ? "D88 WRITTEN BACK" : "D88 WRITE BACK FAILED");
}

static String selectD88File(const char *title, const String &current) {
  const String base = MZ_DIR;
  String sub = "", want = current;
  int slash = current.lastIndexOf('/');
  if (current != ITEM_NONE && slash >= 0) { sub = current.substring(0, slash); want = current.substring(slash + 1); }
  menuPalette();
  canvas->selectFont(&fabgl::FONT_8x16);
  auto kb = PS2Controller.keyboard();
  const int visible = 20;

  for (;;) {
    String path = base + (sub == "" ? "" : "/" + sub);
    listD88Files(path, sub != "");
    if (sub == "") d88Items.insert(d88Items.begin(), { String(ITEM_NONE), 3 });
    else if (d88Items.empty()) { sub = ""; want = ""; continue; }
    int cursor = 0;
    for (int i = 0; i < (int)d88Items.size(); i++)
      if (d88Items[i].name == want) { cursor = i; break; }
    bool isBreak = false, isE0 = false, changeDir = false;

    int shownTop = -1, shownCursor = -1;
    auto drawItem = [&](int idx, int top) {
      int y = 72 + (idx - top) * 16;
      canvas->setBrushColor(fabgl::Color::Blue);
      canvas->fillRectangle(8, y, 639, y + 15);
      if (idx < 0 || idx >= (int)d88Items.size()) return;
      const D88Item &it = d88Items[idx];
      String label = (it.kind == 2 || it.kind == 3) ? it.name : it.name + "/";
      canvas->setPenColor(idx == cursor ? fabgl::Color::BrightYellow
                          : (it.kind == 2 || it.kind == 3) ? fabgl::Color::BrightWhite : fabgl::Color::BrightCyan);
      canvas->drawText(16, y, label.c_str());
    };
    int listTop = 0;
    auto draw = [&]() {
      if (cursor < listTop) listTop = cursor;
      if (cursor >= listTop + visible) listTop = cursor - visible + 1;
      int top = listTop;
      if (shownTop < 0) {
        canvas->setBrushColor(fabgl::Color::Blue);
        canvas->clear();
        canvas->setPenColor(fabgl::Color::White);
        canvas->drawText(8, 16, title);
        canvas->drawText(8, 32, "UP/DOWN:SELECT ENTER:OK BS:UP ESC:CANCEL");
        String shown = path + "/";
        if (shown.length() > 76) shown = "..." + shown.substring(shown.length() - 73);
        canvas->setPenColor(fabgl::Color::Cyan);
        canvas->drawText(8, 48, shown.c_str());
      }
      if (shownTop < 0 || top != shownTop) {
        for (int i = 0; i < visible; i++) drawItem(top + i, top);
      } else if (cursor != shownCursor) {
        drawItem(shownCursor, top);
        drawItem(cursor, top);
      }
      shownTop = top;
      shownCursor = cursor;
    };
    draw();

    while (!changeDir) {
      int jsc = 0;
      if (!kb->scancodeAvailable() && (jsc = joyMenuScancode(&isE0)) != 0) isBreak = false;
      if (jsc || kb->scancodeAvailable()) {
        int sc = jsc ? jsc : kb->getNextScancode(0);
        if (sc == 0xF0) { isBreak = true; continue; }
        if (sc == 0xE0) { isE0 = true; continue; }
        if (!isBreak) {
          int n = (int)d88Items.size();
          if (sc == 0x75 && isE0) { if (cursor > 0) cursor--; draw(); }
          else if (sc == 0x72 && isE0) { if (cursor < n - 1) cursor++; draw(); }
          else if (sc == 0x7D && isE0) { cursor = cursor - visible < 0 ? 0 : cursor - visible; draw(); }
          else if (sc == 0x7A && isE0) { cursor = cursor + visible >= n ? n - 1 : cursor + visible; draw(); }
          else if ((sc == 0x66 && sub != "") || (sc == 0x5A && d88Items[cursor].kind == 0)) {
            int p = sub.lastIndexOf('/');
            want = p >= 0 ? sub.substring(p + 1) : sub;
            sub = p >= 0 ? sub.substring(0, p) : "";
            changeDir = true;
          } else if (sc == 0x5A && d88Items[cursor].kind == 1) {
            sub = sub == "" ? d88Items[cursor].name : sub + "/" + d88Items[cursor].name;
            want = "";
            changeDir = true;
          } else if (sc == 0x5A) {
            const D88Item &it = d88Items[cursor];
            return (it.kind == 3 || sub == "") ? it.name : sub + "/" + it.name;
          } else if (sc == 0x76) {
            return "";
          }
        }
        isBreak = false; isE0 = false;
      }
      vTaskDelay(1);
    }
  }
}

static int selectD88Image(const char *title, const D88Image *imgs, int n, int current) {
  auto kb = PS2Controller.keyboard();
  int cursor = current < 0 || current >= n ? 0 : current;
  bool isBreak = false, isE0 = false, redraw = true;
  int shownCursor = -1;
  menuPalette();
  canvas->selectFont(&fabgl::FONT_8x16);
  auto drawLine = [&](int i) {
    char line[48];
    snprintf(line, sizeof(line), "%2d: %s", i + 1, imgs[i].name[0] ? imgs[i].name : "(NO NAME)");
    canvas->setBrushColor(fabgl::Color::Blue);
    canvas->fillRectangle(8, 72 + i * 16, 639, 72 + i * 16 + 15);
    canvas->setPenColor(i == cursor ? fabgl::Color::BrightYellow : fabgl::Color::BrightWhite);
    canvas->drawText(16, 72 + i * 16, line);
  };
  for (;;) {
    if (redraw) {
      if (shownCursor < 0) {
        canvas->setBrushColor(fabgl::Color::Blue);
        canvas->clear();
        canvas->setPenColor(fabgl::Color::White);
        canvas->drawText(8, 16, title);
        canvas->drawText(8, 32, "THIS FILE HAS SEVERAL DISKS. UP/DOWN:SELECT ENTER:OK ESC:CANCEL");
        for (int i = 0; i < n; i++) drawLine(i);
      } else if (shownCursor != cursor) {
        drawLine(shownCursor);
        drawLine(cursor);
      }
      shownCursor = cursor;
      redraw = false;
    }
    int jsc = 0;
    if (!kb->scancodeAvailable() && (jsc = joyMenuScancode(&isE0)) != 0) isBreak = false;
    if (jsc || kb->scancodeAvailable()) {
      int sc = jsc ? jsc : kb->getNextScancode(0);
      if (sc == 0xF0) { isBreak = true; continue; }
      if (sc == 0xE0) { isE0 = true; continue; }
      if (!isBreak) {
        if (sc == 0x75 && isE0) { if (cursor > 0) cursor--; redraw = true; }
        else if (sc == 0x72 && isE0) { if (cursor < n - 1) cursor++; redraw = true; }
        else if (sc == 0x5A) return cursor;
        else if (sc == 0x76) return -1;
      }
      isBreak = false; isE0 = false;
    }
    vTaskDelay(1);
  }
}

static String bootDisk[NUM_DRIVES];
static int bootImage[NUM_DRIVES];
static String lastChosen;
static String dirOf(const String &f) { int s = f.lastIndexOf('/'); return s >= 0 ? f.substring(0, s) : String(""); }

static bool chooseDisk(int d, String &file, int &image, const String &otherFile, int otherImage) {
  String recent = lastChosen != "" ? lastChosen : otherFile;
  String start = file;
  if (recent != "" && (file == "" || dirOf(file) != dirOf(recent))) start = recent;
  String sel = selectD88File(d ? "MZ-2500  SELECT DISK FOR FD2" : "MZ-2500  SELECT DISK FOR FD1",
                             start == "" ? String(ITEM_NONE) : start);
  if (sel == "") return false;
  if (sel == ITEM_NONE) { file = ""; image = 0; return true; }
  D88Image imgs[D88_MAX_IMAGES];
  int n = listD88Images(MZ_DIR "/" + sel, imgs, D88_MAX_IMAGES);
  int img = 0;
  if (n > 1) {
    int def = (sel == file) ? image : (sel == otherFile) ? otherImage + (d == 1 ? 1 : -1) : (d == 1 ? 1 : 0);
    if (def < 0) def = 0;
    if (def >= n) def = n - 1;
    img = selectD88Image(sel.c_str(), imgs, n, def);
    if (img < 0) return false;
  }
  file = sel;
  image = img;
  lastChosen = sel;
  return true;
}

static void driveDisk(int d, String &file, int &image) {
  const String pre = MZ_DIR "/";
  file = drivePath[d].startsWith(pre) ? drivePath[d].substring(pre.length()) : String("");
  image = 0;
  if (file != "") {
    D88Image imgs[D88_MAX_IMAGES];
    int n = listD88Images(drivePath[d], imgs, D88_MAX_IMAGES);
    for (int i = 0; i < n; i++) if (imgs[i].ofs == driveOfs[d]) image = i;
  }
}

static const int MENU_ITEMS = (MZ_LATCH_COPY ? 8 : 7) + EMU_SWITCH;
static const int MENU_LATCH = 6;
#if EMU_SWITCH
static const int MENU_NEXT = MENU_ITEMS - 2;
static int emuNext = -1;
#endif
static char menuShown[MENU_ITEMS + 2][100];
static bool menuValid = false;
static void bootMenuInvalidate() { menuValid = false; }

static void bootMenuDraw(int cursor, const char *msg, int capture = -1) {
  const int X0 = 40, X1 = 599, Y0 = 72, DY = 26;
  canvas->selectFont(&fabgl::FONT_8x16);
  if (!menuValid) {
    menuPalette();
    canvas->setBrushColor(fabgl::Color::Blue);
    canvas->clear();
    canvas->setPenColor(fabgl::Color::BrightWhite);
    canvas->drawText(X0, 28, "SHARP MZ-2500 BOOT MENU");
    memset(menuShown, 0, sizeof(menuShown));
    menuValid = true;
  }
  for (int i = 0; i < MENU_ITEMS; i++) {
    char line[96];
    if (MZ_LATCH_COPY && i == MENU_LATCH) snprintf(line, sizeof(line), "LatchCpy: %s", mz_latch_copy ? "ON" : "OFF");
#if EMU_SWITCH
    else if (i == MENU_NEXT) { char m[48]; emuNextLabel(emuNext, "MZ2500", m, sizeof(m)); snprintf(line, sizeof(line), "Next    : %s", m); }
#endif
    else switch (i) {
      case 0: case 1: {
        D88Image imgs[D88_MAX_IMAGES];
        int n = bootDisk[i] == "" ? 0 : listD88Images(MZ_DIR "/" + bootDisk[i], imgs, D88_MAX_IMAGES);
        if (n > 1) snprintf(line, sizeof(line), "FD%d     : %.50s [%d/%d]", i + 1, bootDisk[i].c_str(), bootImage[i] + 1, n);
        else snprintf(line, sizeof(line), "FD%d     : %.60s", i + 1, bootDisk[i].c_str());
        break;
      }
      case 2: snprintf(line, sizeof(line), "JoyToKey: %s", joyToKeyOn ? "ON" : "OFF"); break;
      case 3: case 4: case 5:
        if (i == capture) snprintf(line, sizeof(line), "Joy B%d  : PRESS A KEY...", i - 2);
        else snprintf(line, sizeof(line), "Joy B%d  : %s", i - 2, mzKeyName(joyButtonKey[i - 3]));
        break;
      default: snprintf(line, sizeof(line), "Run"); break;
    }
    bool sel = (i == cursor);
    char shown[100];
    snprintf(shown, sizeof(shown), "%c%s", sel ? (i == capture ? 'C' : 'S') : 'N', line);
    if (strcmp(shown, menuShown[i]) == 0) continue;
    strcpy(menuShown[i], shown);
    int y = Y0 + i * DY;
    canvas->setBrushColor(!sel ? fabgl::Color::Blue : i == capture ? fabgl::Color::BrightYellow : fabgl::Color::BrightWhite);
    canvas->fillRectangle(X0 - 4, y - 5, X1, y + 19);
    canvas->setPenColor(sel ? fabgl::Color::Blue : fabgl::Color::BrightWhite);
    canvas->drawText(X0, y, line);
  }
  const char *help = capture >= 0 ? "PRESS THE KEY FOR THIS JOYSTICK BUTTON (F12: CANCEL)"
                                  : "UP/DOWN: SELECT   ENTER: CHANGE / RUN   (JOYTOKEY: NUMPAD 1-9)";
  const char *texts[2] = { help, msg ? msg : "" };
  for (int k = 0; k < 2; k++) {
    if (strcmp(texts[k], menuShown[MENU_ITEMS + k]) == 0) continue;
    snprintf(menuShown[MENU_ITEMS + k], sizeof(menuShown[0]), "%s", texts[k]);
    int y = k ? 400 - 40 : 400 - 68;
    canvas->setBrushColor(fabgl::Color::Blue);
    canvas->fillRectangle(X0 - 4, y - 2, 639, y + 17);
    canvas->setPenColor(k ? fabgl::Color::BrightWhite : fabgl::Color::BrightCyan);
    canvas->drawText(X0, y, texts[k]);
  }
}

static void bootMenu() {
  auto kb = PS2Controller.keyboard();
  const int NITEMS = MENU_ITEMS;
  int pos = 0, capture = -1;
  const char *msg = nullptr;
  bool isBreak = false, isE0 = false, redraw = true, joyChanged = false;
#if EMU_SWITCH
  emuNext = emuNextInit("MZ2500");
#endif
  bootMenuInvalidate();
  for (;;) {
    if (redraw) { bootMenuDraw(pos, msg, capture); redraw = false; }
    int jsc = 0;
    if (capture < 0 && !kb->scancodeAvailable() && (jsc = joyMenuScancode(&isE0)) != 0) isBreak = false;
    if (jsc || kb->scancodeAvailable()) {
      int sc = jsc ? jsc : kb->getNextScancode(0);
      if (sc == 0xF0) { isBreak = true; continue; }
      if (sc == 0xE0) { isE0 = true; continue; }
      if (!isBreak && capture >= 0) {
        for (int i = 0; i < keyMapCount; i++)
          if (keyMapTable[i].ps2 == sc && keyMapTable[i].ext == isE0) {
            joyButtonKey[capture - 3] = keyMapTable[i].key;
            joyChanged = true;
            break;
          }
        capture = -1;
        redraw = true;
      } else if (!isBreak) {
        if (sc == 0x75 && isE0)      { pos = (pos + NITEMS - 1) % NITEMS; redraw = true; }
        else if (sc == 0x72 && isE0) { pos = (pos + 1) % NITEMS; redraw = true; }
        else if (sc == 0x5A && pos >= 3 && pos <= 5) { capture = pos; redraw = true; }
        else if (sc == 0x5A && pos == 2) { joyToKeyOn = !joyToKeyOn; joyChanged = true; redraw = true; }
        else if (sc == 0x5A && MZ_LATCH_COPY && pos == MENU_LATCH) { mz_latch_copy = !mz_latch_copy; redraw = true; }
#if EMU_SWITCH
        else if (sc == 0x5A && pos == MENU_NEXT) { emuNext = emuNextCycle(emuNext, "MZ2500"); redraw = true; }
#endif
        else if (sc == 0x5A) {
          msg = nullptr;
          if (pos == 0 || pos == 1) {
            bootMenuInvalidate();
            if (chooseDisk(pos, bootDisk[pos], bootImage[pos], bootDisk[pos ^ 1], bootImage[pos ^ 1]) && pos == 0 && bootDisk[0] != "" && bootDisk[1] == "") {
              D88Image imgs[D88_MAX_IMAGES];
              if (bootImage[0] + 1 < listD88Images(MZ_DIR "/" + bootDisk[0], imgs, D88_MAX_IMAGES)) {
                bootDisk[1] = bootDisk[0];
                bootImage[1] = bootImage[0] + 1;
              }
            }
          } else {
            bootMenuDraw(pos, "LOADING...");
            for (int d = 0; d < NUM_DRIVES && !msg; d++)
              if (bootDisk[d] != "" && !loadD88ToDrive(d, MZ_DIR "/" + bootDisk[d], bootImage[d])) msg = d ? "FD2 LOAD ERROR" : "FD1 LOAD ERROR";
            if (!msg) break;
          }
          redraw = true;
        }
      }
      isBreak = false; isE0 = false;
    }
    vTaskDelay(1);
  }
  if (joyChanged) saveJoyKeys();
  canvas->setBrushColor(fabgl::Color::Black);
  canvas->clear();
}

static void showStatus(const char *msg) {
  int dark = 0, bright = 15, dv = 1 << 30, bv = -1;
  for (int i = 0; i < 16; i++) {
    const int v = shownPal[i][0] + shownPal[i][1] + shownPal[i][2];
    if (v < dv) { dv = v; dark = i; }
    if (v > bv) { bv = v; bright = i; }
  }
  mzVideoUnshareAll();
  uint8_t **rows = DisplayController.rowTable();
  const fabgl::FontInfo &fnt = fabgl::FONT_8x16;
  for (int r = 0; r < 20; r++) memset(rows[380 + r], (dark << 4) | dark, MZ_SCREEN_W / 2);
  for (int i = 0; msg[i] && i < 79; i++) {
    const uint8_t *g = fnt.data + (uint8_t)msg[i] * 16;
    for (int r = 0; r < 16; r++) {
      uint8_t *d = rows[382 + r] + 2 + i * 4;
      uint8_t bits = g[r];
      for (int k = 0; k < 4; k++) {
        uint8_t hi = (bits & (0x80 >> (2 * k))) ? bright : dark, lo = (bits & (0x40 >> (2 * k))) ? bright : dark;
        d[k] = (uint8_t)((hi << 4) | lo);
      }
    }
  }
  mzVideoInvalidate();
}

static void swapDisk(int d) {
  String file, other;
  int image, otherImage;
  driveDisk(d, file, image);
  driveDisk(d ^ 1, other, otherImage);
  bool err = false;
  if (chooseDisk(d, file, image, other, otherImage)) {
    if (file == "") { drives[d].inserted = false; drivePath[d] = ""; }
    else if (!loadD88ToDrive(d, MZ_DIR "/" + file, image)) err = true;
  }
  canvas->setBrushColor(fabgl::Color::Black);
  canvas->clear();
  canvas->waitCompletion();
  applyPalette();
  showStatus(err ? "D88 LOAD ERROR" : PAUSE_MSG);
}

#define VGA_START_LINE 380
static void vgaWaitFrame(int64_t frameStart) {
  for (;;) {
    int64_t el = esp_timer_get_time() - frameStart;
    if (el >= MZ_FRAME_US + 1500) return;
    if (el < MZ_FRAME_US / 2) { vTaskDelay(pdMS_TO_TICKS((MZ_FRAME_US / 2 - el) / 1000 + 1)); continue; }
    int s = MzVGA16::scanLine();
    if (s >= VGA_START_LINE + 4 || s <= 2) return;
    int us = (VGA_START_LINE + 4 - s) * 32;
    if (us > 2500) vTaskDelay(pdMS_TO_TICKS(us / 1000 - 1));
  }
}

void cpuTask(void *pvParameters) {
  const int64_t frameTimeUs = MZ_FRAME_US;
  int64_t nextFrameTime = esp_timer_get_time();
  uint32_t msAcc = 0;
  int cpuDebt = 0;
  bool renderSkipped = false;
  bool gaveUp = false;
  bool noWait = false, prevNoWait = false;
  uint32_t drawnGen = mz_vram_gen - 1;
  bool drawnBlink = false;

#ifdef USE_SERIAL_DEBUG
  int64_t dbgMin = INT64_MAX, dbgMax = 0, dbgSum = 0, dbgWinStart = 0, dbgIdle = 0;
  int dbgOverBudget = 0, dbgFrameCnt = 0;
#endif

  while (1) {
    if (isPaused) {
      cpuStopped = true;
      vTaskDelay(pdMS_TO_TICKS(10));
      nextFrameTime = esp_timer_get_time();
      continue;
    }
    cpuStopped = false;
#ifdef USE_SERIAL_DEBUG
    int64_t frameStart = esp_timer_get_time();
    if (dbgFrameCnt == 0) dbgWinStart = frameStart;
#endif
    if (paletteForce || mz_palette_gen != shownPalGen) {
      paletteForce = false;
      applyPalette();
    }
    updateJoystick(prevNoWait);
    joyToKeys();
    prevNoWait = noWait;
    mzFrameStart();
    const bool late = !renderSkipped && (gaveUp || esp_timer_get_time() > nextFrameTime + 1000);
    gaveUp = false;
    const bool blink = mzVideoBlinkPhase();
    const bool same = (mz_vram_gen == drawnGen && blink == drawnBlink);
    mz_render_skip = late || same;
    if (!mz_render_skip) { drawnGen = mz_vram_gen; drawnBlink = blink; }
    renderSkipped = late;
    nextFrameTime += frameTimeUs;
    for (int i = 0; i < MZ_LINES_FRAME; i++) {
      mzLineStart(i);
      const int cyc = mz_cyc_line;
      const int run = cyc - cpuDebt;
      if (run > 0) cpuDebt = -mzZ80Run(&mz_cpu, run); else cpuDebt = -run;
      if (mzIrqPending() && mz_cpu.iff1) z80core_int(&mz_cpu, mzIrqAck());
      mzLineEnd(i);
      opnEmuClock += cyc;
      msAcc += cyc;
      while (msAcc >= 6000) { msAcc -= 6000; mzFdcTick1ms(); }
      if (i == MZ_SCREEN_H) frameReady = true;
    }
#ifdef USE_SERIAL_DEBUG
    int64_t elapsed = esp_timer_get_time() - frameStart;
    if (elapsed < dbgMin) dbgMin = elapsed;
    if (elapsed > dbgMax) dbgMax = elapsed;
    dbgSum += elapsed;
    if (elapsed > frameTimeUs) dbgOverBudget++;
    if (++dbgFrameCnt >= 300) {
      const int64_t wall = esp_timer_get_time() - dbgWinStart;
      Serial.printf("SPEED: %d%% of real | frame us min=%lld max=%lld avg=%lld over=%d/%d (budget %lld) | idle avg=%lldus\n",
                    (int)((int64_t)dbgFrameCnt * frameTimeUs * 100 / (wall > 0 ? wall : 1)),
                    (long long)dbgMin, (long long)dbgMax, (long long)(dbgSum / dbgFrameCnt), dbgOverBudget, dbgFrameCnt,
                    (long long)frameTimeUs, (long long)(dbgIdle / dbgFrameCnt));
      dbgMin = INT64_MAX; dbgMax = dbgSum = dbgIdle = 0;
      dbgOverBudget = dbgFrameCnt = 0;
    }
#endif

    int64_t now = esp_timer_get_time();
    int64_t waitTime = nextFrameTime - now;
    noWait = waitTime <= 0;
    if (waitTime > 0) {
      vgaWaitFrame(nextFrameTime - frameTimeUs);
      nextFrameTime = esp_timer_get_time();
#ifdef USE_SERIAL_DEBUG
      dbgIdle += nextFrameTime - now;
#endif
    } else if (waitTime < -2 * frameTimeUs) {
      nextFrameTime = now;
      gaveUp = true;
    }
  }
}

#ifndef BOARD_HAS_PSRAM
bool psramEnable() {
  if (esp_spiram_init() != ESP_OK) return false;
  esp_spiram_init_cache();
  if (!esp_spiram_test()) return false;
  if (esp_spiram_add_to_heapalloc() != ESP_OK) return false;
  heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
  return true;
}
#endif

static void setClockFromBuild() {
  static const char *mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char m[4] = { __DATE__[0], __DATE__[1], __DATE__[2], 0 };
  const int month = (int)((strstr(mon, m) - mon) / 3) + 1;
  const int day = atoi(__DATE__ + 4), year = atoi(__DATE__ + 7);
  const int hh = atoi(__TIME__), mm = atoi(__TIME__ + 3), ss = atoi(__TIME__ + 6);
  struct tm t = {};
  t.tm_year = year - 1900; t.tm_mon = month - 1; t.tm_mday = day; t.tm_hour = 12;
  mktime(&t);
  mzRtcSetTime(year, month, day, t.tm_wday, hh, mm, ss);
}

void setup() {
  Serial.begin(115200);
#ifndef BOARD_HAS_PSRAM
  Serial.printf("PSRAM: %s\n", psramEnable() ? "enabled by the sketch" : "init FAILED");
#endif
  setCpuFrequencyMhz(240);
  disableCore0WDT();
  disableCore1WDT();
  rtc_wdt_protect_off();
  rtc_wdt_disable();
  esp_task_wdt_delete(NULL);
  delay(100);

  PS2Controller.begin(PS2Preset::KeyboardPort0_MousePort1, fabgl::KbdMode::NoVirtualKeys);
  PS2Controller.keyboard()->begin(GPIO_NUM_33, GPIO_NUM_32, false, false);
  PS2Controller.keyboard()->setLEDs(false, false, false);
  Serial1.begin(115200, SERIAL_8N1, 39, -1);

  DisplayController.begin();
  {
    static char mode[96];
    const int vfp = 450 + (MZ_VGA_VTOTAL - 524) / 2;
    snprintf(mode, sizeof(mode), "\"640x400\" 25.175 640 656 752 800 400 %d %d %d -HSync -VSync", vfp, vfp + 2, MZ_VGA_VTOTAL);
    DisplayController.setResolution(mode, -1, -1, false);
    Serial.printf("VGA: %s (%.2fHz)\n", mode, 25175000.0 / 800 / MZ_VGA_VTOTAL);
  }

  M.tvram = (uint8_t *)heap_caps_malloc(0x1800, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  M.pcg   = (uint8_t *)heap_caps_malloc(0x2000, MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
  if (!M.tvram) M.tvram = (uint8_t *)ps_malloc(0x1800);
  if (!M.pcg) M.pcg = (uint8_t *)ps_malloc(0x2000);
  M.ram   = (uint8_t *)ps_malloc(0x40000);
  M.vram  = (uint8_t *)ps_malloc(MZ_VRAM_SIZE);
  M.ipl   = (uint8_t *)ps_malloc(0x8000);
  M.kanji = (uint8_t *)ps_malloc(0x40000);
  M.dic   = (uint8_t *)ps_malloc(0x40000);
  M.phone = (uint8_t *)ps_malloc(0x8000);
  for (int d = 0; d < MZ_NUM_DRIVES; d++) memset(&drives[d], 0, sizeof(drives[d]));
  for (int d = 0; d < NUM_DRIVES; d++) drives[d].buffer = (uint8_t *)ps_malloc(MZ_D88_MAX);
  if (!M.tvram || !M.pcg || !M.ram || !M.vram || !M.ipl || !M.kanji || !M.dic || !M.phone || !drives[0].buffer || !drives[1].buffer) {
    Serial.println("Memory allocation FAILED!");
    while (1) delay(1000);
  }
  memset(M.tvram, 0, 0x1800);
  memset(M.pcg, 0, 0x2000);
  memset(M.ram, 0, 0x40000);
  memset(M.vram, 0, MZ_VRAM_SIZE);
  memset(M.ipl, 0xFF, 0x8000);
  memset(M.kanji, 0xFF, 0x40000);
  memset(M.dic, 0xFF, 0x40000);
  memset(M.phone, 0xFF, 0x8000);
  mzFdcInit(drives, MZ_NUM_DRIVES);

  canvas = new fabgl::Canvas(&DisplayController);
  canvas->setBrushColor(fabgl::Color::Black);
  canvas->clear();
  if (psramFixMissing()) {
    Serial.printf("ESP32 revision %d needs the PSRAM errata fix: set Tools > PSRAM to \"Enabled\" and build again\n", ESP.getChipRevision());
    char msg[48];
    snprintf(msg, sizeof(msg), "ESP32 REV.%d NEEDS THE PSRAM BUG FIX", ESP.getChipRevision());
    canvas->selectFont(&fabgl::FONT_8x16);
    canvas->setPenColor(fabgl::Color::Red);
    canvas->drawText(8, 16, msg);
    canvas->drawText(8, 48, "ARDUINO IDE: TOOLS > PSRAM > \"Enabled\", THEN BUILD AGAIN");
    while (1) delay(1000);
  }

  opnGen.begin(16384);
  soundGenerator.attach(&opnGen);
  opnGen.enable(true);
  soundGenerator.setVolume(100);
  soundGenerator.play(true);

  SPI.begin(SD_SCLK, SD_MISO, SD_MOSI, SD_CS);
  bool sdOK = SD.begin(SD_CS, SPI), romOK = false;
  if (sdOK) {
#if EMU_SWITCH
    emuSwitchCheck("MZ2500");
#endif
    romOK = loadFileToBuf(MZ_DIR "/IPL.ROM", M.ipl, 0x8000);
    if (!loadFileToBuf(MZ_DIR "/KANJI.ROM", M.kanji, 0x40000)) Serial.println("KANJI.ROM not found (kanji not shown)");
    if (!loadFileToBuf(MZ_DIR "/DICT.ROM", M.dic, 0x40000)) Serial.println("DICT.ROM not found");
    if (!loadFileToBuf(MZ_DIR "/PHONE.ROM", M.phone, 0x8000) && !loadFileToBuf(MZ_DIR "/FILE.ROM", M.phone, 0x8000))
      Serial.println("PHONE.ROM / FILE.ROM not found");
  }
  if (!romOK) {
    canvas->selectFont(&fabgl::FONT_8x16);
    canvas->setPenColor(fabgl::Color::Red);
    canvas->drawText(8, 16, sdOK ? MZ_DIR "/IPL.ROM NOT FOUND" : "SD CARD NOT FOUND");
    canvas->drawText(8, 48, "PUT ROMS IN " MZ_DIR "/ ON SD CARD");
    while (1) delay(1000);
  }
  loadJoyKeys();
  bootMenu();
#if MZ_LATCH_COPY
  Serial.printf("LatchCpy: %s\n", mz_latch_copy ? "ON" : "OFF");
#endif

  DisplayController.processPrimitives();
  canvas->waitCompletion();
  uint8_t **vpRows = DisplayController.rowTable();
  for (int y = 0; y < MZ_SCREEN_H; y++) {
    if (!vpRows[y] || ((uintptr_t)vpRows[y] & 3)) {
      Serial.printf("VIEWPORT row %d is NULL or unaligned (%p)\n", y, vpRows[y]);
      while (1) delay(1000);
    }
  }
  mz_rows = vpRows;

  mzInit();
  setClockFromBuild();
  mzReset();
  z80core_init(&mz_cpu);
  applyPalette();

  Serial.println("MZ-2500 Booting...");
#ifndef USE_SERIAL_DEBUG
  Serial.end();
#endif
  xTaskCreatePinnedToCore(cpuTask, "cpuTask", 8192, NULL, 24, NULL, 0);
}

static void updateMouse(bool discard) {
  auto m = PS2Controller.mouse();
  if (!m) return;
  while (m->deltaAvailable()) {
    MouseDelta d;
    if (!m->getNextDelta(&d, 0)) break;
    if (discard) continue;
    __atomic_fetch_add(&mz_mouse_dx, (int32_t)d.deltaX, __ATOMIC_RELAXED);
    __atomic_fetch_add(&mz_mouse_dy, (int32_t)-d.deltaY, __ATOMIC_RELAXED);
    mz_mouse_buttons = (d.buttons.left ? 1 : 0) | (d.buttons.right ? 2 : 0);
  }
}

void loop() {
  opnGen.fillBuffer();
  if (frameReady || isPaused) {
    frameReady = false;
    updateKeyboard();
    updateMouse(isPaused);
  }
  vTaskDelay(1);
}
