// ============================================================================
//  MZ-2500 本体: メモリのバンク / I/O / 割込み(デイジーチェーン) / Z80 PIO / 8255 / 8253 / RP5C15 /
//  YM2203 のポートとタイマー / キーボード / ジョイスティック
//  Common Source Code Project(武田俊也さん)の EmuZ-2500(src/vm/mz2500 と i8253.cpp・z80pio.cpp・
//  rp5c01.cpp)と同じ動き。I/O の割り当ては CSP mz2500.cpp の VM::VM
// ============================================================================
#pragma GCC optimize("O3")

#include "mz.h"
#include "ym2203.h"
#include <string.h>
#include <stdio.h>

Z80 mz_cpu;
MzMem M;
uint8_t *mz_rd_map[32], *mz_wr_map[32];
int mz_line = 0;
int mz_cyc_line = 241;
uint32_t mz_frame_count = 0;
volatile uint8_t mz_key_matrix[MZ_KEY_ROWS];
volatile uint8_t mz_joy[2] = { 0x3F, 0x3F };
bool mz_render_skip = false;

enum { PT_NORMAL = 0, PT_TVRAM, PT_GVRAM, PT_RMW, PT_PCG };
static uint8_t page[8];
static uint8_t page_type[8];
static uint8_t page_wait[8];
static uint8_t *page_base[8];
static uint16_t page_vofs[8];
static uint8_t bank_sel;
static uint8_t dic_bank, kanji_bank;
static uint8_t mode_reg;
static uint8_t wdmy[0x800], rdmy[0x800];

static void setMap(int b, uint8_t data) {
    uint8_t *r = rdmy, *w = wdmy;
    int type = PT_NORMAL, wait = 0;
    page_base[b] = nullptr;
    if (data <= 0x1F) {
        r = w = M.ram + data * 0x2000;
    } else if (data <= 0x2F) {
        static const int8_t ofs16[16] = { 0x00, 0x01, 0x04, 0x05, 0x08, 0x09, 0x0C, 0x0D, 0x02, 0x03, 0x06, 0x07, 0x0A, 0x0B, 0x0E, 0x0F };
        static const int8_t ofs4[16]  = { 0x00, 0x01, 0x04, 0x05, -1, -1, -1, -1, 0x08, 0x09, 0x0C, 0x0D, -1, -1, -1, -1 };
        int o = mzVideo4Color() ? ofs4[data - 0x20] : ofs16[data - 0x20];
        if (o >= 0) {
            page_base[b] = M.vram + MZ_VPLANE(o >> 2) + (o & 3) * 0x2000; page_vofs[b] = (uint16_t)((o & 3) * 0x2000);
            r = w = nullptr;
        }
        type = PT_GVRAM; wait = 1;
    } else if (data <= 0x33) {
        r = w = nullptr; type = PT_RMW; wait = 2;
    } else if (data <= 0x37) {
        r = M.ipl + (data - 0x34) * 0x2000;
    } else if (data == 0x38) {
        r = w = nullptr; type = PT_TVRAM; wait = 1;
    } else if (data == 0x39) {
        r = w = nullptr; type = PT_PCG; wait = 2;
    } else if (data == 0x3A) {
        r = M.dic + dic_bank * 0x2000;
    } else if (data >= 0x3C) {
        r = M.phone + (data - 0x3C) * 0x2000;
    }
    for (int i = 0; i < 4; i++) {
        mz_rd_map[b * 4 + i] = (r == nullptr) ? nullptr : (r == rdmy) ? rdmy : r + i * 0x800;
        mz_wr_map[b * 4 + i] = (w == nullptr) ? nullptr : (w == wdmy) ? wdmy : w + i * 0x800;
    }
    page[b] = data;
    page_type[b] = (uint8_t)type;
    page_wait[b] = (uint8_t)wait;
}
static void setMapNext(uint8_t data) { setMap(bank_sel, data); bank_sel = (bank_sel + 1) & 7; }

void mzRefreshMap() { for (int b = 0; b < 8; b++) setMap(b, page[b]); }

static inline void vramWait(int b) {
    int stall = page_wait[b];
    const int pos = mzLineCycle();
    if (page_type[b] == PT_TVRAM || page_type[b] == PT_PCG) {
        if (!mz_vblank_t && pos >= mz_hd_ts && pos < mz_hd_te) stall += mz_hd_te - pos;
    } else {
        if (!mz_vblank_g && pos >= mz_hd_gs && pos < mz_hd_ge) stall += mz_hd_ge - pos;
    }
    mz_cpu.ICount -= stall;
}

