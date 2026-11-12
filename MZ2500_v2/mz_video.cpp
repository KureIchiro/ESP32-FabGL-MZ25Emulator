// ============================================================================
//  MZ-2500 の画面: CRTC(テキスト F4h〜F7h・グラフィック BCh〜BFh)/ VRAM の読み書き回路 / 1 ラインずつの描画
//  Common Source Code Project(武田俊也さん)の EmuZ-2500 crtc.cpp と同じ結果になるように作る。
//  ・出力は 16 色のパレットの番号(4bpp)。パレットは 3 通り(mzVideoPalette):
//      16 色: アナログの 16 色(CSP palette16。8 = 灰色)。テキストの 8 色(デジタル)は色 c → c | 8(黒は 0)なので 16 色に入る
//      4096 色(YM2203 ポート A bit2 = 0): AEh で書かれた 16 色(4096 色のうちの 16 色)。テキストの色もパレットの
//        8〜15 番から選ばれる(CSP palette4096tmp)ので 16 色に入る。TTGO VGA32 は 64 色なので表示は 64 色に丸まる
//      256 色のモード: CGA の 16 色(pal256out。TTGO VGA32 がそのまま出せる 0・85・170・255 だけの色)
//  ・256 色のモード(320x200 / 320x400)は、色ごとに CGA の 4 色の 2x2(MZ_TILE256。0 なら近い 1 色)で出す。
//    テキストの色も CGA のいちばん近い色になる
//  ・GVRAM のバイトは bit0 が左端のドット(CSP cg_matrix)
// ============================================================================
#pragma GCC optimize("O3")

#include "mz.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

uint8_t **mz_rows = nullptr;
uint32_t mz_vram_gen = 0;
uint32_t mz_text_gen = 0, mz_cg_gen = 0;
uint32_t mz_gv_chunk_gen[1024], mz_tx_chunk_gen[128], mz_pcg_gen;
static uint32_t layout_gen = 1;
static uint32_t cgl_gen = 1;
static uint32_t color_gen = 1;
static uint32_t lineSig[MZ_SCREEN_H];
static bool lineValid[MZ_SCREEN_H];
static inline uint32_t mixWord(uint32_t h, uint32_t w) { h = (h ^ w) * 0x9E3779B1u; return h ^ (h >> 16); }
void mzRefreshMap();

static uint8_t textreg_num, textreg[16];
static uint8_t cgreg_num, cgreg[32];
static uint8_t palette_reg[16];
static uint8_t cg_mask = 0x0F, cg_mask256;
static bool cg_mask256_init;
static bool font_size = true;
static bool column80;
static bool pal_select, screen_reverse, screen_mask;
static uint8_t latch[4];
bool mz_latch_copy = false;
static uint8_t clear_flag;
static uint16_t GDEVS, GDEVE;
static uint8_t GDEHS, GDEHE;
bool mz_vblank_t = true, mz_vblank_g = true;
int mz_hd_ts, mz_hd_te, mz_hd_gs, mz_hd_ge;

static uint8_t pal256rgb[256][3];
static uint8_t pal256near[256];
static uint8_t tile256[256][2];
static const uint8_t dither512[512][2] = {
    {0x00,0x00}, {0x00,0x10}, {0x01,0x10}, {0x01,0x11}, {0x01,0x11}, {0x11,0x11}, {0x11,0x11}, {0x11,0x11},
    {0x00,0x20}, {0x00,0x30}, {0x01,0x30}, {0x01,0x31}, {0x01,0x31}, {0x11,0x31}, {0x11,0x31}, {0x11,0x91},
    {0x02,0x20}, {0x02,0x30}, {0x03,0x30}, {0x03,0x31}, {0x03,0x31}, {0x13,0x31}, {0x11,0xB1}, {0x11,0xB1},
    {0x02,0xA0}, {0x02,0xA0}, {0x02,0xB0}, {0x03,0xB0}, {0x03,0xB1}, {0x13,0xB1}, {0x13,0xB1}, {0x13,0xB1},
    {0x02,0xA2}, {0x02,0xA2}, {0x02,0xB2}, {0x03,0xB2}, {0x03,0xB3}, {0x13,0xB3}, {0x13,0xB3}, {0x13,0xB3},
    {0x22,0x22}, {0x22,0x32}, {0x23,0x32}, {0x23,0x33}, {0x23,0x33}, {0x33,0x33}, {0x33,0xB3}, {0x33,0xB3},
    {0x22,0xA2}, {0x2A,0xA2}, {0x2A,0xB2}, {0x23,0xB2}, {0x23,0xB3}, {0x2B,0xB3}, {0x3B,0xB3}, {0x3B,0xB3},
    {0x2A,0xA2}, {0x2A,0xAA}, {0x2A,0xAA}, {0x2A,0xBA}, {0x2B,0xBA}, {0x2B,0xBB}, {0x3B,0xBB}, {0x3B,0xBB},
    {0x00,0x40}, {0x00,0x50}, {0x01,0x50}, {0x01,0x51}, {0x01,0x51}, {0x11,0x51}, {0x11,0x51}, {0x11,0x91},
    {0x02,0x40}, {0x00,0x70}, {0x01,0x70}, {0x09,0x90}, {0x09,0x90}, {0x09,0x91}, {0x19,0x91}, {0x19,0x91},
    {0x02,0x60}, {0x03,0x60}, {0x03,0x61}, {0x13,0x61}, {0x13,0x61}, {0x09,0xB1}, {0x19,0xB1}, {0x19,0xB1},
    {0x02,0x62}, {0x03,0x62}, {0x03,0x63}, {0x13,0x63}, {0x13,0x63}, {0x13,0xB1}, {0x13,0xB1}, {0x19,0xB9},
    {0x22,0x62}, {0x23,0x62}, {0x23,0x63}, {0x33,0x63}, {0x33,0x63}, {0x13,0xB3}, {0x13,0xB3}, {0x1B,0xB9},
    {0x22,0xA2}, {0x0A,0xA2}, {0x0A,0xB2}, {0x23,0xB2}, {0x23,0xB3}, {0x0B,0xB3}, {0x1B,0xB3}, {0x1B,0xB3},
    {0x2A,0xA2}, {0x2A,0xA2}, {0x2A,0xB2}, {0x2B,0xB2}, {0x2B,0xB2}, {0x2B,0xB3}, {0x3B,0xB3}, {0x3B,0xB3},
    {0x2A,0xAA}, {0x2A,0xAA}, {0x2A,0xAA}, {0x2A,0xBA}, {0x2B,0xBA}, {0x2B,0xBB}, {0x3B,0xBB}, {0x3B,0xBB},
    {0x04,0x40}, {0x04,0x50}, {0x05,0x50}, {0x05,0x51}, {0x05,0x51}, {0x15,0x51}, {0x15,0x51}, {0x11,0xD1},
    {0x06,0x60}, {0x04,0x70}, {0x06,0x90}, {0x06,0x91}, {0x16,0x91}, {0x09,0xD1}, {0x19,0xD1}, {0x19,0xD1},
    {0x04,0x62}, {0x06,0x70}, {0x00,0xF0}, {0x01,0xF0}, {0x01,0xF1}, {0x09,0x99}, {0x19,0x99}, {0x19,0x99},
    {0x24,0x62}, {0x06,0x72}, {0x02,0xF0}, {0x03,0xF0}, {0x03,0xF1}, {0x09,0xB9}, {0x19,0xB9}, {0x19,0xB9},
    {0x02,0xE2}, {0x0A,0xA6}, {0x02,0xF2}, {0x03,0xF2}, {0x03,0xF3}, {0x0B,0xB9}, {0x1B,0xB9}, {0x1B,0xB9},
    {0x22,0xE2}, {0x2A,0xA6}, {0x0A,0xAA}, {0x0A,0xBA}, {0x0B,0xBA}, {0x0B,0xBB}, {0x1B,0xBB}, {0x1B,0xBB},
    {0x2A,0xE2}, {0x2A,0xE2}, {0x2A,0xF2}, {0x2B,0xF2}, {0x2B,0xF2}, {0x2B,0xF3}, {0x3B,0xF3}, {0x9B,0xBB},
    {0x2A,0xAA}, {0xAA,0xAA}, {0xAA,0xAA}, {0xAA,0xBA}, {0xAA,0xBA}, {0xAB,0xBA}, {0xAB,0xBB}, {0xBB,0xBB},
    {0x04,0x44}, {0x04,0x54}, {0x05,0x54}, {0x05,0x55}, {0x05,0x55}, {0x15,0x55}, {0x15,0xD1}, {0x15,0xD1},
    {0x06,0xC0}, {0x06,0xC0}, {0x06,0xD0}, {0x06,0xD1}, {0x16,0xD1}, {0x15,0xD1}, {0x1D,0xD1}, {0x1D,0xD1},
    {0x04,0xE0}, {0x04,0xE0}, {0x04,0xF0}, {0x05,0xF0}, {0x05,0xF1}, {0x09,0xD9}, {0x19,0xD9}, {0x19,0xD9},
    {0x04,0xE2}, {0x04,0xE2}, {0x07,0xE0}, {0x07,0xF0}, {0x07,0xF1}, {0x09,0xF9}, {0x19,0xF9}, {0x19,0xF9},
    {0x24,0xE2}, {0x24,0xE2}, {0x07,0xE2}, {0x07,0xF2}, {0x07,0xF3}, {0x0B,0xF9}, {0x1B,0xF9}, {0x1B,0xF9},
    {0x26,0xE2}, {0x26,0xE2}, {0x0A,0xEA}, {0x0A,0xFA}, {0x0B,0xFA}, {0x0B,0xFB}, {0x1B,0xFB}, {0x1B,0xFB},
    {0x2E,0xE2}, {0x6A,0xAA}, {0x6A,0xAA}, {0x6A,0xBA}, {0x6B,0xBA}, {0x6B,0xBB}, {0x6B,0xBB}, {0x9B,0xFB},
    {0x2A,0xEA}, {0x2A,0xEA}, {0xAA,0xEA}, {0xAA,0xFA}, {0xAA,0xFA}, {0xAB,0xFA}, {0xAB,0xFB}, {0xBB,0xFB},
    {0x04,0x44}, {0x04,0x54}, {0x05,0x54}, {0x05,0x55}, {0x05,0x55}, {0x15,0x55}, {0x15,0xD5}, {0x15,0xD5},
    {0x06,0xC4}, {0x06,0xC4}, {0x06,0xD4}, {0x06,0xD5}, {0x16,0xD5}, {0x15,0xD5}, {0x1D,0xD1}, {0x1D,0xD1},
    {0x04,0xE4}, {0x04,0xE4}, {0x04,0xF4}, {0x05,0xF4}, {0x05,0xF5}, {0x0D,0xD9}, {0x1D,0xD9}, {0x1D,0xD9},
    {0x06,0xE6}, {0x06,0xE6}, {0x07,0xE4}, {0x07,0xF4}, {0x07,0xF5}, {0x0D,0xF9}, {0x1D,0xF9}, {0x1D,0xF9},
    {0x26,0xE6}, {0x26,0xE6}, {0x0E,0xE8}, {0x07,0xE7}, {0x07,0xF7}, {0x0F,0xF9}, {0x1F,0xF9}, {0x1F,0xF9},
    {0x2A,0xE6}, {0x0E,0xEA}, {0x0E,0xEA}, {0x0E,0xFA}, {0x0F,0xFA}, {0x0F,0xFB}, {0x1F,0xFB}, {0x1F,0xFB},
    {0x2E,0xE2}, {0x6A,0xEA}, {0x6A,0xEA}, {0x6A,0xFA}, {0x6B,0xFA}, {0x6B,0xFB}, {0x6B,0xFB}, {0x9B,0xFB},
    {0x2E,0xEA}, {0x2E,0xEA}, {0xAA,0xEA}, {0xAA,0xFA}, {0xAA,0xFA}, {0xAB,0xFA}, {0xAB,0xFB}, {0xBB,0xFB},
    {0x44,0x44}, {0x44,0x54}, {0x45,0x54}, {0x45,0x55}, {0x45,0x55}, {0x55,0x55}, {0x55,0x55}, {0x55,0xD5},
    {0x46,0x64}, {0x0C,0xC4}, {0x46,0xD4}, {0x46,0xD5}, {0x56,0xD5}, {0x0D,0xD5}, {0x1D,0xD5}, {0x1D,0xD5},
    {0x46,0x66}, {0x0C,0xC6}, {0x0C,0xCC}, {0x0C,0xDC}, {0x0D,0xDC}, {0x0D,0xDD}, {0x1D,0xDD}, {0x1D,0xDD},
    {0x46,0xE6}, {0x0C,0xE6}, {0x0C,0xEC}, {0x0C,0xFC}, {0x0D,0xFC}, {0x0D,0xFD}, {0x1D,0xFD}, {0x1D,0xFD},
    {0x0E,0xE6}, {0x0E,0xE6}, {0x0E,0xEC}, {0x0E,0xFC}, {0x0F,0xFC}, {0x0F,0xFD}, {0x1F,0xFD}, {0x1F,0xFD},
    {0x2E,0xE6}, {0x2E,0xE6}, {0x0E,0xEE}, {0x0E,0xFE}, {0x0F,0xFE}, {0x0F,0xFF}, {0x1F,0xFF}, {0x1F,0xFF},
    {0x6E,0xEA}, {0x6E,0xEA}, {0x6E,0xEA}, {0x6E,0xFA}, {0x6F,0xFA}, {0x6F,0xFB}, {0x3F,0xF7}, {0x9F,0xFB},
    {0x2E,0xEE}, {0xAE,0xEA}, {0xAE,0xEA}, {0xAE,0xFA}, {0xAE,0xFA}, {0xAF,0xFA}, {0xAF,0xFB}, {0xBF,0xFB},
    {0x44,0xC4}, {0x44,0xC4}, {0x44,0xD4}, {0x45,0xD4}, {0x45,0xD5}, {0x55,0xD5}, {0x55,0xD5}, {0x55,0xD5},
    {0x4C,0xC4}, {0x4C,0xC4}, {0x4C,0xD4}, {0x4D,0xD4}, {0x4D,0xD4}, {0x4D,0xD5}, {0x5D,0xD5}, {0x5D,0xD5},
    {0x4C,0xC6}, {0x4C,0xC6}, {0x4C,0xD6}, {0x4D,0xD6}, {0x4D,0xD6}, {0x5D,0xD6}, {0x5D,0xF5}, {0x9D,0xDD},
    {0x4C,0xE6}, {0x4C,0xE6}, {0x4C,0xF6}, {0x4D,0xF6}, {0x4D,0xF6}, {0x5D,0xF6}, {0x5D,0xFD}, {0x5D,0xFD},
    {0x4E,0xE6}, {0x4E,0xE6}, {0x4E,0xF6}, {0x4F,0xF6}, {0x4F,0xF6}, {0x5F,0xF6}, {0x5F,0xFD}, {0x5F,0xFD},
    {0x6E,0xE6}, {0x6E,0xE6}, {0x6E,0xE7}, {0x6E,0xF7}, {0x6E,0xF7}, {0x6F,0xF7}, {0x5F,0xF7}, {0x9F,0xFD},
    {0x6E,0xEE}, {0x6E,0xEE}, {0x8E,0xEE}, {0x7E,0xE7}, {0x7E,0xE7}, {0x7E,0xF7}, {0x7F,0xF7}, {0x9F,0xFF},
    {0xAE,0xEE}, {0xAE,0xEE}, {0xAE,0xEE}, {0xAE,0xFE}, {0xAE,0xFE}, {0xAF,0xFE}, {0xAF,0xFF}, {0xBF,0xFF},
    {0x44,0xC4}, {0x44,0xC4}, {0x44,0xD4}, {0x45,0xD4}, {0x45,0xD5}, {0x55,0xD5}, {0x55,0xD5}, {0x5D,0xD5},
    {0x4C,0xC4}, {0x4C,0xC4}, {0x4C,0xD4}, {0x4C,0xDC}, {0x4D,0xDC}, {0x4D,0xD5}, {0x5D,0xD5}, {0x5D,0xDD},
    {0x4C,0xCC}, {0x4C,0xCC}, {0xCC,0xCC}, {0xCC,0xDC}, {0xCC,0xDC}, {0xCD,0xDC}, {0xCD,0xDD}, {0xDD,0xDD},
    {0x4C,0xEC}, {0x4C,0xEC}, {0x4C,0xEC}, {0x4C,0xFC}, {0x4D,0xFC}, {0x4D,0xFD}, {0x5D,0xFD}, {0x5D,0xFD},
    {0x4E,0xEC}, {0x4E,0xEC}, {0x4E,0xEC}, {0x4E,0xFC}, {0x4F,0xFC}, {0x4F,0xFD}, {0x5F,0xFD}, {0x5F,0xFD},
    {0x4E,0xEE}, {0x4E,0xEE}, {0xCE,0xEC}, {0xCE,0xFC}, {0xCE,0xFC}, {0xCF,0xFC}, {0xCF,0xFD}, {0xDF,0xFD},
    {0x6E,0xEE}, {0x6E,0xEE}, {0xCE,0xEE}, {0xCE,0xFE}, {0xCE,0xFE}, {0xCF,0xFE}, {0xCF,0xFF}, {0xDF,0xFF},
    {0xEE,0xEE}, {0xEE,0xEE}, {0xEE,0xEE}, {0xEE,0xFE}, {0xEE,0xFE}, {0xEF,0xFE}, {0xEF,0xFF}, {0xFF,0xFF},
};
static uint8_t digitalCga[8];
static uint8_t txt64near[64];

