// ============================================================================
//  マウス: Z80 SIO とマウス(2026-11-06。X1 版 x1_mouse.cpp と同じ作り)
//  Common Source Project(vm/mz2500/mz2500.cpp・serial.cpp・mouse.cpp、vm/z80sio.cpp)と同じつなぎ方:
//    ・SIO は CDh の bit7 が 0 なら A0h〜A3h、1 なら B0h〜B3h に出る。選ばれていない方を読むと FFh。
//      ポート: +0 = A データ、+1 = A 制御、+2 = B データ、+3 = B 制御
//    ・CDh の bit0〜2 = チャンネル B、bit3〜5 = チャンネル A のクロック(4MHz / 13 / 2^n)
//    ・チャンネル B にマウス。YM2203 のポート A の bit3 が 1(マウスを選ぶ)のとき、ソフトが WR5 の DTR を立てる
//      (/DTR 端子が H→L)と、マウスが 3 バイト(状態・X の移動量・Y の移動量)を送る。状態は X1 と同じ:
//      bit0 = 左ボタン、bit1 = 右ボタン、bit4/5 = X が +/- にあふれた、bit6/7 = Y が +/- にあふれた
//    ・1 バイトの時間 = (スタート 1 + データ + パリティ + ストップ) × クロックの倍率 / クロック
//      (星くずばこ vol.12 は CDh = 0Ah・×16・8 ビット・ストップ 1 = 約 4800bps、1 バイト約 2.08ms)
//    ・割込みはデイジーチェーンで PIO の次、割込みコントローラ(C6h/C7h)の前(mz_core.cpp)。受信の割込みは、
//      割込みを出した文字が読まれずに残っているあいだ出し続ける(データを読むと減る。受け付けても消えない)。
//      RETI か WR0 の 38h(チャンネル A に書く)で処理中を解く
//  SIO は受信と割込みに要るところだけ(送信・同期モード・外部/ステータス割込みなどは無し)。
// ============================================================================
#include "mz.h"
#include <string.h>

volatile int32_t mz_mouse_dx = 0, mz_mouse_dy = 0;
volatile uint8_t mz_mouse_buttons = 0;
uint8_t mz_sio_irq = 0;
bool mz_sio_busy = false;

struct SioCh {
    uint8_t ptr;
    uint8_t wr[8];
    uint8_t fifo[4];
    int     n;
    int     recvIntr;
    bool    firstData;
    bool    nextRecv;
    bool    overflow;
    uint8_t pend[3];
    int     pendN, pendPos;
    int32_t delay;
};
static SioCh sio[2];
static bool sioInService;
static bool sioAddrA0 = true;
static uint8_t sioClkN[2];
static bool mouseSel;

static void updateIrq() {
    const bool req = sio[0].recvIntr > 0 || sio[1].recvIntr > 0;
    mz_sio_irq = (uint8_t)((req ? 1 : 0) | (sioInService ? 2 : 0));
}

static void chReset(SioCh &c) {
    const uint8_t vec = c.wr[2];
    memset(&c, 0, sizeof(c));
    c.wr[2] = vec;
}

void mzSioReset() {
    memset(sio, 0, sizeof(sio));
    sioInService = false;
    sioAddrA0 = true;
    sioClkN[0] = sioClkN[1] = 0;
    mouseSel = false;
    mz_sio_busy = false;
    updateIrq();
}

void mzSioSelect(uint8_t v) {
    sioAddrA0 = (v & 0x80) == 0;
    sioClkN[0] = (uint8_t)((v >> 3) & 7);
    sioClkN[1] = (uint8_t)(v & 7);
}

void mzMouseSelect(bool on) { mouseSel = on; }

static int32_t byteCycles(const SioCh &c, int ch) {
    static const uint8_t dataBits[4] = { 5, 7, 6, 8 };
    static const uint8_t mult[4] = { 1, 16, 32, 64 };
    const int stop = (c.wr[4] >> 2) & 3;
    const int halfBits = 2 * (1 + dataBits[c.wr[3] >> 6] + (c.wr[4] & 1)) + (stop == 2 ? 3 : stop == 3 ? 4 : 2);
    return (int32_t)halfBits * mult[c.wr[4] >> 6] * (13 << sioClkN[ch]) * 3 / 4;
}