#if defined(__GNUC__)
#define MZ_ALWAYS_INLINE inline __attribute__((always_inline))
#else
#define MZ_ALWAYS_INLINE inline
#endif

static MZ_ALWAYS_INLINE byte mzRdSlowBody(zword A) {
    const int b = A >> 13, ofs = A & 0x1FFF;
    vramWait(b);
    switch (page_type[b]) {
        case PT_GVRAM: return page_base[b] ? page_base[b][ofs] : 0xFF;
        case PT_RMW:   return mzVideoRmwRead((uint16_t)(((page[b] - 0x30) << 13) | ofs));
        case PT_TVRAM: return ofs < 0x1800 ? M.tvram[ofs] : 0xFF;
        case PT_PCG:
            if ((kanji_bank & 0x80) && ofs < 0x800) return M.kanji[(kanji_bank & 0x7F) * 0x800 + ofs];
            return M.pcg[ofs];
    }
    return 0xFF;
}
extern "C" byte MZ_FAST MzRdSlow(zword A) { return mzRdSlowBody(A); }

static MZ_ALWAYS_INLINE void mzWrSlowBody(zword A, byte V) {
    const int b = A >> 13, ofs = A & 0x1FFF;
    vramWait(b);
    switch (page_type[b]) {
        case PT_GVRAM: if (page_base[b] && page_base[b][ofs] != V) { page_base[b][ofs] = V; mzMarkVram((uint16_t)(page_vofs[b] + ofs)); } break;
        case PT_RMW:   mzVideoRmwWrite((uint16_t)(((page[b] - 0x30) << 13) | ofs), V); break;
        case PT_TVRAM: if (ofs < 0x1800 && M.tvram[ofs] != V) { M.tvram[ofs] = V; mzMarkText((uint16_t)ofs); } break;
        case PT_PCG:
            if ((kanji_bank & 0x80) && ofs < 0x800) break;
            if (M.pcg[ofs] != V) { M.pcg[ofs] = V; mzMarkPcg(); }
            break;
    }
}
extern "C" void MZ_FAST MzWrSlow(zword A, byte V) { mzWrSlowBody(A, V); }

struct IrqCh { bool enb, req, inService; uint8_t vector; };
static IrqCh irqc[4];
static uint8_t irqc_select;

struct PioPort {
    uint8_t mode, dir, mask, ctrl1, vector, wreg, rreg;
    bool setDir, setMask, enb, enbTmp, req, inService;
};
static PioPort pio[2];
enum { PIO_OUT = 0x00, PIO_IN = 0x40, PIO_BIDIR = 0x80, PIO_CTRL = 0xC0 };

bool MZ_FAST mzIrqPending() {
    for (int ch = 0; ch < 2; ch++) {
        if (pio[ch].inService) return false;
        if (pio[ch].enb && pio[ch].req) return true;
    }
    if (mz_sio_irq & 2) return false;
    if (mz_sio_irq & 1) return true;
    for (int ch = 0; ch < 4; ch++) {
        if (irqc[ch].inService) return false;
        if (irqc[ch].enb && irqc[ch].req) return true;
    }
    return false;
}

uint8_t MZ_FAST mzIrqAck() {
    for (int ch = 0; ch < 2; ch++) {
        if (pio[ch].inService) return 0xFF;
        if (pio[ch].enb && pio[ch].req) { pio[ch].req = false; pio[ch].inService = true; return pio[ch].vector; }
    }
    if (mz_sio_irq & 2) return 0xFF;
    if (mz_sio_irq & 1) return mzSioIrqAck();
    for (int ch = 0; ch < 4; ch++) {
        if (irqc[ch].inService) return 0xFF;
        if (irqc[ch].enb && irqc[ch].req) { irqc[ch].req = false; irqc[ch].inService = true; return irqc[ch].vector; }
    }
    return 0xFF;
}

extern "C" int MZ_FAST mzIrqTake(void) { return mzIrqPending() ? mzIrqAck() : -1; }

extern "C" void MZ_FAST mzReti(void) {
    for (int ch = 0; ch < 2; ch++) if (pio[ch].inService) { pio[ch].inService = false; return; }
    if (mz_sio_irq & 2) { mzSioReti(); return; }
    for (int ch = 0; ch < 4; ch++) if (irqc[ch].inService) { irqc[ch].inService = false; return; }
}

static bool irq_line[4];
static void irqSignal(int ch, bool on) {
    if (on != irq_line[ch]) { irq_line[ch] = on; irqc[ch].req = on; }
}