static uint8_t analog[16][3];
static uint8_t pal4096[16][3];
uint32_t mz_palette_gen = 1;
static bool use4096;
static const uint8_t pal256out[16][3] = {
    {   0,   0,   0 }, {   0,   0, 170 }, {   0, 170,   0 }, {   0, 170, 170 },
    { 170,   0,   0 }, { 170,   0, 170 }, { 170,  85,   0 }, { 170, 170, 170 },
    {  85,  85,  85 }, {  85,  85, 255 }, {  85, 255,  85 }, {  85, 255, 255 },
    { 255,  85,  85 }, { 255,  85, 255 }, { 255, 255,  85 }, { 255, 255, 255 },
};
static bool use256;

static const uint8_t *vram_b, *vram_r, *vram_g, *vram_i;
static void buildSpread();

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void updateHdisp() {
    const double cpc = (double)MZ_CYC_FRAME / MZ_LINES_FRAME / MZ_CHARS_LINE;
    if ((textreg[7] & 0x7F) < (textreg[8] & 0x7F)) {
        mz_hd_ts = (int)(cpc * (textreg[7] & 0x7F) + 0.5) - 2;
        mz_hd_te = (int)(cpc * (textreg[8] & 0x7F) + 0.5) + 6;
    } else mz_hd_ts = mz_hd_te = 0;
    if (GDEHS < GDEHE) {
        mz_hd_gs = (int)(cpc * (GDEHS + 10) + 0.5) - 2;
        mz_hd_ge = (int)(cpc * (GDEHE + 10) + 0.5) + 4;
    } else mz_hd_gs = mz_hd_ge = 0;
}
bool mzVideo4Color() { return (cgreg[0x0E] & 0x13) == 0x03; }

