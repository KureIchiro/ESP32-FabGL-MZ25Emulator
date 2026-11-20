// ============================================================================
//  機種の切り替え(全機種で同じファイル。起動用プログラム EmuLauncher と組で使う)
//  ・SD の /EMU に機種ごとのプログラム(<ID>.BIN)と、次に起動する機種の ID を書いた NEXT.TXT を置く
//  ・各機種は起動時(SD を開いたあと)に emuSwitchCheck を呼ぶ。NEXT.TXT の機種が自分でなく、その .BIN があれば、
//    起動用プログラム(factory パーティション)に切り替えて再起動する。起動用プログラムが .BIN を書き込んで起動する
//  ・起動メニューの「Next」で次に起動する機種を選ぶ(NEXT.TXT に書く。電源を入れ直すかリセットすると切り替わる)
//  ・パーティションの分け方は partitions.csv(全機種と起動用プログラムで同じ。スケッチのフォルダに置く)
// ============================================================================
#pragma once
#include <Arduino.h>
#include <SD.h>
#include <esp_partition.h>
#include <esp_ota_ops.h>

#define EMU_DIR       "/EMU"
#define EMU_NEXT_FILE "/EMU/NEXT.TXT"
#define EMU_COUNT     9
static const char *const EMU_ID[EMU_COUNT]   = { "MSX", "MSX2", "MARKIII", "PC88", "X1", "X1TURBO", "MZ2500", "MD", "PCE" };
static const char *const EMU_NAME[EMU_COUNT] = { "MSX", "MSX2+", "MARK III", "PC-8801mkIISR", "X1", "X1turbo", "MZ-2500", "MEGA DRIVE", "PC ENGINE" };

static inline int emuIndexOf(const String &id) {
  for (int i = 0; i < EMU_COUNT; i++) if (id.equalsIgnoreCase(EMU_ID[i])) return i;
  return -1;
}
static inline String emuBinPath(int i) { return String(EMU_DIR "/") + EMU_ID[i] + ".BIN"; }
static inline bool emuBinExists(int i) { return i >= 0 && i < EMU_COUNT && SD.exists(emuBinPath(i)); }

static inline const esp_partition_t *emuLauncherPartition() {
  const esp_partition_t *p = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  uint8_t magic = 0;
  if (!p || esp_partition_read(p, 0, &magic, 1) != ESP_OK || magic != 0xE9) return nullptr;
  return p;
}

static inline int emuReadNext() {
  File f = SD.open(EMU_NEXT_FILE);
  if (!f) return -1;
  String s = f.readStringUntil('\n');
  f.close();
  s.trim();
  return emuIndexOf(s);
}
static inline bool emuWriteNext(int i) {
  if (i < 0 || i >= EMU_COUNT) return false;
  if (!SD.exists(EMU_DIR)) SD.mkdir(EMU_DIR);
  File f = SD.open(EMU_NEXT_FILE, FILE_WRITE);
  if (!f) return false;
  f.println(EMU_ID[i]);
  f.close();
  return true;
}

static inline void emuSwitchCheck(const char *myId) {
  const int next = emuReadNext();
  if (next < 0 || String(myId).equalsIgnoreCase(EMU_ID[next])) return;
  if (!emuBinExists(next)) { Serial.printf("EMU: %s not found, staying on %s\n", emuBinPath(next).c_str(), myId); return; }
  const esp_partition_t *p = emuLauncherPartition();
  if (!p) { Serial.println("EMU: launcher not installed (see EmuLauncher)"); return; }
  Serial.printf("EMU: switching to %s\n", EMU_ID[next]);
  if (esp_ota_set_boot_partition(p) == ESP_OK) { SD.end(); delay(50); esp_restart(); }
}

static inline int emuNextInit(const char *myId) {
  const int next = emuReadNext(), me = emuIndexOf(myId);
  return (next >= 0 && (next == me || emuBinExists(next))) ? next : me;
}
static inline int emuNextCycle(int cur, const char *myId) {
  if (!emuLauncherPartition()) return cur;
  const int me = emuIndexOf(myId);
  for (int k = 1; k <= EMU_COUNT; k++) {
    const int i = (cur + k) % EMU_COUNT;
    if (i == me || emuBinExists(i)) { emuWriteNext(i); return i; }
  }
  return cur;
}
static inline void emuNextLabel(int cur, const char *myId, char *buf, size_t n) {
  if (!emuLauncherPartition()) { snprintf(buf, n, "%s (NO LAUNCHER)", EMU_NAME[cur < 0 ? 0 : cur]); return; }
  if (cur == emuIndexOf(myId)) snprintf(buf, n, "%s", EMU_NAME[cur]);
  else snprintf(buf, n, "%s (AFTER RESET)", EMU_NAME[cur]);
}