static void pioCheckMode3(int ch) {
    PioPort &p = pio[ch];
    const uint8_t mask = (uint8_t)~p.mask;
    uint8_t val = (uint8_t)(((p.rreg & p.dir) | (p.wreg & ~p.dir)) & mask);
    switch (p.ctrl1 & 0x60) {
        case 0x00: p.req = (val != mask); break;
        case 0x20: p.req = (val != 0); break;
        case 0x40: p.req = (val == 0); break;
        default:   p.req = (val == mask); break;
    }
}

static uint8_t key_column;
static uint8_t key_val = 0xFF;
static void keyUpdate() {
    uint8_t v = 0xFF;
    if (key_column & 0x10) {
        int c = key_column & 0x0F;
        if (c < MZ_KEY_ROWS) v = mz_key_matrix[c];
    } else {
        for (int c = 0; c < MZ_KEY_ROWS; c++) v &= mz_key_matrix[c];
    }
    key_val = v;
    PioPort &p = pio[1];
    p.rreg = v;
    if (p.mode == PIO_CTRL) pioCheckMode3(1);
    else if (p.mode == PIO_IN || p.mode == PIO_BIDIR) p.req = true;
}

static void pioPortAOut(uint8_t v) {
    key_column = v & 0x1F;
    mzVideoSignal(MZV_COLUMN80, (v & 0x20) != 0);
    keyUpdate();
}

static void pioWrite(int addr, uint8_t data) {
    const int ch = (addr >> 1) & 1;
    PioPort &p = pio[ch];
    if (!(addr & 1)) {
        if (ch == 0 && p.wreg != data) pioPortAOut(data);
        p.wreg = data;
        if (p.mode == PIO_CTRL) pioCheckMode3(ch);
        return;
    }
    if (p.setDir) { p.dir = data; p.setDir = false; }
    else if (p.setMask) { p.mask = data; p.setMask = false; p.enb = p.enbTmp; }
    else if (!(data & 0x01)) p.vector = data;
    else if ((data & 0x0F) == 0x03) { p.enb = (data & 0x80) != 0; }
    else if ((data & 0x0F) == 0x07) {
        p.enb = (data & 0x80) != 0;
        p.ctrl1 = data;
        if (data & 0x10) { p.setMask = true; p.req = false; p.enbTmp = p.enb; p.enb = false; }
    } else if ((data & 0x0F) == 0x0F) {
        const uint8_t m = data & 0xC0;
        if (m == PIO_OUT) p.dir = 0x00;
        else if (m == PIO_IN || m == PIO_BIDIR) p.dir = 0xFF;
        else p.setDir = true;
        p.mode = m;
    }
    if (p.mode == PIO_CTRL && !p.setMask && !p.setDir) pioCheckMode3(ch);
}

static uint8_t pioRead(int addr) {
    const int ch = (addr >> 1) & 1;
    const PioPort &p = pio[ch];
    if (!(addr & 1)) return (uint8_t)((p.rreg & p.dir) | (p.wreg & ~p.dir));
    return (uint8_t)(pio[0].mode | (pio[1].mode >> 4));
}

static uint8_t ppi_a, ppi_c, ppi_ctrl = 0x9B;
static bool special_reset_req;
static int ipl_reset_cyc = -1;
static uint8_t ppiPortB() {
    uint8_t v = 0x38;
    if (!mzVideoVblankG()) v |= 0x01;
    v |= key_val & 0x80;
    return v;
}
static void ppiPortCOut(uint8_t v) {
    const uint8_t old = ppi_c, chg = ppi_c ^ v;
    ppi_c = v;
    if (chg & 0x01) mzVideoSignal(MZV_MASK, (v & 0x01) != 0);
    if (chg & 0x04) mzBeepHook((v & 0x04) != 0);
    if (!(old & 0x02) && (v & 0x02)) { special_reset_req = true; mz_cpu.ICount = 0; }
    if ((old & 0x08) && !(v & 0x08)) ipl_reset_cyc = MZ_CPU_CLOCK / 10000;
    else if (!(old & 0x08) && (v & 0x08)) ipl_reset_cyc = -1;
}
static void ppiWrite(int addr, uint8_t v) {
    switch (addr & 3) {
        case 0: ppi_a = v; mzVideoSignal(MZV_REVERSE, (v & 0x10) == 0); break;
        case 1: break;
        case 2: ppiPortCOut(v); break;
        case 3:
            if (v & 0x80) { ppi_ctrl = v; ppi_a = 0; mzVideoSignal(MZV_REVERSE, true); ppiPortCOut(0); }
            else {
                const int bit = (v >> 1) & 7;
                ppiPortCOut((v & 1) ? (uint8_t)(ppi_c | (1 << bit)) : (uint8_t)(ppi_c & ~(1 << bit)));
            }
            break;
    }
}
static uint8_t ppiRead(int addr) {
    switch (addr & 3) {
        case 0: return ppi_a;
        case 1: return ppiPortB();
        case 2: return ppi_c;
    }
    return 0xFF;
}