static int nearest16(int r, int g, int b) {
    int best = 0, bd = 1 << 30;
    for (int i = 0; i < 16; i++) {
        const int dr = r - pal256out[i][0], dg = g - pal256out[i][1], db = b - pal256out[i][2];
        const int d = dr * dr * 299 + dg * dg * 587 + db * db * 114;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static void tile2x2(int r, int g, int b, uint8_t t[2]) {
#if MZ_TILE256
    const uint8_t *d = dither512[((r >> 5) << 6) | ((g >> 5) << 3) | (b >> 5)];
    t[0] = d[0]; t[1] = d[1];
#else
    const uint8_t n = (uint8_t)nearest16(r, g, b);
    t[0] = t[1] = (uint8_t)((n << 4) | n);
#endif
}

static void build256(uint8_t reg0a) {
    for (int i = 0; i < 256; i++) {
        int r, g, b;
        if (reg0a == 0xFF) {
            b = ((i & 0x20) ? 128 : 0) | ((i & 2) ? 64 : 0) | ((i & 0x80) ? 32 : 0);
            r = ((i & 0x40) ? 128 : 0) | ((i & 4) ? 64 : 0) | ((i & 0x80) ? 32 : 0);
            g = ((i & 0x10) ? 128 : 0) | ((i & 1) ? 64 : 0) | ((i & 0x80) ? 32 : 0);
            const int t0 = b, t1 = r, t2 = g; r = t0; g = t1; b = t2;
        } else {
            const int b0 = reg0a & 3, r0 = (reg0a >> 2) & 3, g0 = (reg0a >> 4) & 3;
            b = ((i & 0x10) ? 128 : 0) | ((i & 1) ? 64 : 0) | (((b0 == 0 && (i & 0x80)) || (b0 == 1 && (i & 8)) || b0 == 2) ? 32 : 0);
            r = ((i & 0x20) ? 128 : 0) | ((i & 2) ? 64 : 0) | (((r0 == 0 && (i & 0x80)) || (r0 == 1 && (i & 8)) || r0 == 2) ? 32 : 0);
            g = ((i & 0x40) ? 128 : 0) | ((i & 4) ? 64 : 0) | (((g0 == 0 && (i & 0x80)) || (g0 == 1 && (i & 8)) || g0 == 2) ? 32 : 0);
        }
        b |= (b >> 3) | (b >> 6); r |= (r >> 3) | (r >> 6); g |= (g >> 3) | (g >> 6);
        pal256rgb[i][0] = (uint8_t)r; pal256rgb[i][1] = (uint8_t)g; pal256rgb[i][2] = (uint8_t)b;
        pal256near[i] = (uint8_t)nearest16(r, g, b);
        tile2x2(r, g, b, tile256[i]);
    }
    for (int i = 0; i < 64; i++) txt64near[i] = pal256near[((i & 0x38) << 1) | (i & 7)];
}

void mzVideoPalette(uint8_t rgb[16][3]) { memcpy(rgb, use256 ? pal256out : use4096 ? pal4096 : analog, sizeof(analog)); }

void mzVideoInit() {
    for (int i = 0; i < 16; i++) {
        if (i == 8) { analog[i][0] = analog[i][1] = analog[i][2] = 152; continue; }
        analog[i][0] = ((i & 0x0A) == 0x0A) ? 255 : ((i & 0x0A) == 2) ? 127 : 0;
        analog[i][1] = ((i & 0x0C) == 0x0C) ? 255 : ((i & 0x0C) == 4) ? 127 : 0;
        analog[i][2] = ((i & 0x09) == 0x09) ? 255 : ((i & 0x09) == 1) ? 127 : 0;
    }
    memcpy(pal4096, analog, sizeof(analog));
    for (int c = 0; c < 8; c++) digitalCga[c] = (uint8_t)(c ? nearest16((c & 2) ? 255 : 0, (c & 4) ? 255 : 0, (c & 1) ? 255 : 0) : 0);
    vram_b = M.vram + MZ_VPLANE(0); vram_r = M.vram + MZ_VPLANE(1); vram_g = M.vram + MZ_VPLANE(2); vram_i = M.vram + MZ_VPLANE(3);
    build256(0xFF);
    buildSpread();
}

void mzVideoReset() {
    memset(textreg, 0, sizeof(textreg));
    memset(cgreg, 0, sizeof(cgreg));
    textreg[0x03] = 17; textreg[0x05] = textreg[0x03] + 200;
    textreg[0x07] = 9;  textreg[0x08] = textreg[0x07] + 80;
    cgreg_num = 0x80;
    cgreg[0x00] = cgreg[0x01] = cgreg[0x02] = cgreg[0x03] = cgreg[0x06] = 0xFF;
    GDEVS = 0; GDEVE = 400; cgreg[0x0A] = GDEVE & 0xFF; cgreg[0x0B] = GDEVE >> 8;
    GDEHS = 0; GDEHE = 80;  cgreg[0x0D] = GDEHE;
    for (int i = 0; i < 16; i++) palette_reg[i] = (uint8_t)i;
    font_size = true; column80 = false;
    cg_mask = 0x0F; cg_mask256 = 0; cg_mask256_init = true;
    clear_flag = 0;
    pal_select = screen_reverse = screen_mask = false;
    latch[0] = latch[1] = latch[2] = latch[3] = 0;
    mz_vblank_t = mz_vblank_g = true;
    build256(0xFF);
    updateHdisp();
    mz_vram_gen++; mz_text_gen++; mz_cg_gen++; layout_gen++; color_gen++; cgl_gen++;
}

void mzVideoSignal(int id, bool on) {
    const bool cur = id == MZV_COLUMN80 ? column80 : id == MZV_PAL4096 ? pal_select : id == MZV_REVERSE ? screen_reverse : screen_mask;
    if (cur == on) return;
    switch (id) {
        case MZV_COLUMN80: column80 = on; break;
        case MZV_PAL4096:  pal_select = on; break;
        case MZV_REVERSE:  screen_reverse = on; break;
        case MZV_MASK:     screen_mask = on; break;
    }
    mz_vram_gen++; mz_text_gen++; layout_gen++;
}

void MZ_FAST mzVideoVline(int v) {
    if (v == textreg[5] * 2) mz_vblank_t = true;
    else if (v == textreg[3] * 2) mz_vblank_t = false;
    if (v == GDEVE) { mz_vblank_g = true; clear_flag = 0; }
    else if (v == GDEVS) mz_vblank_g = false;
}

static inline bool rmwPlane(uint8_t *p, uint8_t mode, bool on, uint8_t col, uint8_t data) {
    const uint8_t o = *p;
    uint8_t v;
    if (mode == 0x00) v = (uint8_t)((o & ~cgreg[6]) | (on ? (data & col & cgreg[6]) : 0));
    else if (mode == 0x40) v = (uint8_t)((o & ~data) | (on ? (data & col) : 0));
    else return false;
    if (v == o) return false;
    *p = v;
    return true;
}
void MZ_FAST mzVideoRmwWrite(uint16_t addr, uint8_t data) {
    uint8_t *vb = M.vram + MZ_VPLANE(0), *vr = M.vram + MZ_VPLANE(1), *vg = M.vram + MZ_VPLANE(2), *vi = M.vram + MZ_VPLANE(3);
    const uint8_t mode = cgreg[5] & 0xC0;
    bool changed = false;
    if (cgreg[0x0E] == 0x03) {
        uint8_t *p0, *p1;
        if (addr & 0x4000) { p0 = vg; p1 = vi; } else { p0 = vb; p1 = vr; }
        addr &= 0x3FFF;
        if (cgreg[5] & 1) changed |= rmwPlane(p0 + addr, mode, (cgreg[4] & 1) != 0, cgreg[0], data);
        if (cgreg[5] & 2) changed |= rmwPlane(p1 + addr, mode, (cgreg[4] & 2) != 0, cgreg[1], data);
    } else {
        addr &= 0x7FFF;
        uint8_t *pl[4] = { vb, vr, vg, vi };
        for (int k = 0; k < 4; k++)
            if (cgreg[5] & (1 << k)) changed |= rmwPlane(pl[k] + addr, mode, (cgreg[4] & (1 << k)) != 0, cgreg[k], data);
    }
    if (changed) mzMarkVram(addr);
}

static uint8_t compareResult(uint8_t b, uint8_t r, uint8_t g, uint8_t i) {
    const uint8_t cmp = cgreg[7] & 0x0F;
    uint8_t val = 0;
    for (int k = 0; k < 8; k++) {
        const uint8_t c = (uint8_t)(((b >> k) & 1) | (((r >> k) & 1) << 1) | (((g >> k) & 1) << 2) | (((i >> k) & 1) << 3));
        if (c == cmp) val |= (uint8_t)(1 << k);
    }
    return val;
}

void MZ_FAST mzVideoWriteCgData(uint8_t data) {
    const int n = cgreg_num & 0x1F;
    if (n < 0x08 && !(n == 0x05 && (data & 0xC0) == 0x80)) {
        cgreg[n] = data;
        if (cgreg_num & 0x80) cgreg_num = (uint8_t)((cgreg_num & 0xFC) | ((cgreg_num + 1) & 0x03));
        return;
    }
    mzVideoWriteIO(0xBD, data);
}

uint8_t MZ_FAST mzVideoRmwRead(uint16_t addr) {
    uint8_t b, r, g, i, pl;
    if (cgreg[0x0E] == 0x03) {
        b = latch[0] = (addr & 0x4000) ? vram_g[addr & 0x3FFF] : vram_b[addr & 0x3FFF];
        r = latch[1] = (addr & 0x4000) ? vram_i[addr & 0x3FFF] : vram_r[addr & 0x3FFF];
        g = latch[2] = 0; i = latch[3] = 0;
        pl = cgreg[7] & 1;
    } else {
        addr &= 0x7FFF;
        b = latch[0] = vram_b[addr]; r = latch[1] = vram_r[addr];
        g = latch[2] = vram_g[addr]; i = latch[3] = vram_i[addr];
        pl = cgreg[7] & 3;
    }
#if MZ_LATCH_COPY
    if (mz_latch_copy) {
        cgreg[0] = latch[0]; cgreg[1] = latch[1]; cgreg[2] = latch[2]; cgreg[3] = latch[3];
    }
#endif
    if (cgreg[7] & 0x10) return compareResult(b, r, g, i);
#if MZ_LATCH_COPY
    if (mz_latch_copy) return (uint8_t)(b | r | g | i);
#endif
    return latch[pl];
}

#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
void mzVideoWriteIO(uint16_t port, uint8_t data) {
    switch (port & 0xFF) {
        case 0xAE: {
            const int h = port >> 8, num = (h & 0x1F) >> 1;
            uint8_t r = pal4096[num][0], g = pal4096[num][1], b = pal4096[num][2];
            if (h & 1) g = (uint8_t)((data & 0x0F) << 4);
            else { r = (uint8_t)(data & 0xF0); b = (uint8_t)((data & 0x0F) << 4); }
            r |= r >> 4; g |= g >> 4; b |= b >> 4;
            if (h & 1) { if (pal4096[num][1] != g) { pal4096[num][1] = g; color_gen++; if (use4096) mz_palette_gen++; } }
            else if (pal4096[num][0] != r || pal4096[num][2] != b) { pal4096[num][0] = r; pal4096[num][2] = b; color_gen++; if (use4096) mz_palette_gen++; }
            break;
        }
        case 0xBC: cgreg_num = data; break;
        case 0xBD: {
            const int n = cgreg_num & 0x1F;
            const uint8_t prev = cgreg[n], prev09 = cgreg[0x09], prev0B = cgreg[0x0B];
            cgreg[n] = data;
            switch (n) {
                case 0x05:
                    if ((data & 0xC0) == 0x80) {
                        uint16_t st = 0, sz = 0x8000;
                        switch (cgreg[0x0E]) {
                            case 0x03: case 0x14: case 0x15: case 0x17: case 0x1D: st = 0x0000; sz = 0x4000; break;
                            case 0x94: case 0x95: case 0x97: case 0x9D: st = 0x4000; sz = 0x4000; break;
                        }
                        for (int k = 0; k < 4; k++) if (data & (1 << k)) memset(M.vram + MZ_VPLANE(k) + st, 0, sz);
                        clear_flag = 1;
                        for (uint32_t o = st; o < (uint32_t)st + sz; o += 32) mzMarkVram((uint16_t)o);
                    }
                    break;
                case 0x08: cgreg[0x09] = 0;
                case 0x09: GDEVS = (uint16_t)(cgreg[0x08] | ((cgreg[0x09] & 1) << 8)); break;
                case 0x0A: cgreg[0x0B] = 0;
                case 0x0B: GDEVE = (uint16_t)(cgreg[0x0A] | ((cgreg[0x0B] & 1) << 8)); break;
                case 0x0C: GDEHS = cgreg[0x0C] & 0x7F; updateHdisp(); break;
                case 0x0D: GDEHE = cgreg[0x0D] & 0x7F; updateHdisp(); break;
                case 0x0E: if ((prev & 0x10) != (data & 0x10)) mzRefreshMap(); break;
                case 0x18: cg_mask256_init = true; break;
            }
            if (n >= 0x08 && (prev != data || cgreg[0x09] != prev09 || cgreg[0x0B] != prev0B)) { mz_vram_gen++; mz_cg_gen++; layout_gen++; cgl_gen++; }
            if (cgreg_num & 0x80) cgreg_num = (uint8_t)((cgreg_num & 0xFC) | ((cgreg_num + 1) & 0x03));
            break;
        }
        case 0xF4: textreg_num = data; break;
        case 0xF5:
            if (textreg_num < 0x10) {
                if (textreg_num == 0x0A && (textreg[0x0A] & 0x3F) != (data & 0x3F)) build256(data & 0x3F);
                textreg[textreg_num] = data;
                if (textreg_num == 7 || textreg_num == 8) updateHdisp();
                layout_gen++;
            } else if (textreg_num >= 0x80 && textreg_num < 0x90) {
                palette_reg[textreg_num & 0x0F] = data;
                color_gen++;
            }
            mz_vram_gen++; mz_text_gen++;
            break;
        case 0xF6: cg_mask = (uint8_t)((data & 7) | ((data & 1) ? 8 : 0)); cg_mask256_init = true; mz_vram_gen++; layout_gen++; break;
        case 0xF7: font_size = (data & 1) != 0; mz_vram_gen++; mz_text_gen++; layout_gen++; break;
    }
}

uint8_t mzVideoReadIO(uint16_t port) {
    switch (port & 0xFF) {
        case 0xBC: return (cgreg[7] & 0x10) ? compareResult(latch[0], latch[1], latch[2], latch[3]) : latch[0];
        case 0xBD: return (cgreg[7] & 0x10) ? (uint8_t)((mz_vblank_g ? 0 : 0x80) | clear_flag) : latch[1];
        case 0xBE: return latch[2];
        case 0xBF: return latch[3];
        case 0xF4: case 0xF5: case 0xF6: case 0xF7: {
            const int pos = mzLineCycle();
            const bool hblank_t = !(pos >= mz_hd_ts && pos < mz_hd_te);
            return (uint8_t)((mz_vblank_t ? 0 : 1) | (hblank_t ? 0 : 2));
        }
    }
    return 0xFF;
}

static int copyY = -1;
void mzVideoInvalidate() { mz_vram_gen++; layout_gen++; cgl_gen++; copyY = -1; }

void mzVideoDebugDump(char *buf, int n) {
    int k = snprintf(buf, n, "text:");
    for (int i = 0; i < 16; i++) k += snprintf(buf + k, n - k, " %02X", textreg[i]);
    k += snprintf(buf + k, n - k, " | cg:");
    for (int i = 0; i < 0x19; i++) k += snprintf(buf + k, n - k, " %02X", cgreg[i]);
    k += snprintf(buf + k, n - k, " | pal:");
    for (int i = 0; i < 16; i++) k += snprintf(buf + k, n - k, " %02X", palette_reg[i]);
    snprintf(buf + k, n - k, " | mask=%d col80=%d font=%d cgmask=%X pal4096=%d", screen_mask, column80, font_size, cg_mask, pal_select);
}

static bool blinkOn() { return ((mz_frame_count / 28) & 1) != 0; }
bool mzVideoBlinkPhase() { return blinkOn(); }

static void cellLine(uint16_t src, int i, uint8_t trans, uint8_t *d8) {
    const uint8_t t1 = M.tvram[src], t2 = M.tvram[0x1000 + src], attr = M.tvram[0x800 + src];
    const uint8_t sel = (uint8_t)((t2 & 0xC0) | (attr & 0x38));
    const bool blink = (attr & 0x80) && blinkOn();
    const uint8_t *pcg0 = M.pcg, *pcg1 = M.pcg + 0x800, *pcg2 = M.pcg + 0x1000, *pcg3 = M.pcg + 0x1800;
    if (sel & 0x08) {
        if (blink) { memset(d8, (attr & 0x40) ? 7 : trans, 8); return; }
        const uint32_t code = font_size ? (uint32_t)t1 << 3 : (uint32_t)(t1 & 0xFE) << 3;
        uint8_t p1 = pcg1[code + i], p2 = pcg2[code + i], p3 = pcg3[code + i];
        if (attr & 0x40) { p1 = ~p1; p2 = ~p2; p3 = ~p3; }
        for (int x = 0; x < 8; x++) {
            const int s = 7 - x;
            const uint8_t col = (uint8_t)(((p1 >> s) & 1) | (((p2 >> s) & 1) << 1) | (((p3 >> s) & 1) << 2));
            d8[x] = col ? col : trans;
        }
        return;
    }
    const uint8_t *pat;
    switch (sel) {
        case 0x00: case 0x40: pat = pcg0; break;
        case 0x80: pat = M.kanji; break;
        case 0xC0: pat = M.kanji + 0x20000; break;
        default:
            pat = ((sel & 0x30) == 0x10) ? pcg1 : ((sel & 0x30) == 0x20) ? pcg2 : pcg3;
            break;
    }
    uint32_t code;
    const bool kanji = (sel == 0x80 || sel == 0xC0);
    if (font_size) code = kanji ? (((uint32_t)(t2 & 0x3F) << 11) | ((uint32_t)t1 << 3)) : ((uint32_t)t1 << 3);
    else code = kanji ? (((uint32_t)(t2 & 0x3F) << 11) | ((uint32_t)(t1 & 0xFE) << 3)) : ((uint32_t)(t1 & 0xFE) << 3);
    const uint8_t col = attr & 7;
    const uint8_t fore = col ? col : 8;
    uint8_t p;
    if (attr & 0x40) p = blink ? 0xFF : (uint8_t)~pat[code + i];
    else p = blink ? 0x00 : pat[code + i];
    for (int x = 0; x < 8; x++) d8[x] = ((p >> (7 - x)) & 1) ? fore : trans;
}

static void textRow(int r, uint8_t *t) {
    memset(t, 0, MZ_SCREEN_W);
    const uint8_t trans = (textreg[0] & 2) ? 8 : 0;
    const int line = (textreg[0] & 0x10) ? 2 : 0;
    const int height = (textreg[0] & 0x10) ? 20 : 16;
    const int vd = (textreg[9] & 0x0F) << 1;
    const uint16_t start = (uint16_t)(textreg[1] | ((textreg[2] & 0x07) << 8));
    const int ty = r + vd;
    bool drawn = false;
    if (ty >= line) {
        const int c = (ty - line) / height, l = (ty - line) % height;
        if (line + c * height < 416 && l < 16) {
            const int i = font_size ? (l >> 1) : l;
            drawn = true;
            if (column80) {
                for (int x = 0; x < 80; x++) cellLine((uint16_t)((start + c * 80 + x) & 0x7FF), i, trans, t + x * 8);
            } else {
                uint8_t a[8], b[8];
                const int mode = textreg[0] & 0x0C;
                for (int x = 0; x < 40; x++) {
                    const uint16_t s1 = (uint16_t)((start + c * 40 + x) & 0x7FF), s2 = (uint16_t)((start + 0x400 + c * 40 + x) & 0x7FF);
                    uint8_t *d = t + x * 16;
                    if (mode == 0x04) { cellLine(s1, i, trans, a); for (int k = 0; k < 8; k++) d[2 * k] = d[2 * k + 1] = a[k]; }
                    else if (mode == 0x08) { cellLine(s2, i, trans, a); for (int k = 0; k < 8; k++) d[2 * k] = d[2 * k + 1] = a[k]; }
                    else {
                        cellLine(s1, i, trans, a);
                        cellLine(s2, i, trans, b);
                        for (int k = 0; k < 8; k++) {
                            uint8_t v;
                            if (mode == 0x00) {
                                v = (uint8_t)((((a[k] & 7) << 3) | (b[k] & 7)) + 16);
                                if (v == 16 && (a[k] & 8) && (b[k] & 8)) v = 8;
                            } else {
                                v = (a[k] & 7) ? a[k] : (b[k] & 7) ? b[k] : (uint8_t)((a[k] & 8) | (b[k] & 8));
                            }
                            d[2 * k] = d[2 * k + 1] = v;
                        }
                    }
                }
            }
        }
    }
    const bool to64 = column80 ? !(textreg[0] & 1) : ((textreg[0] & 0x0C) != 0 && !(textreg[0] & 1));
    if (to64 && r + vd >= line && drawn)
        for (int x = 0; x < MZ_SCREEN_W; x++) if (!(t[x] & 8)) t[x] = (uint8_t)(((t[x] & 7) << 3) + 16);
    int SL = clampi((textreg[3] - 17) << 1, 0, 400), EL = clampi((textreg[5] - 17) << 1, 0, 400);
    const int off = column80 ? 9 : 8;
    int SC = clampi((textreg[7] & 0x7F) - off, 0, 80), EC = clampi((textreg[8] & 0x7F) - off, 0, 80);
    bool outV = (EL >= SL) ? (r < SL || r >= EL) : (r >= EL && r < SL);
    if (outV) { memset(t, trans, MZ_SCREEN_W); return; }
    if (EC >= SC) { memset(t, trans, SC << 3); memset(t + (EC << 3), trans, (80 - EC) << 3); }
    else memset(t + (EC << 3), trans, (SC - EC) << 3);
}

static uint16_t addrStep(uint16_t a, uint16_t sad1) { return (a == sad1) ? 0 : (uint16_t)((a + 1) & 0x7FFF); }
static uint16_t addrAdvance(uint16_t a, uint32_t k, uint16_t sad1) {
    if (a > sad1) {
        const uint32_t toWrap = 0x8000u - a;
        if (k < toWrap) return (uint16_t)(a + k);
        k -= toWrap; a = 0;
    }
    const uint32_t period = (uint32_t)sad1 + 1;
    const uint32_t pos = (uint32_t)a + k;
    if (pos <= sad1) return (uint16_t)pos;
    return (uint16_t)((pos - period) % period);
}
static void rowAddr(int y, int xmax, uint16_t *addr, int *hdsc) {
    uint8_t HD = cgreg[0x0F] & 7;
    const uint16_t SAD0 = (uint16_t)(cgreg[0x10] | ((cgreg[0x11] & 0x7F) << 8));
    const uint16_t SAD1 = (uint16_t)(cgreg[0x12] | ((cgreg[0x13] & 0x7F) << 8));
    const uint16_t SAD2 = (uint16_t)(cgreg[0x14] | ((cgreg[0x15] & 0x7F) << 8));
    const int SLN1 = cgreg[0x16] | ((cgreg[0x17] & 1) << 8);
    if (!(cgreg[0x0E] & 0x02)) HD <<= 1;
    uint16_t a;
    if (y < SLN1) { a = addrAdvance(SAD0, (uint32_t)y * xmax, SAD1); *hdsc = HD; }
    else { a = addrAdvance(SAD2, (uint32_t)(y - SLN1) * xmax, SAD1); *hdsc = 0; }
    for (int x = 0; x < xmax; x++) { addr[x] = a; a = addrStep(a, SAD1); }
}

static inline uint8_t pix16(uint8_t B, uint8_t R, uint8_t G, uint8_t I, int k) {
    return (uint8_t)(((B >> k) & 1) | (((R >> k) & 1) << 1) | (((G >> k) & 1) << 2) | (((I >> k) & 1) << 3));
}

static void cg320x16(int y, int pl, uint8_t *cg) {
    uint16_t addr[40]; int hd;
    rowAddr(y, 40, addr, &hd);
    const uint16_t ex = (uint16_t)((cgreg[0x0E] & 0x80) << 7);
    const uint16_t sub = (pl & 1) ? 0x2000 : 0x0000;
    const uint8_t m = (pl == 0 || pl == 2) ? (cgreg[0x18] & 0x0F) : (cgreg[0x18] >> 4);
    for (int x = 0; x < 40; x++) {
        const uint16_t src = (uint16_t)((addr[x] ^ sub) | ex) & 0x7FFF;
        const uint8_t B = (m & 1) ? vram_b[src] : 0, R = (m & 2) ? vram_r[src] : 0, G = (m & 4) ? vram_g[src] : 0, I = (m & 8) ? vram_i[src] : 0;
        for (int k = 0; k < 8; k++) {
            const uint8_t c = pix16(B, R, G, I, k);
            const int dx = x * 16 + k * 2 + hd;
            if (c && dx < MZ_SCREEN_W) { cg[dx] = c; if (dx + 1 < MZ_SCREEN_W) cg[dx + 1] = c; }
        }
    }
}
static void cg640x16(int y, int pl, uint8_t *cg) {
    uint16_t addr[80]; int hd;
    rowAddr(y, 80, addr, &hd);
    const uint16_t ex = (uint16_t)((cgreg[0x0E] & 0x80) << 7);
    const uint16_t sub = (pl & 1) ? 0x4000 : 0x0000;
    const uint8_t m = cgreg[0x18];
    for (int x = 0; x < 80; x++) {
        const uint16_t src = (uint16_t)((addr[x] ^ sub) | ex) & 0x7FFF;
        const uint8_t B = (m & 1) ? vram_b[src] : 0, R = (m & 2) ? vram_r[src] : 0, G = (m & 4) ? vram_g[src] : 0, I = (m & 8) ? vram_i[src] : 0;
        for (int k = 0; k < 8; k++) { const int dx = x * 8 + k + hd; if (dx < MZ_SCREEN_W) cg[dx] = pix16(B, R, G, I, k); }
    }
}
static void cg640x400x4(int y, uint8_t *cg) {
    uint16_t addr[80]; int hd;
    rowAddr(y, 80, addr, &hd);
    for (int x = 0; x < 80; x++) {
        const uint16_t src = addr[x];
        const uint8_t B = (cgreg[0x18] & 1) ? ((src & 0x4000) ? vram_g[src & 0x3FFF] : vram_b[src & 0x3FFF]) : 0;
        const uint8_t R = (cgreg[0x18] & 2) ? ((src & 0x4000) ? vram_i[src & 0x3FFF] : vram_r[src & 0x3FFF]) : 0;
        for (int k = 0; k < 8; k++) { const int dx = x * 8 + k + hd; if (dx < MZ_SCREEN_W) cg[dx] = pix16(B, R, 0, 0, k); }
    }
}
static void cg640x400x16(int y, uint8_t *cg) {
    uint16_t addr[80]; int hd;
    rowAddr(y, 80, addr, &hd);
    for (int x = 0; x < 80; x++) {
        const uint16_t src = addr[x] & 0x7FFF;
        const uint8_t B = vram_b[src], R = vram_r[src], G = vram_g[src], I = vram_i[src];
        for (int k = 0; k < 8; k++) { const int dx = x * 8 + k + hd; if (dx < MZ_SCREEN_W) cg[dx] = pix16(B, R, G, I, k); }
    }
}
static void cg320x256(int y, uint8_t *cg) {
    if (cg_mask256_init) {
        cg_mask256 = cgreg[0x18];
        if (!(cg_mask & 1)) cg_mask256 &= ~(0x11 | 0x88);
        if (!(cg_mask & 2)) cg_mask256 &= ~0x22;
        if (!(cg_mask & 4)) cg_mask256 &= ~0x44;
        cg_mask256_init = false;
    }
    uint16_t addr[40]; int hd;
    rowAddr(y, 40, addr, &hd);
    const uint16_t ex = (uint16_t)((cgreg[0x0E] & 0x80) << 7);
    const uint8_t m = cg_mask256;
    for (int x = 0; x < 40; x++) {
        const uint16_t s1 = (uint16_t)(addr[x] | ex) & 0x7FFF, s2 = (uint16_t)((addr[x] ^ 0x2000) | ex) & 0x7FFF;
        const uint8_t B1 = (m & 0x01) ? vram_b[s1] : 0, B0 = (m & 0x10) ? vram_b[s2] : 0;
        const uint8_t R1 = (m & 0x02) ? vram_r[s1] : 0, R0 = (m & 0x20) ? vram_r[s2] : 0;
        const uint8_t G1 = (m & 0x04) ? vram_g[s1] : 0, G0 = (m & 0x40) ? vram_g[s2] : 0;
        const uint8_t I1 = (m & 0x08) ? vram_i[s1] : 0, I0 = (m & 0x80) ? vram_i[s2] : 0;
        for (int k = 0; k < 8; k++) {
            const uint8_t c = (uint8_t)(pix16(B0, R0, G0, I0, k) | (pix16(B1, R1, G1, I1, k) << 4));
            const int dx = x * 16 + k * 2 + hd;
            if (dx < MZ_SCREEN_W) { cg[dx] = c; if (dx + 1 < MZ_SCREEN_W) cg[dx + 1] = c; }
        }
    }
}

static bool cgRow(int r, uint8_t *cg) {
    memset(cg, 0, MZ_SCREEN_W);
    switch (cgreg[0x0E]) {
        case 0x03: cg640x400x4(r, cg); return false;
        case 0x93: cg640x400x16(r, cg); return false;
        case 0x14: cg320x16(r >> 1, 0, cg); cg320x16(r >> 1, 1, cg); return false;
        case 0x15: cg320x16(r >> 1, 1, cg); cg320x16(r >> 1, 0, cg); return false;
        case 0x94: cg320x16(r >> 1, 2, cg); cg320x16(r >> 1, 3, cg); return false;
        case 0x95: cg320x16(r >> 1, 3, cg); cg320x16(r >> 1, 2, cg); return false;
        case 0x17: cg640x16(r >> 1, 0, cg); return false;
        case 0x97: cg640x16(r >> 1, 1, cg); return false;
        case 0x1D: case 0x9D: cg320x256(r >> 1, cg); return true;
        case 0x19: case 0x99: cg320x256(r, cg); return true;
    }
    return (cgreg[0x0E] == 0x1D || cgreg[0x0E] == 0x9D || cgreg[0x0E] == 0x19 || cgreg[0x0E] == 0x99);
}

static inline uint8_t digital(int c) { return c ? (uint8_t)(c | 8) : 0; }

static void renderGeneric(int y, uint8_t *dst) {
    static uint8_t txt[MZ_SCREEN_W], cg[MZ_SCREEN_W], out[MZ_SCREEN_W];
    textRow(y, txt);
    const bool is256 = cgRow(y, cg);
    const bool row400 = (cgreg[0x0E] == 0x03 || cgreg[0x0E] == 0x93 || cgreg[0x0E] == 0x19 || cgreg[0x0E] == 0x99);
    const int vs = (GDEVS <= GDEVE) ? GDEVS * (row400 ? 1 : 2) : 0;
    const int ve = (GDEVS <= GDEVE) ? GDEVE * (row400 ? 1 : 2) : 400;
    const int hs = (GDEHS <= GDEHE && GDEHS < 80) ? (GDEHS << 3) : 0;
    const int he = (GDEHS <= GDEHE && GDEHE < 80) ? (GDEHE << 3) : 640;
    const bool inV = (y >= vs && y < ve);
    const uint8_t back16 = (uint8_t)(((textreg[0x0B] & 4) >> 2) | ((textreg[0x0B] & 0x20) >> 4) | ((textreg[0x0C] & 1) << 2) | ((textreg[0x0B] & 1) << 3));
    const bool want4096 = pal_select && !is256;
    if (want4096 != use4096) { use4096 = want4096; mz_palette_gen++; }
    if (is256 != use256) { use256 = is256; mz_palette_gen++; }
    if (!is256) {
        uint8_t cgcol[16]; bool front[16];
        for (int c = 0; c < 16; c++) {
            cgcol[c] = (palette_reg[c] & 0x0F) ? (uint8_t)(palette_reg[c] & cg_mask) : (uint8_t)(back16 & cg_mask);
            front[c] = (palette_reg[c] & 0x10) != 0;
        }
        uint8_t txtc[9], txtOut[9];
        const uint8_t bgTxt = palette_reg[back16] & 0x0F;
        if (use4096) {
            int dark = 0, dv = 1 << 30;
            for (int i = 0; i < 16; i++) { const int v = pal4096[i][0] + pal4096[i][1] + pal4096[i][2]; if (v < dv) { dv = v; dark = i; } }
            txtc[0] = 0;
            for (int c = 1; c < 8; c++) txtc[c] = cgcol[c + 8];
            txtc[8] = cgcol[0];
            memcpy(txtOut, txtc, sizeof(txtc));
            txtOut[8] = (uint8_t)dark;
        } else {
            txtc[0] = 0;
            for (int c = 1; c < 8; c++) txtc[c] = digital(c);
            txtc[8] = 0;
            memcpy(txtOut, txtc, sizeof(txtc));
        }
        txtOut[0] = bgTxt;
        for (int x = 0; x < MZ_SCREEN_W; x++) {
            uint8_t t = txt[x];
            if (t >= 16) { const int c64 = t - 16; t = c64 ? (uint8_t)((c64 >> 3) ? (c64 >> 3) : (c64 & 7)) : 0; }
            if (inV && x >= hs && x < he) {
                const int c = cg[x] & 0x0F;
                out[x] = (t == 0 || front[c]) ? cgcol[c] : txtc[t];
            } else {
                out[x] = txtOut[t];
            }
        }
    } else {
        const int bb = ((textreg[0x0B] & 7) << 5), br = ((textreg[0x0B] & 0x38) << 2), bg = (((textreg[0x0B] & 0xC0) >> 1) | ((textreg[0x0C] & 1) << 7));
        uint8_t backT[2];
        tile2x2(br, bg, bb, backT);
        const int yl = y & 1;
        for (int x = 0; x < MZ_SCREEN_W; x++) {
            const uint8_t t = txt[x];
            const int sh = (x & 1) ? 0 : 4;
            const uint8_t back256 = (uint8_t)((backT[yl] >> sh) & 15);
            uint8_t tc;
            if (t == 0 || t == 16) tc = 255;
            else if (t == 8) tc = 0;
            else if (t < 8) tc = digitalCga[t];
            else tc = txt64near[(t - 16) & 63];
            if (inV && x >= hs && x < he) {
                const uint8_t c = cg[x];
                const bool fr = (palette_reg[c >> 4] & 0x10) != 0;
                const uint8_t cc = (uint8_t)(((palette_reg[c >> 4] & 0x0F) << 4) | (c & 0x0F));
                const uint8_t cgOut = (cc == 0) ? back256 : (uint8_t)((tile256[cc][yl] >> sh) & 15);
                out[x] = (tc == 255 || fr) ? cgOut : tc;
            } else {
                out[x] = (tc == 255) ? back256 : tc;
            }
        }
    }
    for (int x = 0; x < MZ_SCREEN_W / 2; x++) dst[x] = (uint8_t)((out[2 * x] << 4) | out[2 * x + 1]);
}

static uint32_t spreadL[256];
static uint32_t spreadT[256];
static uint32_t spreadD[16];
static uint32_t spreadTD[16];
static void buildSpread() {
    for (int v = 0; v < 256; v++) {
        uint32_t l = 0, t = 0;
        for (int k = 0; k < 8; k++) {
            const int sh = (k >> 1) * 8 + ((k & 1) ? 0 : 4);
            if (v & (1 << k)) l |= 1u << sh;
            if (v & (0x80 >> k)) t |= 1u << sh;
        }
        spreadL[v] = l; spreadT[v] = t;
    }
    for (int v = 0; v < 16; v++) {
        uint32_t d = 0, t = 0;
        for (int k = 0; k < 4; k++) {
            if (v & (1 << k)) d |= 0x11u << (8 * k);
            if (v & (8 >> k)) t |= 0x11u << (8 * k);
        }
        spreadD[v] = d; spreadTD[v] = t;
    }
}
static inline uint32_t fill4(uint32_t v) { return v * 0x11111111u; }
static inline uint32_t nz4(uint32_t w) { return ((w | (w >> 1) | (w >> 2) | (w >> 3)) & 0x11111111u) * 15u; }
static inline uint32_t lut4(const uint8_t *t, uint32_t w) {
    return (uint32_t)t[w & 0xFF] | ((uint32_t)t[(w >> 8) & 0xFF] << 8) | ((uint32_t)t[(w >> 16) & 0xFF] << 16) | ((uint32_t)t[w >> 24] << 24);
}

static void MZ_FAST cellWords(uint16_t src, int i, uint8_t trans, bool dbl, uint32_t *w) {
    const uint8_t t1 = M.tvram[src], t2 = M.tvram[0x1000 + src], attr = M.tvram[0x800 + src];
    const uint8_t sel = (uint8_t)((t2 & 0xC0) | (attr & 0x38));
    const bool blink = (attr & 0x80) && blinkOn();
    if (sel & 0x08) {
        if (blink) { w[0] = fill4((attr & 0x40) ? 7 : trans); if (dbl) w[1] = w[0]; return; }
        const uint32_t code = font_size ? (uint32_t)t1 << 3 : (uint32_t)(t1 & 0xFE) << 3;
        uint8_t p1 = M.pcg[0x800 + code + i], p2 = M.pcg[0x1000 + code + i], p3 = M.pcg[0x1800 + code + i];
        if (attr & 0x40) { p1 = ~p1; p2 = ~p2; p3 = ~p3; }
        const uint32_t tr = trans ? 0x88888888u : 0;
        const uint8_t any = (uint8_t)(p1 | p2 | p3);
        if (!dbl) {
            w[0] = spreadT[p1] | (spreadT[p2] << 1) | (spreadT[p3] << 2) | (tr & ~(spreadT[any] * 15u));
        } else {
            w[0] = spreadTD[p1 >> 4] | (spreadTD[p2 >> 4] << 1) | (spreadTD[p3 >> 4] << 2) | (tr & ~(spreadTD[any >> 4] * 15u));
            w[1] = spreadTD[p1 & 15] | (spreadTD[p2 & 15] << 1) | (spreadTD[p3 & 15] << 2) | (tr & ~(spreadTD[any & 15] * 15u));
        }
        return;
    }
    const uint8_t *pat;
    switch (sel) {
        case 0x00: case 0x40: pat = M.pcg; break;
        case 0x80: pat = M.kanji; break;
        case 0xC0: pat = M.kanji + 0x20000; break;
        default:
            pat = ((sel & 0x30) == 0x10) ? M.pcg + 0x800 : ((sel & 0x30) == 0x20) ? M.pcg + 0x1000 : M.pcg + 0x1800;
            break;
    }
    uint32_t code;
    const bool kanji = (sel == 0x80 || sel == 0xC0);
    if (font_size) code = kanji ? (((uint32_t)(t2 & 0x3F) << 11) | ((uint32_t)t1 << 3)) : ((uint32_t)t1 << 3);
    else code = kanji ? (((uint32_t)(t2 & 0x3F) << 11) | ((uint32_t)(t1 & 0xFE) << 3)) : ((uint32_t)(t1 & 0xFE) << 3);
    const uint8_t col = attr & 7;
    const uint32_t F = fill4(col ? col : 8), T = fill4(trans);
    uint8_t p;
    if (attr & 0x40) p = blink ? 0xFF : (uint8_t)~pat[code + i];
    else p = blink ? 0x00 : pat[code + i];
    if (!dbl) {
        const uint32_t m = spreadT[p] * 15u;
        w[0] = (F & m) | (T & ~m);
    } else {
        const uint32_t m0 = spreadTD[p >> 4] * 15u, m1 = spreadTD[p & 15] * 15u;
        w[0] = (F & m0) | (T & ~m0);
        w[1] = (F & m1) | (T & ~m1);
    }
}

struct MzTextRow { bool valid; uint32_t key, layout; int n; uint8_t x[80]; };
static MzTextRow textRowList;
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void buildTextRow(uint16_t base, int c, int n, uint32_t key) {
    MzTextRow &R = textRowList;
    R.valid = true; R.key = key; R.layout = layout_gen; R.n = 0;
    const int lines = font_size ? 8 : 16;
    for (int x = 0; x < n; x++) {
        const uint16_t src = (uint16_t)((base + c * n + x) & 0x7FF);
        const uint8_t t1 = M.tvram[src], t2 = M.tvram[0x1000 + src], attr = M.tvram[0x800 + src];
        bool dots = (attr & 0xC0) != 0;
        if (!dots) {
            const uint8_t sel = (uint8_t)((t2 & 0xC0) | (attr & 0x38));
            if (sel & 0x08) {
                const uint32_t code = font_size ? (uint32_t)t1 << 3 : (uint32_t)(t1 & 0xFE) << 3;
                for (int i = 0; i < lines && !dots; i++) dots = (M.pcg[0x800 + code + i] | M.pcg[0x1000 + code + i] | M.pcg[0x1800 + code + i]) != 0;
            } else {
                const uint8_t *pat;
                switch (sel) {
                    case 0x00: case 0x40: pat = M.pcg; break;
                    case 0x80: pat = M.kanji; break;
                    case 0xC0: pat = M.kanji + 0x20000; break;
                    default:
                        pat = ((sel & 0x30) == 0x10) ? M.pcg + 0x800 : ((sel & 0x30) == 0x20) ? M.pcg + 0x1000 : M.pcg + 0x1800;
                        break;
                }
                const bool kanji = (sel == 0x80 || sel == 0xC0);
                uint32_t code;
                if (font_size) code = kanji ? (((uint32_t)(t2 & 0x3F) << 11) | ((uint32_t)t1 << 3)) : ((uint32_t)t1 << 3);
                else code = kanji ? (((uint32_t)(t2 & 0x3F) << 11) | ((uint32_t)(t1 & 0xFE) << 3)) : ((uint32_t)(t1 & 0xFE) << 3);
                for (int i = 0; i < lines && !dots; i++) dots = pat[code + i] != 0;
            }
        }
        if (dots) R.x[R.n++] = (uint8_t)x;
    }
}

static uint32_t txw[80];
static uint32_t txwGen, txwLayout, txwSum;
static int txwKey = -100;
static bool MZ_FAST textWords(int r, uint32_t ts) {
    const uint8_t trans = (textreg[0] & 2) ? 8 : 0;
    const int line = (textreg[0] & 0x10) ? 2 : 0;
    const int height = (textreg[0] & 0x10) ? 20 : 16;
    const int vd = (textreg[9] & 0x0F) << 1;
    const uint16_t start = (uint16_t)(textreg[1] | ((textreg[2] & 0x07) << 8));
    const int ty = r + vd;
    int c = 0, i = -1;
    if (ty >= line) {
        const int cc = (ty - line) / height, l = (ty - line) % height;
        if (line + cc * height < 416 && l < 16) { c = cc; i = font_size ? (l >> 1) : l; }
    }
    const int SL = clampi((textreg[3] - 17) << 1, 0, 400), EL = clampi((textreg[5] - 17) << 1, 0, 400);
    const bool outV = (EL >= SL) ? (r < SL || r >= EL) : (r >= EL && r < SL);
    const int key = outV ? -2 : (i < 0 ? -1 : ((c << 5) | (i << 1) | (blinkOn() ? 1 : 0)));
    if (key == txwKey && (mz_text_gen == txwGen || (layout_gen == txwLayout && ts == txwSum))) return true;
    txwKey = key; txwGen = mz_text_gen; txwLayout = layout_gen; txwSum = ts;
    const uint32_t T = fill4(trans);
    if (outV) { for (int g = 0; g < 80; g++) txw[g] = T; return false; }
    const int mode = textreg[0] & 0x0C;
    if (i < 0) memset(txw, 0, sizeof(txw));
    else if (column80 || mode == 0x04 || mode == 0x08) {
        const bool dbl = !column80;
        const int n = column80 ? 80 : 40;
        const uint16_t base = (!column80 && mode == 0x08) ? (uint16_t)(start + 0x400) : start;
        const uint32_t rk = ts ^ (uint32_t)(c | (n << 8) | (base << 16)) * 0x85EBCA6Bu;
        if (!textRowList.valid || textRowList.key != rk || textRowList.layout != layout_gen) buildTextRow(base, c, n, rk);
        for (int g = 0; g < 80; g++) txw[g] = T;
        const MzTextRow &R = textRowList;
        for (int k = 0; k < R.n; k++) {
            const int x = R.x[k];
            cellWords((uint16_t)((base + c * n + x) & 0x7FF), i, trans, dbl, &txw[dbl ? 2 * x : x]);
        }
    } else {
        for (int x = 0; x < 40; x++) {
            uint32_t a[2], b[2];
            cellWords((uint16_t)((start + c * 40 + x) & 0x7FF), i, trans, true, a);
            cellWords((uint16_t)((start + 0x400 + c * 40 + x) & 0x7FF), i, trans, true, b);
            for (int k = 0; k < 2; k++) {
                const uint32_t mA = nz4(a[k] & 0x77777777u), mB = nz4(b[k] & 0x77777777u) & ~mA;
                const uint32_t blk = (mode == 0x0C) ? ((a[k] | b[k]) & 0x88888888u) : (a[k] & b[k] & 0x88888888u);
                txw[2 * x + k] = (a[k] & mA) | (b[k] & mB) | (blk & ~(mA | mB));
            }
        }
    }
    const int off = column80 ? 9 : 8;
    const int SC = clampi((textreg[7] & 0x7F) - off, 0, 80), EC = clampi((textreg[8] & 0x7F) - off, 0, 80);
    if (EC >= SC) { for (int g = 0; g < SC; g++) txw[g] = T; for (int g = EC; g < 80; g++) txw[g] = T; }
    else for (int g = EC; g < SC; g++) txw[g] = T;
    return false;
}

static inline uint32_t bswap32(uint32_t v) {
#ifdef _MSC_VER
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}
static void shiftRight(uint32_t *w, int s) {
    const int ws = s >> 3, ns = (s & 7) * 4;
    for (int g = 79; g >= 0; g--) {
        const int k = g - ws;
        const uint32_t cur = k >= 0 ? bswap32(w[k]) : 0, prev = k >= 1 ? bswap32(w[k - 1]) : 0;
        w[g] = bswap32(ns ? ((cur >> ns) | (prev << (32 - ns))) : cur);
    }
}

static uint32_t cgw[80];
static uint32_t cgwGen, cgwLayout, cgwSum;
static int cgwRow = -1;

static uint8_t **rowTab;
static uint8_t *rowOwn[MZ_SCREEN_H];
static bool rowShared[MZ_SCREEN_H / 2];
static bool cgcValid[MZ_SCREEN_H / 2];
static uint32_t cgcLayout[MZ_SCREEN_H / 2], cgcSum[MZ_SCREEN_H / 2];

static bool MZ_FAST cgWords(int y, uint32_t gs) {
    const uint8_t mode = cgreg[0x0E];
    int kind, ycg, xmax;
    switch (mode) {
        case 0x17: case 0x97: kind = 0; ycg = y >> 1; xmax = 80; break;
        case 0x14: case 0x15: case 0x94: case 0x95: kind = 1; ycg = y >> 1; xmax = 40; break;
        case 0x93: kind = 2; ycg = y; xmax = 80; break;
        case 0x03: kind = 3; ycg = y; xmax = 80; break;
        default: kind = 4; ycg = 0; xmax = 0; break;
    }
    if (ycg == cgwRow && (mz_cg_gen == cgwGen || (layout_gen == cgwLayout && gs == cgwSum))) return true;
    cgwGen = mz_cg_gen; cgwLayout = layout_gen; cgwSum = gs;
    if (kind == 4) { memset(cgw, 0, sizeof(cgw)); cgwRow = 0; return false; }
    const bool cacheable = kind <= 1 && rowTab == mz_rows && rowShared[ycg];
    if (cacheable && cgcValid[ycg] && cgcLayout[ycg] == cgl_gen && cgcSum[ycg] == gs) {
        memcpy(cgw, rowOwn[2 * ycg + 1], sizeof(cgw));
        cgwRow = ycg;
        return false;
    }
    uint8_t HD = cgreg[0x0F] & 7;
    if (!(mode & 0x02)) HD <<= 1;
    const uint16_t SAD0 = (uint16_t)(cgreg[0x10] | ((cgreg[0x11] & 0x7F) << 8));
    const uint16_t SAD1 = (uint16_t)(cgreg[0x12] | ((cgreg[0x13] & 0x7F) << 8));
    const uint16_t SAD2 = (uint16_t)(cgreg[0x14] | ((cgreg[0x15] & 0x7F) << 8));
    const int SLN1 = cgreg[0x16] | ((cgreg[0x17] & 1) << 8);
    uint16_t a0;
    int hd = 0;
    if (ycg < SLN1) { hd = HD; a0 = addrAdvance(SAD0, (uint32_t)ycg * xmax, SAD1); }
    else a0 = addrAdvance(SAD2, (uint32_t)(ycg - SLN1) * xmax, SAD1);
    const bool linear = (a0 <= SAD1) ? (a0 + xmax - 1 <= SAD1) : (a0 + xmax - 1 <= 0x7FFF);
    uint16_t addr[80];
    if (!linear) { uint16_t a = a0; for (int x = 0; x < xmax; x++) { addr[x] = a; a = addrStep(a, SAD1); } }
#define CG_ADDR(x) (linear ? (uint16_t)(a0 + (x)) : addr[x])
    const uint16_t ex = (uint16_t)((mode & 0x80) << 7);
    const uint8_t r18 = cgreg[0x18];
    if (kind == 0) {
        const uint16_t sub = (mode & 0x80) ? 0x4000 : 0x0000;
        const uint8_t mb = (r18 & 1) ? 0xFF : 0, mr = (r18 & 2) ? 0xFF : 0, mg = (r18 & 4) ? 0xFF : 0, mi = (r18 & 8) ? 0xFF : 0;
        for (int x = 0; x < 80; x++) {
            const uint16_t src = (uint16_t)(((CG_ADDR(x) ^ sub) | ex) & 0x7FFF);
            cgw[x] = spreadL[vram_b[src] & mb] | (spreadL[vram_r[src] & mr] << 1) | (spreadL[vram_g[src] & mg] << 2) | (spreadL[vram_i[src] & mi] << 3);
        }
    } else if (kind == 1) {
        const int plB = (mode == 0x14) ? 0 : (mode == 0x15) ? 1 : (mode == 0x94) ? 2 : 3;
        const int plT = (mode == 0x14) ? 1 : (mode == 0x15) ? 0 : (mode == 0x94) ? 3 : 2;
        const uint16_t subB = (plB & 1) ? 0x2000 : 0x0000, subT = (plT & 1) ? 0x2000 : 0x0000;
        const uint8_t mB = (plB == 0 || plB == 2) ? (r18 & 0x0F) : (r18 >> 4), mT = (plT == 0 || plT == 2) ? (r18 & 0x0F) : (r18 >> 4);
        const uint8_t bB = (mB & 1) ? 0xFF : 0, rB = (mB & 2) ? 0xFF : 0, gB = (mB & 4) ? 0xFF : 0, iB = (mB & 8) ? 0xFF : 0;
        const uint8_t bT = (mT & 1) ? 0xFF : 0, rT = (mT & 2) ? 0xFF : 0, gT = (mT & 4) ? 0xFF : 0, iT = (mT & 8) ? 0xFF : 0;
        for (int x = 0; x < 40; x++) {
            const uint16_t ad = CG_ADDR(x);
            const uint16_t sb = (uint16_t)(((ad ^ subB) | ex) & 0x7FFF), st = (uint16_t)(((ad ^ subT) | ex) & 0x7FFF);
            const uint8_t B0 = vram_b[sb] & bB, R0 = vram_r[sb] & rB, G0 = vram_g[sb] & gB, I0 = vram_i[sb] & iB;
            const uint8_t B1 = vram_b[st] & bT, R1 = vram_r[st] & rT, G1 = vram_g[st] & gT, I1 = vram_i[st] & iT;
            const uint8_t any = (uint8_t)(B1 | R1 | G1 | I1);
            uint32_t bot = spreadD[B0 & 15] | (spreadD[R0 & 15] << 1) | (spreadD[G0 & 15] << 2) | (spreadD[I0 & 15] << 3);
            uint32_t top = spreadD[B1 & 15] | (spreadD[R1 & 15] << 1) | (spreadD[G1 & 15] << 2) | (spreadD[I1 & 15] << 3);
            uint32_t m = spreadD[any & 15] * 15u;
            cgw[2 * x] = (top & m) | (bot & ~m);
            bot = spreadD[B0 >> 4] | (spreadD[R0 >> 4] << 1) | (spreadD[G0 >> 4] << 2) | (spreadD[I0 >> 4] << 3);
            top = spreadD[B1 >> 4] | (spreadD[R1 >> 4] << 1) | (spreadD[G1 >> 4] << 2) | (spreadD[I1 >> 4] << 3);
            m = spreadD[any >> 4] * 15u;
            cgw[2 * x + 1] = (top & m) | (bot & ~m);
        }
    } else if (kind == 2) {
        for (int x = 0; x < 80; x++) {
            const uint16_t src = CG_ADDR(x) & 0x7FFF;
            cgw[x] = spreadL[vram_b[src]] | (spreadL[vram_r[src]] << 1) | (spreadL[vram_g[src]] << 2) | (spreadL[vram_i[src]] << 3);
        }
    } else {
        for (int x = 0; x < 80; x++) {
            const uint16_t src = CG_ADDR(x), o = src & 0x3FFF;
            const uint8_t B = (r18 & 1) ? ((src & 0x4000) ? vram_g[o] : vram_b[o]) : 0;
            const uint8_t R = (r18 & 2) ? ((src & 0x4000) ? vram_i[o] : vram_r[o]) : 0;
            cgw[x] = spreadL[B] | (spreadL[R] << 1);
        }
    }
#undef CG_ADDR
    if (hd) shiftRight(cgw, hd);
    cgwRow = ycg;
    if (cacheable) {
        memcpy(rowOwn[2 * ycg + 1], cgw, sizeof(cgw));
        cgcValid[ycg] = true; cgcLayout[ycg] = cgl_gen; cgcSum[ycg] = gs;
    }
    return false;
}

static uint32_t MZ_FAST gvLineSum(int y) {
    const uint8_t mode = cgreg[0x0E];
    int kind, ycg, xmax;
    switch (mode) {
        case 0x17: case 0x97: kind = 0; ycg = y >> 1; xmax = 80; break;
        case 0x14: case 0x15: case 0x94: case 0x95: kind = 1; ycg = y >> 1; xmax = 40; break;
        case 0x1D: case 0x9D: kind = 1; ycg = y >> 1; xmax = 40; break;
        case 0x19: case 0x99: kind = 1; ycg = y; xmax = 40; break;
        case 0x93: kind = 2; ycg = y; xmax = 80; break;
        case 0x03: kind = 3; ycg = y; xmax = 80; break;
        default: return 0;
    }
    const uint16_t SAD0 = (uint16_t)(cgreg[0x10] | ((cgreg[0x11] & 0x7F) << 8));
    const uint16_t SAD1 = (uint16_t)(cgreg[0x12] | ((cgreg[0x13] & 0x7F) << 8));
    const uint16_t SAD2 = (uint16_t)(cgreg[0x14] | ((cgreg[0x15] & 0x7F) << 8));
    const int SLN1 = cgreg[0x16] | ((cgreg[0x17] & 1) << 8);
    uint16_t a = (ycg < SLN1) ? addrAdvance(SAD0, (uint32_t)ycg * xmax, SAD1) : addrAdvance(SAD2, (uint32_t)(ycg - SLN1) * xmax, SAD1);
    const bool linear = (a <= SAD1) ? (a + xmax - 1 <= SAD1) : (a + xmax - 1 <= 0x7FFF);
    const uint16_t ex = (uint16_t)((mode & 0x80) << 7);
    uint16_t s1 = 0, s2 = 0;
    if (kind == 0) s1 = (mode & 0x80) ? 0x4000 : 0x0000;
    else if (kind == 1) {
        s1 = (mode == 0x15 || mode == 0x95) ? 0x2000 : 0x0000;
        s2 = (uint16_t)(s1 ^ 0x2000);
    }
    uint32_t s = 0;
    int last = -1, last2 = -1;
    for (int x = 0; x < xmax; ) {
        int ch;
        if (kind == 2) ch = (a & 0x7FFF) >> 5;
        else if (kind == 3) ch = (a & 0x3FFF) >> 5;
        else ch = (((a ^ s1) | ex) & 0x7FFF) >> 5;
        if (ch != last) { s = s * 0x01000193u + mz_gv_chunk_gen[ch]; last = ch; }
        if (kind == 1) {
            const int ch2 = (((a ^ s2) | ex) & 0x7FFF) >> 5;
            if (ch2 != last2) { s = s * 0x01000193u + mz_gv_chunk_gen[ch2]; last2 = ch2; }
        }
        if (linear) { const int k = 32 - (a & 31); x += k; a = (uint16_t)(a + k); }
        else { x++; a = addrStep(a, SAD1); }
    }
    return s;
}

static uint32_t txBlinkGen[128];
static uint16_t txBlinkBits[128];
static bool txBlinkInit;
static inline uint32_t txBlinkMask(int ch) {
    if (!txBlinkInit) { for (int i = 0; i < 128; i++) txBlinkGen[i] = ~mz_tx_chunk_gen[i]; txBlinkInit = true; }
    if (txBlinkGen[ch] != mz_tx_chunk_gen[ch]) {
        uint16_t m = 0;
        const uint8_t *a = M.tvram + 0x800 + ch * 16;
        for (int i = 0; i < 16; i++) if (a[i] & 0x80) m |= (uint16_t)(1 << i);
        txBlinkBits[ch] = m;
        txBlinkGen[ch] = mz_tx_chunk_gen[ch];
    }
    return txBlinkBits[ch];
}

#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static uint32_t txLineSum(int r) {
    const int line = (textreg[0] & 0x10) ? 2 : 0;
    const int height = (textreg[0] & 0x10) ? 20 : 16;
    const int vd = (textreg[9] & 0x0F) << 1;
    const uint16_t start = (uint16_t)(textreg[1] | ((textreg[2] & 0x07) << 8));
    const int ty = r + vd;
    int c = 0, i = -1;
    if (ty >= line) {
        const int cc = (ty - line) / height, l = (ty - line) % height;
        if (line + cc * height < 416 && l < 16) { c = cc; i = l; }
    }
    const int SL = clampi((textreg[3] - 17) << 1, 0, 400), EL = clampi((textreg[5] - 17) << 1, 0, 400);
    const bool outV = (EL >= SL) ? (r < SL || r >= EL) : (r >= EL && r < SL);
    if (outV) return 1;
    if (i < 0) return 2;
    uint32_t s = mz_pcg_gen;
    uint32_t blink = 0;
    int last = -1, last2 = -1;
    const int mode = textreg[0] & 0x0C;
    const bool two = !column80 && mode != 0x04 && mode != 0x08;
    const int n = column80 ? 80 : 40;
    const uint16_t base = (uint16_t)((!column80 && mode == 0x08) ? start + 0x400 : start);
    for (int x = 0; x < n; ) {
        const uint16_t o = (uint16_t)((base + c * n + x) & 0x7FF);
        const int k = (16 - (o & 15)) < (n - x) ? 16 - (o & 15) : n - x;
        const uint32_t range = ((1u << k) - 1) << (o & 15);
        if ((o >> 4) != last) { last = o >> 4; s = s * 0x01000193u + mz_tx_chunk_gen[last]; }
        blink |= txBlinkMask(o >> 4) & range;
        if (two) {
            const uint16_t o2 = (uint16_t)((start + 0x400 + c * 40 + x) & 0x7FF);
            if ((o2 >> 4) != last2) { last2 = o2 >> 4; s = s * 0x01000193u + mz_tx_chunk_gen[last2]; }
            blink |= txBlinkMask(o2 >> 4) & range;
        }
        x += k;
    }
    if (blink && blinkOn()) s ^= 0x165667B1u;
    return s;
}

static uint8_t cgLut[256], frontLut[256], txtLutIn[256], txtLutOut[256];
static uint8_t lutCg[16], lutTxtc[9], lutTxtOut[9];
static uint32_t lutFront;
static bool lutValid, cgIdent;
#ifndef MZ_COMPOSE2
#define MZ_COMPOSE2 0
#endif
#if MZ_COMPOSE2
static uint8_t outTLut[256];
static uint32_t outTKey;
static bool outTValid;
#endif
static int copyHs, copyHe;
static uint32_t copyFrame;

#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void rebuildLuts(const uint8_t *cgcol, uint32_t front, const uint8_t *txtc, const uint8_t *txtOut) {
    if (!lutValid || memcmp(cgcol, lutCg, 16) != 0) {
        memcpy(lutCg, cgcol, 16);
        cgIdent = true;
        for (int c = 0; c < 16; c++) if (cgcol[c] != c) cgIdent = false;
        for (int v = 0; v < 256; v++) cgLut[v] = (uint8_t)((cgcol[v >> 4] << 4) | cgcol[v & 15]);
    }
    if (!lutValid || front != lutFront) {
        lutFront = front;
        for (int v = 0; v < 256; v++) frontLut[v] = (uint8_t)((((front >> (v >> 4)) & 1) ? 0xF0 : 0) | (((front >> (v & 15)) & 1) ? 0x0F : 0));
    }
    if (!lutValid || memcmp(txtc, lutTxtc, 9) != 0) {
        memcpy(lutTxtc, txtc, 9);
        for (int v = 0; v < 256; v++) txtLutIn[v] = (uint8_t)((((v >> 4) < 9 ? txtc[v >> 4] : 0) << 4) | ((v & 15) < 9 ? txtc[v & 15] : 0));
    }
    if (!lutValid || memcmp(txtOut, lutTxtOut, 9) != 0) {
        memcpy(lutTxtOut, txtOut, 9);
        for (int v = 0; v < 256; v++) txtLutOut[v] = (uint8_t)((((v >> 4) < 9 ? txtOut[v >> 4] : 0) << 4) | ((v & 15) < 9 ? txtOut[v & 15] : 0));
    }
    lutValid = true;
#if MZ_COMPOSE2
    outTValid = false;
#endif
}

