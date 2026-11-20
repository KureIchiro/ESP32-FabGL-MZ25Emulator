// ============================================================================
//  SHARP MZ-2500 エミュレータ(ESP32 / FabGL)の共通の定義
//  参考: Common Source Code Project(武田俊也さん)の EmuZ-2500(src/vm/mz2500)、MAME mz2500.cpp
//  範囲: MZ-2500 モード(6MHz)。高解像度(24kHz・400 ライン)のモニタ。16 色・4 色・256 色の画面モード
//        (256 色は 16 色に置き換えて表示)。4096 色のボード・MZ-80B / MZ-2000 モードは入れない
// ============================================================================
#pragma once
#include <stdint.h>
#include "z80core.h"

#ifdef _MSC_VER
#define MZ_FAST
#else
#include <esp_attr.h>
#define MZ_FAST IRAM_ATTR
#endif

#define MZ_CPU_CLOCK      6000000
#define MZ_LINES_FRAME    448
#define MZ_CYC_FRAME      108128
#define MZ_CHARS_LINE     108
#define MZ_SCREEN_W       640
#define MZ_SCREEN_H       400

extern z80_t mz_cpu;
extern "C" int mzZ80Run(z80_t *z, int cycles);
extern int mz_line;
extern int mz_cyc_line;
static inline int mzLineCycle() {
    int c = mz_cyc_line - mz_cpu.icount;
    return c < 0 ? 0 : c;
}

struct MzMem {
    uint8_t *ram;
    uint8_t *vram;
    uint8_t *tvram;
    uint8_t *pcg;
    uint8_t *ipl;
    uint8_t *kanji;
    uint8_t *dic;
    uint8_t *phone;
};
extern MzMem M;
#define MZ_VPLANE(k)  ((uint32_t)(k) * 0x8000u + (uint32_t)(k) * 32u)
#define MZ_VRAM_SIZE  (MZ_VPLANE(3) + 0x8000u)
extern "C" uint8_t *mz_rd_map[32], *mz_wr_map[32];

void mzInit();
void mzReset();
void mzFrameStart();
void mzLineStart(int line);
void mzLineEnd(int line);
bool mzIrqPending();
uint8_t mzIrqAck();

#define MZ_KEY_ROWS 14
extern volatile uint8_t mz_key_matrix[MZ_KEY_ROWS];
extern volatile uint8_t mz_joy[2];

extern volatile int32_t mz_mouse_dx, mz_mouse_dy;
extern volatile uint8_t mz_mouse_buttons;
extern uint8_t mz_sio_irq;
extern bool mz_sio_busy;
void mzSioReset();
void mzSioSelect(uint8_t v);
void mzMouseSelect(bool on);
uint8_t mzSioRead(uint8_t port);
void mzSioWrite(uint8_t port, uint8_t v);
void mzSioTick(uint32_t cyc);
uint8_t mzSioIrqAck();
void mzSioReti();

void mzOpnWriteHook(uint8_t reg, uint8_t val);
void mzBeepHook(bool on);
uint32_t mzOpnClock();

extern uint8_t **mz_rows;
void mzVideoInit();
void mzVideoReset();
void mzVideoWriteIO(uint16_t port, uint8_t val);
uint8_t mzVideoReadIO(uint16_t port);
void mzVideoWriteCgData(uint8_t val);
uint8_t mzVideoRmwRead(uint16_t addr);
void mzVideoRmwWrite(uint16_t addr, uint8_t val);
#ifndef MZ_LATCH_COPY
#define MZ_LATCH_COPY 0
#endif
extern bool mz_latch_copy;
bool mzVideo4Color();
void mzVideoSignal(int id, bool on);
enum { MZV_COLUMN80 = 0, MZV_PAL4096, MZV_REVERSE, MZV_MASK };
void mzVideoVline(int v);
extern bool mz_vblank_g, mz_vblank_t;
extern int mz_hd_ts, mz_hd_te, mz_hd_gs, mz_hd_ge;
static inline bool mzVideoVblankG() { return mz_vblank_g; }
static inline bool mzVideoVblankT() { return mz_vblank_t; }
void mzRenderLine(int y);
#ifndef MZ_TILE256
#define MZ_TILE256 1
#endif
void mzVideoInvalidate();
void mzVideoUnshareAll();
void mzVideoDebugDump(char *buf, int n);
void mzVideoPalette(uint8_t rgb[16][3]);
extern uint32_t mz_palette_gen;
extern uint32_t mz_vram_gen;
extern uint32_t mz_cg_gen, mz_text_gen, mz_pcg_gen, mz_gv_chunk_gen[1024], mz_tx_chunk_gen[128];
static inline void mzMarkVram(uint16_t ofs) { mz_cg_gen++; mz_gv_chunk_gen[(ofs >> 5) & 1023] = ++mz_vram_gen; }
static inline void mzMarkText(uint16_t ofs) { mz_text_gen++; mz_tx_chunk_gen[(ofs >> 4) & 127] = ++mz_vram_gen; }
static inline void mzMarkPcg() { mz_text_gen++; mz_pcg_gen = ++mz_vram_gen; }
bool mzVideoBlinkPhase();
extern bool mz_render_skip;

#define MZ_D88_MAX          (1024u * 1024u)
#define MZ_D88_TRACKS       164
struct MzDrive {
    uint8_t *buffer;
    uint32_t size;
    bool     inserted;
    bool     has_d88_header;
    bool     protect;
    uint32_t trk_ofs[MZ_D88_TRACKS];
    uint8_t  trk_nsec[MZ_D88_TRACKS];
};
#define MZ_NUM_DRIVES 4
void mzFdcInit(MzDrive *drives, int numDrives);
void mzFdcReset();
uint8_t mzFdcReadIO(uint16_t port);
void mzFdcWriteIO(uint16_t port, uint8_t val);
void mzFdcTick1ms();
void mzFdcLineTick();
bool mzLoadD88(MzDrive &drv, const uint8_t *fileBuf, uint32_t fileSize);
uint32_t mzWriteBackD88(const MzDrive &drv, uint8_t *outBuf, uint32_t outBufSize);
void mzFdcDebugDump(char *buf, int bufSize);
void mzFdcSetReverse(bool on);

void mzRtcSetTime(int year, int mon, int day, int wday, int hour, int min, int sec);

extern uint32_t mz_frame_count;