struct PitCh {
    int32_t count; uint16_t countReg, latch; uint8_t ctrl, mode;
    bool countLatched, lowRead, highRead, lowWrite, highWrite;
    int delay; bool start, gate, prevIn, prevOut, nullCount;
};
static PitCh pit[3];
#define PIT_COUNT_VALUE(c) ((c).countReg == 0 ? 0x10000 : ((c).mode == 3 && (c).countReg == 1) ? 0x10001 : (int32_t)(c).countReg)
static void pitInputClock(int ch, int clock);
static void pitSetSignal(int ch, bool on) {
    const bool prev = pit[ch].prevOut;
    pit[ch].prevOut = on;
    if (prev == on) return;
    if (ch == 0) {
        irqSignal(1, on);
        if (pit[1].prevIn && !on) pitInputClock(1, 1);
        pit[1].prevIn = on;
    } else if (ch == 1) {
        if (pit[2].prevIn && !on) pitInputClock(2, 1);
        pit[2].prevIn = on;
    }
}
static void pitStart(int ch) {
    if (pit[ch].lowWrite || pit[ch].highWrite || !pit[ch].gate) return;
    pit[ch].start = true;
}
static void pitInputClock(int ch, int clock) {
    PitCh &c = pit[ch];
    if (!(c.start && clock)) return;
    if (c.delay > 0) {
        c.delay--; clock--;
        if (c.delay == 0) { c.count = PIT_COUNT_VALUE(c); c.nullCount = false; }
        if (clock == 0) return;
    }
    c.count -= clock;
    const int32_t tmp = PIT_COUNT_VALUE(c);
    for (;;) {
        if (c.mode == 3) {
            pitSetSignal(ch, c.count > (tmp >> 1));
        } else {
            if (c.count <= 1 && (c.mode == 2 || c.mode == 4 || c.mode == 5)) pitSetSignal(ch, false);
            if (c.count <= 0) pitSetSignal(ch, true);
        }
        if (c.count > 0) break;
        if (c.mode == 2 || c.mode == 3) { c.count += tmp; c.nullCount = false; continue; }
        c.start = false; c.count = 0x10000;
        break;
    }
}
static void pitGate(int ch, bool on) {
    PitCh &c = pit[ch];
    const bool prev = c.gate;
    c.gate = on;
    if (prev && !on) {
        if (!(c.mode == 1 || c.mode == 5)) c.start = false;
        if (c.mode == 2 || c.mode == 3) pitSetSignal(ch, true);
    } else if (!prev && on) {
        c.start = false;
        if (!(c.mode == 0 || c.mode == 4)) c.delay = 1;
        pitStart(ch);
        if (c.mode == 1) pitSetSignal(ch, false);
    }
}
static void pitLatch(int ch) {
    PitCh &c = pit[ch];
    c.latch = (uint16_t)c.count;
    c.countLatched = true;
    if ((c.ctrl & 0x30) == 0x10) { c.lowRead = true; c.highRead = false; }
    else if ((c.ctrl & 0x30) == 0x20) { c.lowRead = false; c.highRead = true; }
    else c.lowRead = c.highRead = true;
}
static void pitWrite(int addr, uint8_t data) {
    int ch = addr & 3;
    if (ch < 3) {
        PitCh &c = pit[ch];
        if (!c.lowWrite && !c.highWrite) {
            if (c.ctrl & 0x10) c.lowWrite = true;
            if (c.ctrl & 0x20) c.highWrite = true;
        }
        if (c.lowWrite) { c.countReg = data; c.lowWrite = false; }
        else if (c.highWrite) {
            if ((c.ctrl & 0x30) == 0x20) c.countReg = (uint16_t)(data << 8);
            else c.countReg |= (uint16_t)(data << 8);
            c.highWrite = false;
        }
        c.nullCount = true;
        pitSetSignal(ch, c.mode != 0);
        if (c.mode == 0 || c.mode == 4) { c.start = false; c.delay = c.prevIn ? 2 : 1; pitStart(ch); }
        else if ((c.mode == 2 || c.mode == 3) && !c.start) { c.delay = c.prevIn ? 2 : 1; pitStart(ch); }
        return;
    }
    if ((data & 0xC0) == 0xC0) return;
    ch = (data >> 6) & 3;
    PitCh &c = pit[ch];
    if (data & 0x30) {
        static const uint8_t modes[8] = { 0, 1, 2, 3, 4, 5, 2, 3 };
        c.mode = modes[(data >> 1) & 7];
        c.countLatched = c.lowRead = c.highRead = c.lowWrite = c.highWrite = false;
        c.ctrl = data;
        pitSetSignal(ch, c.mode != 0);
        c.start = false;
        c.countReg = 0;
        c.nullCount = true;
    } else if (!c.countLatched) {
        pitLatch(ch);
    }
}
static uint8_t pitRead(int addr) {
    const int ch = addr & 3;
    if (ch == 3) return 0xFF;
    PitCh &c = pit[ch];
    if (!c.countLatched && !c.lowRead && !c.highRead) pitLatch(ch);
    if (c.lowRead) {
        c.lowRead = false;
        if (!c.highRead) c.countLatched = false;
        return (uint8_t)c.latch;
    }
    if (c.highRead) { c.highRead = false; c.countLatched = false; return (uint8_t)(c.latch >> 8); }
    return 0xFF;
}
static void pitReset() {
    for (int ch = 0; ch < 3; ch++) {
        PitCh &c = pit[ch];
        memset(&c, 0, sizeof(c));
        c.gate = true; c.count = 0x10000; c.ctrl = 0x34; c.mode = 3; c.nullCount = true;
    }
}
static uint32_t pit_acc;