#if MZ_COMPOSE2
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void buildOutT(uint32_t T, uint32_t front) {
    for (int v = 0; v < 256; v++) {
        const uint32_t ci = (uint32_t)v * 0x01010101u;
        const uint32_t c = lut4(cgLut, ci);
        uint32_t m = nz4(T);
        if (front) m &= ~lut4(frontLut, ci);
        outTLut[v] = (uint8_t)((c & ~m) | (lut4(txtLutIn, T) & m));
    }
    outTKey = T; outTValid = true;
}
#endif

static uint8_t colCg[16], colTxtc[9], colTxtOut[9];
static uint32_t colFront, colSig, colLayout, colColor;
static bool colValid, colUse4096;
#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static void updateColors() {
    const uint8_t back16 = (uint8_t)(((textreg[0x0B] & 4) >> 2) | ((textreg[0x0B] & 0x20) >> 4) | ((textreg[0x0C] & 1) << 2) | ((textreg[0x0B] & 1) << 3));
    uint32_t front = 0;
    for (int c = 0; c < 16; c++) {
        colCg[c] = (palette_reg[c] & 0x0F) ? (uint8_t)(palette_reg[c] & cg_mask) : (uint8_t)(back16 & cg_mask);
        if (palette_reg[c] & 0x10) front |= 1u << c;
    }
    if (use4096) {
        int dark = 0, dv = 1 << 30;
        for (int i = 0; i < 16; i++) { const int v = pal4096[i][0] + pal4096[i][1] + pal4096[i][2]; if (v < dv) { dv = v; dark = i; } }
        colTxtc[0] = 0;
        for (int c = 1; c < 8; c++) colTxtc[c] = colCg[c + 8];
        colTxtc[8] = colCg[0];
        memcpy(colTxtOut, colTxtc, sizeof(colTxtc));
        colTxtOut[8] = (uint8_t)dark;
    } else {
        colTxtc[0] = 0;
        for (int c = 1; c < 8; c++) colTxtc[c] = digital(c);
        colTxtc[8] = 0;
        memcpy(colTxtOut, colTxtc, sizeof(colTxtc));
    }
    colTxtOut[0] = palette_reg[back16] & 0x0F;
    uint32_t sig = layout_gen * 0x9E3779B1u + front;
    for (int c = 0; c < 16; c++) sig = sig * 0x01000193u + colCg[c];
    for (int c = 0; c < 9; c++) sig = sig * 0x01000193u + (uint32_t)(colTxtc[c] | (colTxtOut[c] << 4));
    colFront = front; colSig = sig;
    colLayout = layout_gen; colColor = color_gen; colUse4096 = use4096; colValid = true;
}