static inline int32_t takeDelta(volatile int32_t *p) {
#if defined(__GNUC__)
    return __atomic_exchange_n(p, 0, __ATOMIC_RELAXED);
#else
    const int32_t v = *p; *p = 0; return v;
#endif
}

static void mouseSend() {
    int32_t dx = takeDelta(&mz_mouse_dx), dy = takeDelta(&mz_mouse_dy);
    uint8_t st = mz_mouse_buttons & 3;
    if (dx > 127)  { st |= 0x10; dx = 127; }  else if (dx < -128) { st |= 0x20; dx = -128; }
    if (dy > 127)  { st |= 0x40; dy = 127; }  else if (dy < -128) { st |= 0x80; dy = -128; }
    SioCh &c = sio[1];
    c.n = 0;
    c.recvIntr = 0;
    c.pend[0] = st; c.pend[1] = (uint8_t)dx; c.pend[2] = (uint8_t)dy;
    c.pendN = 3; c.pendPos = 0; c.delay = byteCycles(c, 1);
    c.firstData = true;
    mz_sio_busy = true;
    updateIrq();
}

void mzSioTick(uint32_t cyc) {
    SioCh &c = sio[1];
    if (!c.pendN) { mz_sio_busy = false; return; }
    c.delay -= (int32_t)cyc;
    if (c.delay > 0) return;
    if (!(c.wr[3] & 1)) { c.delay += byteCycles(c, 1); return; }
    if (c.n < 4) c.fifo[c.n++] = c.pend[c.pendPos]; else c.overflow = true;
    const uint8_t m = c.wr[1] & 0x18;
    if ((m == 0x08 && (c.firstData || c.nextRecv)) || (m & 0x10)) { c.recvIntr++; updateIrq(); }
    c.firstData = c.nextRecv = false;
    c.pendPos++;
    if (--c.pendN) c.delay += byteCycles(c, 1); else mz_sio_busy = false;
}

static uint8_t sioVector() {
    uint8_t v = sio[1].wr[2];
    if (sio[1].wr[1] & 0x04) {
        const int affect = sio[0].recvIntr > 0 ? 6 : sio[1].recvIntr > 0 ? 2 : 3;
        v = (uint8_t)((v & 0xF1) | (affect << 1));
    }
    return v;
}

uint8_t mzSioIrqAck() {
    sioInService = true;
    updateIrq();
    return sioVector();
}

void mzSioReti() {
    sioInService = false;
    updateIrq();
}

static inline bool selected(uint8_t port) { return ((port & 0xF0) == 0xA0) == sioAddrA0; }

uint8_t mzSioRead(uint8_t port) {
    if (!selected(port)) return 0xFF;
    const int ch = (port >> 1) & 1;
    SioCh &c = sio[ch];
    if (!(port & 1)) {
        if (!c.n) return 0x00;
        const uint8_t v = c.fifo[0];
        memmove(c.fifo, c.fifo + 1, --c.n);
        if (c.recvIntr > 0 && --c.recvIntr == 0) updateIrq();
        return v;
    }
    const int r = c.ptr;
    c.ptr = 0;
    switch (r) {
        case 0: return (uint8_t)((c.n ? 0x01 : 0) | ((ch == 0 && (mz_sio_irq & 1)) ? 0x02 : 0) | 0x04);
        case 1: return (uint8_t)(0x8F | (c.overflow ? 0x20 : 0));
        case 2: return ch ? sioVector() : 0x00;
        default: return 0x00;
    }
}

void mzSioWrite(uint8_t port, uint8_t v) {
    if (!selected(port)) return;
    const int ch = (port >> 1) & 1;
    SioCh &c = sio[ch];
    if (!(port & 1)) return;
    if (c.ptr == 0) {
        c.wr[0] = v;
        c.ptr = v & 7;
        switch (v & 0x38) {
            case 0x18:
                chReset(c);
                if (ch == 1) mz_sio_busy = false;
                updateIrq();
                break;
            case 0x20: c.nextRecv = true; break;
            case 0x30: c.overflow = false; break;
            case 0x38: if (ch == 0 && sioInService) mzSioReti(); break;
            default: break;
        }
        return;
    }
    const int r = c.ptr;
    c.ptr = 0;
    const uint8_t old = c.wr[r];
    c.wr[r] = v;
    if (ch == 1 && r == 5 && (v & 0x80) && !(old & 0x80) && mouseSel) mouseSend();
}