static uint8_t rtc_regs[16], rtc_time[13];
static struct { int year, mon, day, wday, hour, min, sec; } rtc_now = { 2026, 10, 29, 4, 12, 0, 0 };
static bool rtc_p1, rtc_p16, rtc_pulse;
static int rtc_cnt16;
static uint32_t rtc_acc;
#define BCD_LO(v) ((v) % 10)
#define BCD_HI(v) (((v) / 10) % 10)
static void rtcRead() {
    const bool h12 = !(rtc_regs[0x0A] & 1);
    const int hour = h12 ? rtc_now.hour % 12 : rtc_now.hour;
    const int ampm = (h12 && rtc_now.hour >= 12) ? 2 : 0;
    const int y = rtc_now.year % 100;
    const int v[13] = { BCD_LO(rtc_now.sec), BCD_HI(rtc_now.sec), BCD_LO(rtc_now.min), BCD_HI(rtc_now.min),
                        BCD_LO(hour), BCD_HI(hour) | ampm, rtc_now.wday, BCD_LO(rtc_now.day), BCD_HI(rtc_now.day),
                        BCD_LO(rtc_now.mon), BCD_HI(rtc_now.mon), BCD_LO(y), BCD_HI(y) };
    for (int i = 0; i < 13; i++) rtc_time[i] = (uint8_t)v[i];
}
static void rtcPulse() {
    bool p = false;
    if (!(rtc_regs[0x0F] & 8)) p |= rtc_p1;
    if (!(rtc_regs[0x0F] & 4)) p |= rtc_p16;
    rtc_pulse = !p;
}
void mzRtcSetTime(int year, int mon, int day, int wday, int hour, int min, int sec) {
    rtc_now.year = year; rtc_now.mon = mon; rtc_now.day = day; rtc_now.wday = wday;
    rtc_now.hour = hour; rtc_now.min = min; rtc_now.sec = sec;
    rtcRead();
}
static void rtcSecond() {
    static const int mdays[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (++rtc_now.sec < 60) return;
    rtc_now.sec = 0;
    if (++rtc_now.min < 60) return;
    rtc_now.min = 0;
    if (++rtc_now.hour < 24) return;
    rtc_now.hour = 0;
    rtc_now.wday = (rtc_now.wday + 1) % 7;
    int md = mdays[(rtc_now.mon - 1) % 12];
    if (rtc_now.mon == 2 && (rtc_now.year % 4) == 0) md = 29;
    if (++rtc_now.day <= md) return;
    rtc_now.day = 1;
    if (++rtc_now.mon <= 12) return;
    rtc_now.mon = 1; rtc_now.year++;
}
static void rtcWrite(int reg, uint8_t v) {
    reg &= 0x0F;
    if (reg <= 0x0C) {
        if (!(rtc_regs[0x0D] & 1)) {
            rtc_time[reg] = v;
            const bool h12 = !(rtc_regs[0x0A] & 1);
            rtc_now.sec = rtc_time[0] + (rtc_time[1] & 7) * 10;
            rtc_now.min = rtc_time[2] + (rtc_time[3] & 7) * 10;
            rtc_now.hour = h12 ? rtc_time[4] + (rtc_time[5] & 1) * 10 + ((rtc_time[5] & 2) ? 12 : 0) : rtc_time[4] + (rtc_time[5] & 3) * 10;
            rtc_now.day = rtc_time[7] + (rtc_time[8] & 3) * 10;
            rtc_now.mon = rtc_time[9] + (rtc_time[10] & 1) * 10;
            rtc_now.year = 2000 + rtc_time[11] + rtc_time[12] * 10;
            return;
        }
    }
    const uint8_t chg = rtc_regs[reg] ^ v;
    rtc_regs[reg] = v;
    if (reg == 0x0A && (chg & 1)) rtcRead();
    if (reg == 0x0F) rtcPulse();
}
static uint8_t rtcReadReg(int reg) {
    reg &= 0x0F;
    if (reg <= 0x0C && !(rtc_regs[0x0D] & 1)) return rtc_time[reg];
    if (reg == 0x0B) {
        for (int i = 0; i < 3; i++) if ((rtc_now.year - i) % 4 == 0) return (uint8_t)i;
        return 3;
    }
    return rtc_regs[reg];
}

static uint8_t opn_addr, opn_regs[256];
static bool opn_porta_set;
static Ym2203Timer opn_timer;
static uint32_t opn_us16_acc;
static uint32_t frame_cyc;
uint32_t mzOpnClock() { return frame_cyc + (uint32_t)mzLineCycle(); }

static uint8_t opnPortB() { return (uint8_t)(0x37 | (rtc_pulse ? 0x08 : 0x00)); }
static void opnWrite(uint8_t val) {
    const uint8_t reg = opn_addr;
    const uint8_t prev = opn_regs[reg];
    opn_regs[reg] = val;
    if (reg >= 0x24 && reg <= 0x27) ym2203TimerWrite(opn_timer, reg, val);
    if (reg == 0x2D || reg == 0x2E || reg == 0x2F) ym2203TimerWrite(opn_timer, reg, val);
    if (reg == 0x0E && (prev != val || !opn_porta_set)) {
        opn_porta_set = true;
        mzFdcSetReverse((val & 0x02) != 0);
        mzVideoSignal(MZV_PAL4096, (val & 0x04) == 0);
        mzMouseSelect((val & 0x08) != 0);
    }
    mzOpnWriteHook(reg, val);
}
static uint8_t opnRead() {
    if (opn_addr == 0x0E) return (opn_regs[0x07] & 0x40) ? opn_regs[0x0E] : 0xFF;
    if (opn_addr == 0x0F) return (opn_regs[0x07] & 0x80) ? opn_regs[0x0F] : opnPortB();
    if (opn_addr < 0x10) return opn_regs[opn_addr];
    if (opn_addr == 0xFF) return 0x01;
    return 0xFF;
}

static uint8_t joy_mode = 0x0F;
static uint8_t joyRead() {
    uint8_t val = 0x3F;
    const int num = (joy_mode & 0x40) ? 1 : 0;
    bool dir;
    if (num) { if (!(joy_mode & 0x04)) val &= ~0x20; if (!(joy_mode & 0x08)) val &= ~0x10; dir = !(joy_mode & 0x20); }
    else     { if (!(joy_mode & 0x01)) val &= ~0x20; if (!(joy_mode & 0x02)) val &= ~0x10; dir = !(joy_mode & 0x10); }
    const uint8_t j = (uint8_t)~mz_joy[num];
    if (dir) val &= (uint8_t)~(j & 0x0F);
    if (j & 0x10) val &= ~0x20;
    if (j & 0x20) val &= ~0x10;
    return val;
}

#ifdef _MSC_VER
void (*mz_out_trace)(uint16_t port, uint8_t val) = nullptr;
void (*mz_in_trace)(uint16_t port, uint8_t val) = nullptr;
#endif
static MZ_ALWAYS_INLINE void outBody(zword port, byte v) {
    const uint8_t p = port & 0xFF;
#ifdef _MSC_VER
    if (mz_out_trace) mz_out_trace(port, v);
#endif
    switch (p) {
        case 0xA0: case 0xA1: case 0xA2: case 0xA3:
        case 0xB0: case 0xB1: case 0xB2: case 0xB3: mzSioWrite(p, v); return;
        case 0xCD: mzSioSelect(v); return;
        case 0xAE: mzVideoWriteIO(port, v); return;
        case 0xB4: bank_sel = v & 7; return;
        case 0xB5: setMapNext(v & 0x3F); return;
        case 0xB7: mode_reg = v & 3; return;
        case 0xBD: mzVideoWriteCgData(v); return;
        case 0xBC: case 0xBE: case 0xBF: mzVideoWriteIO(port, v); return;
        case 0xC6:
            irqc[0].enb = (v & 8) != 0; irqc[1].enb = (v & 4) != 0; irqc[2].enb = (v & 2) != 0; irqc[3].enb = (v & 1) != 0;
            irqc_select = v;
            return;
        case 0xC7:
            if (irqc_select & 0x80) irqc[0].vector = v;
            if (irqc_select & 0x40) irqc[1].vector = v;
            if (irqc_select & 0x20) irqc[2].vector = v;
            if (irqc_select & 0x10) irqc[3].vector = v;
            return;
        case 0xC8:
            mz_cpu.ICount -= 1; opn_addr = v;
            if (v >= 0x2D && v <= 0x2F) { ym2203TimerWrite(opn_timer, v, 0); mzOpnWriteHook(v, 0); }
            return;
        case 0xC9: mz_cpu.ICount -= 1; opnWrite(v); return;
        case 0xCC: mz_cpu.ICount -= 3; rtcWrite(port >> 8, v); return;
        case 0xCE:
            dic_bank = v & 0x1F;
            for (int b = 0; b < 8; b++) if (page[b] == 0x3A) setMap(b, 0x3A);
            return;
        case 0xCF: kanji_bank = v; return;
        case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            mz_cpu.ICount -= 1; mzFdcWriteIO(port, v); return;
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: ppiWrite(p, v); return;
        case 0xE4: case 0xE5: case 0xE6: case 0xE7: pitWrite(p, v); return;
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: mz_cpu.ICount -= 1; pioWrite(p, v); return;
        case 0xEF: joy_mode = v; return;
        case 0xF0: case 0xF1: case 0xF2: case 0xF3:
            for (int ch = 0; ch < 2; ch++) { pitGate(ch, true); pitGate(ch, false); pitGate(ch, true); }
            return;
        case 0xF4: case 0xF5: case 0xF6: case 0xF7: mzVideoWriteIO(port, v); return;
        default: return;
    }
}
extern "C" void MZ_FAST OutZ80(zword port, byte v) { outBody(port, v); }

static inline uint8_t ioRead(zword port) {
    const uint8_t p = port & 0xFF;
    switch (p) {
        case 0xA0: case 0xA1: case 0xA2: case 0xA3:
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
            return mzSioRead(p);
        case 0xB4: return bank_sel;
        case 0xB5: { const uint8_t v = page[bank_sel]; bank_sel = (bank_sel + 1) & 7; return v; }
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: return mzVideoReadIO(port);
        case 0xC8: mz_cpu.ICount -= 1; return opn_timer.status & 3;
        case 0xC9: mz_cpu.ICount -= 1; return opnRead();
        case 0xCA: return 0x30;
        case 0xCC: mz_cpu.ICount -= 3; return rtcReadReg(port >> 8);
        case 0xD8: case 0xD9: case 0xDA: case 0xDB: mz_cpu.ICount -= 1; return mzFdcReadIO(port);
        case 0xE0: case 0xE1: case 0xE2: case 0xE3: return ppiRead(p);
        case 0xE4: case 0xE5: case 0xE6: case 0xE7: return pitRead(p);
        case 0xE8: case 0xE9: case 0xEA: case 0xEB: mz_cpu.ICount -= 1; return pioRead(p);
        case 0xEF: return joyRead();
        case 0xF4: case 0xF5: case 0xF6: case 0xF7: return mzVideoReadIO(port);
    }
    return 0xFF;
}
extern "C" byte MZ_FAST mzIoPeek(byte port) {
    switch (port) {
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: return mzVideoReadIO(port);
        case 0xC8: return opn_timer.status & 3;
        case 0xE0: case 0xE1: case 0xE2: return ppiRead(port);
    }
    return 0xFF;
}

extern "C" byte MZ_FAST InZ80(zword port) {
    const uint8_t v = ioRead(port);
#ifdef _MSC_VER
    if (mz_in_trace) mz_in_trace(port, v);
#endif
    return v;
}

extern "C" void PatchZ80(Z80 *R) { (void)R; }

void mzInit() {
    memset(rdmy, 0xFF, sizeof(rdmy));
    for (int r = 0; r < MZ_KEY_ROWS; r++) mz_key_matrix[r] = 0xFF;
    mzVideoInit();
    rtcRead();
}

void mzReset() {
    bank_sel = 0;
    dic_bank = kanji_bank = 0;
    mode_reg = 0;
    static const uint8_t init[8] = { 0x34, 0x35, 0x36, 0x37, 0x04, 0x05, 0x06, 0x07 };
    mzVideoReset();
    for (int b = 0; b < 8; b++) setMap(b, init[b]);
    memset(irqc, 0, sizeof(irqc));
    memset(irq_line, 0, sizeof(irq_line));
    irqc_select = 0;
    for (int ch = 0; ch < 2; ch++) {
        PioPort &p = pio[ch];
        memset(&p, 0, sizeof(p));
        p.mode = PIO_IN; p.dir = 0xFF; p.mask = 0xFF; p.wreg = 0x00; p.rreg = 0xFF;
    }
    key_column = 0;
    keyUpdate();
    ppi_a = 0; ppi_c = 0; ppi_ctrl = 0x9B;
    pitReset();
    pit_acc = 0;
    memset(rtc_regs, 0, sizeof(rtc_regs));
    rtc_regs[0x0A] = 1; rtc_regs[0x0D] = 8; rtc_regs[0x0F] = 0x0C;
    rtc_p1 = rtc_p16 = false; rtc_cnt16 = 0; rtc_acc = 0;
    rtcPulse();
    rtcRead();
    opn_addr = 0;
    memset(opn_regs, 0, sizeof(opn_regs));
    opn_porta_set = false;
    mzFdcSetReverse(false);
    ym2203TimerReset(opn_timer, 2000000);
    mzOpnWriteHook(0x2D, 0);
    opn_us16_acc = 0;
    joy_mode = 0x0F;
    mzSioReset();
    mzFdcReset();
    mz_line = 0;
    frame_cyc = 0;
    special_reset_req = false;
    ipl_reset_cyc = -1;
}

void mzFrameStart() {
    frame_cyc = 0;
    mz_frame_count++;
    keyUpdate();
}

void MZ_FAST mzLineStart(int line) {
    mz_line = line;
    mz_cyc_line = 241 + (((line * 160) % 448) < 160 ? 1 : 0);
    mzVideoVline(line);
    irqSignal(0, mzVideoVblankG());
}

static void specialReset() {
    Z80 &R = mz_cpu;
    R.PC.W = 0; R.SPtr.W = 0;
    R.AF.W = R.BC.W = R.DE.W = R.HL.W = 0; R.AF.B.l = Z_FLAG;
    R.AF1.W = R.BC1.W = R.DE1.W = R.HL1.W = 0;
    R.I = R.R = 0;
    R.IFF = 0;
    bank_sel = 0;
    for (int b = 0; b < 8; b++) setMap(b, (uint8_t)b);
}

void MZ_FAST mzLineEnd(int line) {
    const uint32_t cyc = (uint32_t)mz_cyc_line;
    if (special_reset_req) { special_reset_req = false; specialReset(); }
    if (ipl_reset_cyc >= 0) {
        ipl_reset_cyc -= (int)cyc;
        if (ipl_reset_cyc < 0) { mzReset(); ResetZ80(&mz_cpu); return; }
    }
    frame_cyc += cyc;
    if (mz_sio_busy) mzSioTick(cyc);
    pit_acc += cyc;
    while (pit_acc >= 192) { pit_acc -= 192; pitInputClock(0, 1); }
    rtc_acc += cyc;
    while (rtc_acc >= MZ_CPU_CLOCK / 32) {
        rtc_acc -= MZ_CPU_CLOCK / 32;
        if (++rtc_cnt16 >= 16) { rtc_cnt16 = 0; rtc_p1 = !rtc_p1; if (!rtc_p1) { rtcSecond(); if (rtc_regs[0x0D] & 8) rtcRead(); } }
        rtc_p16 = !rtc_p16;
        rtcPulse();
    }
    opn_us16_acc += cyc * 16;
    const uint32_t us16 = opn_us16_acc / 6;
    opn_us16_acc -= us16 * 6;
    ym2203TimerAdvance(opn_timer, us16);
    mzFdcLineTick();
    if (line < MZ_SCREEN_H && !mz_render_skip) mzRenderLine(line);
}