static uint8_t out256[2][256], front256[256];
static uint8_t back256Byte[2];
static uint8_t txt256Lut[256];
static uint32_t out256Layout, out256Color;
static bool out256Valid;

static uint8_t mask256() {
    if (cg_mask256_init) {
        cg_mask256 = cgreg[0x18];
        if (!(cg_mask & 1)) cg_mask256 &= ~(0x11 | 0x88);
        if (!(cg_mask & 2)) cg_mask256 &= ~0x22;
        if (!(cg_mask & 4)) cg_mask256 &= ~0x44;
        cg_mask256_init = false;
    }
    return cg_mask256;
}

static void build256Out() {
    const int bb = ((textreg[0x0B] & 7) << 5), br = ((textreg[0x0B] & 0x38) << 2), bg = (((textreg[0x0B] & 0xC0) >> 1) | ((textreg[0x0C] & 1) << 7));
    tile2x2(br, bg, bb, back256Byte);
    for (int c = 0; c < 256; c++) {
        const uint8_t hi = palette_reg[c >> 4];
        const uint8_t cc = (uint8_t)(((hi & 0x0F) << 4) | (c & 0x0F));
        if (cc == 0) { out256[0][c] = back256Byte[0]; out256[1][c] = back256Byte[1]; }
        else { out256[0][c] = tile256[cc][0]; out256[1][c] = tile256[cc][1]; }
        front256[c] = (hi & 0x10) ? 0xFF : 0x00;
    }
    const bool to64 = column80 ? !(textreg[0] & 1) : ((textreg[0] & 0x0C) != 0 && !(textreg[0] & 1));
    uint8_t nib[16] = { 0 };
    for (int c = 1; c < 8; c++) nib[c] = to64 ? txt64near[c << 3] : digitalCga[c];
    for (int v = 0; v < 256; v++) txt256Lut[v] = (uint8_t)((nib[v >> 4] << 4) | nib[v & 15]);
    out256Layout = layout_gen; out256Color = color_gen; out256Valid = true;
}

static void text64Words(int r, uint32_t *tw, uint32_t *mt) {
    const uint8_t trans = (textreg[0] & 2) ? 8 : 0;
    const uint32_t transM = trans ? 0xFFFFFFFFu : 0;
    const int line = (textreg[0] & 0x10) ? 2 : 0;
    const int height = (textreg[0] & 0x10) ? 20 : 16;
    const int vd = (textreg[9] & 0x0F) << 1;
    const uint16_t start = (uint16_t)(textreg[1] | ((textreg[2] & 0x07) << 8));
    const int ty = r + vd;
    int c = 0, i = -1;
    if (ty >= line) {
        const int cc = (ty - line) / height, l = (ty - line) % height;
        if (line + cc * height < 416 && l < 16) { c = cc; i = font_size ? (l >> 1) : l; }
    }
    const int SL = clampi((textreg[3] - 17) << 1, 0, 400), EL = clampi((textreg[5] - 17) << 1, 0, 400);
    const bool outV = (EL >= SL) ? (r < SL || r >= EL) : (r >= EL && r < SL);
    if (outV) { for (int g = 0; g < 80; g++) { tw[g] = 0; mt[g] = transM; } return; }
    if (i < 0) { memset(tw, 0, 80 * sizeof(uint32_t)); memset(mt, 0, 80 * sizeof(uint32_t)); }
    else {
        for (int x = 0; x < 40; x++) {
            uint32_t a[2], b[2];
            cellWords((uint16_t)((start + c * 40 + x) & 0x7FF), i, trans, true, a);
            cellWords((uint16_t)((start + 0x400 + c * 40 + x) & 0x7FF), i, trans, true, b);
            for (int k = 0; k < 2; k++) {
                uint32_t o = 0, m = 0;
                for (int s = 0; s < 32; s += 4) {
                    const uint32_t av = (a[k] >> s) & 15, bv = (b[k] >> s) & 15;
                    const uint32_t v = ((av & 7) << 3) | (bv & 7);
                    if (v) { o |= (uint32_t)txt64near[v] << s; m |= 15u << s; }
                    else if ((av & 8) && (bv & 8)) m |= 15u << s;
                }
                tw[2 * x + k] = o; mt[2 * x + k] = m;
            }
        }
    }
    const int SC = clampi((textreg[7] & 0x7F) - 8, 0, 80), EC = clampi((textreg[8] & 0x7F) - 8, 0, 80);
    if (EC >= SC) {
        for (int g = 0; g < SC; g++) { tw[g] = 0; mt[g] = transM; }
        for (int g = EC; g < 80; g++) { tw[g] = 0; mt[g] = transM; }
    } else for (int g = EC; g < SC; g++) { tw[g] = 0; mt[g] = transM; }
}

#ifdef _MSC_VER
__declspec(noinline)
#else
__attribute__((noinline))
#endif
static int render256(int y, uint8_t *dst) {
    const uint8_t mode = cgreg[0x0E];
    if (use4096) { use4096 = false; mz_palette_gen++; }
    if (!use256) { use256 = true; mz_palette_gen++; }
    if (!out256Valid || out256Layout != layout_gen || out256Color != color_gen) build256Out();
    const uint32_t gs = gvLineSum(y), ts = txLineSum(y);
    const uint32_t sig = mixWord(mixWord(mixWord(0x2560A11Du ^ layout_gen, color_gen), gs), ts);
    if (lineValid[y] && lineSig[y] == sig) return 2;
    lineSig[y] = sig; lineValid[y] = true;
    copyY = -1;
    static uint32_t tw[80], tm[80];
    if (!column80 && (textreg[0] & 0x0C) == 0x00) text64Words(y, tw, tm);
    else {
        textWords(y, ts);
        for (int g = 0; g < 80; g++) { tw[g] = lut4(txt256Lut, txw[g]); tm[g] = nz4(txw[g]); }
    }
    const bool row400 = (mode == 0x19 || mode == 0x99);
    const int ycg = row400 ? y : (y >> 1);
    static uint8_t col[320 + 8];
    static int colRow = -1;
    static uint32_t colLayout, colSum;
    if (ycg != colRow || layout_gen != colLayout || gs != colSum) {
        colRow = ycg; colLayout = layout_gen; colSum = gs;
        uint16_t addr[40];
        int hd;
        rowAddr(ycg, 40, addr, &hd);
        const uint16_t ex = (uint16_t)((mode & 0x80) << 7);
        const uint8_t m = mask256();
        const int sh0 = hd >> 1;
        memset(col, 0, sh0);
        for (int x = 0; x < 40; x++) {
            const uint16_t s1 = (uint16_t)(addr[x] | ex) & 0x7FFF, s2 = (uint16_t)((addr[x] ^ 0x2000) | ex) & 0x7FFF;
            const uint32_t lo = spreadL[(m & 0x10) ? vram_b[s2] : 0] | (spreadL[(m & 0x20) ? vram_r[s2] : 0] << 1) |
                                (spreadL[(m & 0x40) ? vram_g[s2] : 0] << 2) | (spreadL[(m & 0x80) ? vram_i[s2] : 0] << 3);
            const uint32_t hi = spreadL[(m & 0x01) ? vram_b[s1] : 0] | (spreadL[(m & 0x02) ? vram_r[s1] : 0] << 1) |
                                (spreadL[(m & 0x04) ? vram_g[s1] : 0] << 2) | (spreadL[(m & 0x08) ? vram_i[s1] : 0] << 3);
            const int p = x * 8 + sh0;
            if (p >= 320) break;
            for (int k = 0; k < 8; k++) {
                const int s = (k >> 1) * 8 + ((k & 1) ? 0 : 4);
                col[p + k] = (uint8_t)((((hi >> s) & 15) << 4) | ((lo >> s) & 15));
            }
        }
    }
    const int vs = (GDEVS <= GDEVE) ? GDEVS * (row400 ? 1 : 2) : 0;
    const int ve = (GDEVS <= GDEVE) ? GDEVE * (row400 ? 1 : 2) : 400;
    const int hs = (GDEHS <= GDEHE && GDEHS < 80) ? (GDEHS << 3) : 0;
    const int he = (GDEHS <= GDEHE && GDEHE < 80) ? (GDEHE << 3) : 640;
    const bool inV = (y >= vs && y < ve);
    const int gHs = inV ? (hs >> 3) : 80, gHe = inV ? (he >> 3) : 80;
    const int par = y & 1;
    const uint8_t *const ob = out256[par];
    const uint32_t backW = back256Byte[par] * 0x01010101u;
    uint32_t *d = (uint32_t *)dst;
    for (int g = 0; g < 80; g++) {
        uint32_t mt = tm[g];
        if (g < gHs || g >= gHe) { d[g] = (backW & ~mt) | (tw[g] & mt); continue; }
        const uint8_t *c4 = col + 4 * g;
        const uint32_t cw = (uint32_t)ob[c4[0]] | ((uint32_t)ob[c4[1]] << 8) | ((uint32_t)ob[c4[2]] << 16) | ((uint32_t)ob[c4[3]] << 24);
        const uint32_t fw = (uint32_t)front256[c4[0]] | ((uint32_t)front256[c4[1]] << 8) | ((uint32_t)front256[c4[2]] << 16) | ((uint32_t)front256[c4[3]] << 24);
        mt &= ~fw;
        d[g] = (cw & ~mt) | (tw[g] & mt);
    }
    return 1;
}

static inline uint32_t lineSigFast(int y, uint32_t &gs, uint32_t &ts) {
    if (pal_select != use4096) { use4096 = pal_select; mz_palette_gen++; }
    if (use256) { use256 = false; mz_palette_gen++; }
    if (!colValid || colLayout != layout_gen || colColor != color_gen || colUse4096 != use4096) updateColors();
    gs = gvLineSum(y); ts = txLineSum(y);
    return (colSig * 0x01000193u + gs) * 0x01000193u + ts;
}

static int MZ_FAST renderFast(int y, uint8_t *dst) {
    const uint8_t mode = cgreg[0x0E];
    if (mode == 0x1D || mode == 0x9D || mode == 0x19 || mode == 0x99) return render256(y, dst);
    const bool row400 = (mode == 0x03 || mode == 0x93);
    const int vs = (GDEVS <= GDEVE) ? GDEVS * (row400 ? 1 : 2) : 0;
    const int ve = (GDEVS <= GDEVE) ? GDEVE * (row400 ? 1 : 2) : 400;
    const int hs = (GDEHS <= GDEHE && GDEHS < 80) ? (GDEHS << 3) : 0;
    const int he = (GDEHS <= GDEHE && GDEHE < 80) ? (GDEHE << 3) : 640;
    const bool inV = (y >= vs && y < ve);
    uint32_t gs, ts;
    const uint32_t sig = lineSigFast(y, gs, ts);
    const uint8_t *const cgcol = colCg, *const txtc = colTxtc, *const txtOut = colTxtOut;
    const uint32_t front = colFront;
    if (lineValid[y] && lineSig[y] == sig) return 2;
    lineSig[y] = sig; lineValid[y] = true;
    const bool cgSame = cgWords(y, gs);
    const bool txSame = textWords(y, ts);
    const bool lutSame = lutValid && memcmp(cgcol, lutCg, 16) == 0 && front == lutFront &&
                         memcmp(txtc, lutTxtc, 9) == 0 && memcmp(txtOut, lutTxtOut, 9) == 0;
    if (!lutSame) rebuildLuts(cgcol, front, txtc, txtOut);
    const int gHs = inV ? (hs >> 3) : 80, gHe = inV ? (he >> 3) : 80;
    const bool copy = cgSame && txSame && lutSame && copyY == y - 1 && copyFrame == mz_frame_count && gHs == copyHs && gHe == copyHe;
    copyY = y; copyFrame = mz_frame_count; copyHs = gHs; copyHe = gHe;
    if (copy) { memcpy(dst, mz_rows[y - 1], MZ_SCREEN_W / 2); return 1; }
    uint32_t *d = (uint32_t *)dst;
    int g = 0;
    for (; g < gHs; g++) d[g] = lut4(txtLutOut, txw[g]);
    if (front == 0xFFFF) {
        for (; g < gHe; g++) d[g] = cgIdent ? cgw[g] : lut4(cgLut, cgw[g]);
    } else {
#if MZ_COMPOSE2
        const uint32_t T = fill4((textreg[0] & 2) ? 8 : 0);
        if (T) {
            if (!outTValid || outTKey != T) buildOutT(T, front);
            for (; g < gHe; g++) {
                const uint32_t ci = cgw[g], t = txw[g];
                if (t == T) { d[g] = lut4(outTLut, ci); continue; }
                uint32_t c = cgIdent ? ci : lut4(cgLut, ci);
                if (t) {
                    uint32_t m = nz4(t);
                    if (front) m &= ~lut4(frontLut, ci);
                    c = (c & ~m) | (lut4(txtLutIn, t) & m);
                }
                d[g] = c;
            }
        }
#endif
        for (; g < gHe; g++) {
            const uint32_t ci = cgw[g], t = txw[g];
            uint32_t c = cgIdent ? ci : lut4(cgLut, ci);
            if (t) {
                uint32_t m = nz4(t);
                if (front) m &= ~lut4(frontLut, ci);
                c = (c & ~m) | (lut4(txtLutIn, t) & m);
            }
            d[g] = c;
        }
    }
    for (; g < 80; g++) d[g] = lut4(txtLutOut, txw[g]);
    return 1;
}

static int pairY = -1;
static uint32_t pairFrame, pairVgen, pairColor;

static void rowsTake() {
    rowTab = mz_rows;
    for (int y = 0; y < MZ_SCREEN_H; y++) rowOwn[y] = mz_rows[y];
    memset(rowShared, 0, sizeof(rowShared));
    memset(cgcValid, 0, sizeof(cgcValid));
    pairY = -1;
}

void mzVideoUnshareAll() {
    if (!mz_rows || rowTab != mz_rows) return;
    for (int r = 0; r < MZ_SCREEN_H / 2; r++) {
        if (!rowShared[r]) continue;
        const int y = 2 * r + 1;
        memcpy(rowOwn[y], rowOwn[y - 1], MZ_SCREEN_W / 2);
        mz_rows[y] = rowOwn[y];
        rowShared[r] = false;
        cgcValid[r] = false;
        lineSig[y] = lineSig[y - 1]; lineValid[y] = lineValid[y - 1];
    }
    pairY = -1;
}

static void MZ_FAST drawLine(int y, uint8_t *dst) {
    if (screen_mask) {
        pairY = -1;
        const uint32_t sig = layout_gen * 0x9E3779B1u ^ 0x5BD1E995u;
        if (lineValid[y] && lineSig[y] == sig) return;
        lineSig[y] = sig; lineValid[y] = true;
        memset(dst, 0, MZ_SCREEN_W / 2);
        return;
    }
    const int r = renderFast(y, dst);
    if (!(y & 1)) {
        const uint8_t mode = cgreg[0x0E];
        const bool pair = font_size && mode != 0x03 && mode != 0x93 && mode != 0x1D && mode != 0x9D && mode != 0x19 && mode != 0x99;
        pairY = pair ? y : -1; pairFrame = mz_frame_count; pairVgen = mz_vram_gen; pairColor = color_gen;
    }
    if (r == 2) return;
    if (r == 1) return;
    lineValid[y] = false;
    renderGeneric(y, dst);
}

void MZ_FAST mzRenderLine(int y) {
    if (!mz_rows) return;
    if (rowTab != mz_rows) rowsTake();
    uint8_t *const dst = rowOwn[y];
    if (!dst) return;
    if (y & 1) {
        const int r = y >> 1;
        if (pairY == y - 1 && pairFrame == mz_frame_count) {
            bool same = (pairVgen == mz_vram_gen && pairColor == color_gen);
            if (!same && lineValid[y - 1]) { uint32_t gs, ts; same = (lineSigFast(y, gs, ts) == lineSig[y - 1]); }
            if (same) {
                if (!rowShared[r]) { rowShared[r] = true; mz_rows[y] = rowOwn[y - 1]; lineValid[y] = false; }
                return;
            }
        }
        if (rowShared[r]) {
            rowShared[r] = false;
            cgcValid[r] = false;
            lineValid[y] = false;
            drawLine(y, dst);
            mz_rows[y] = dst;
            return;
        }
    }
    drawLine(y, dst);
}
