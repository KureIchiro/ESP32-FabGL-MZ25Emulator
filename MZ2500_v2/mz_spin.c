/* ============================================================================
 *  MZ-2500: Z80 の待ちループの早送り(MZ_IDLE_SKIP。切り替えは z80core_config.h)
 *  2026-11-06 に前の Z80 コアの中に入れたものを、2026-11-20 に z80core のフック(Z80CORE_SPIN)へ移した(X1 版と同じ作り)。
 *
 *  ゲームが割込みや垂直帰線を待つ小さなループ(Druaga: IN A,(BDh) / BIT 7,A / JP NZ、レイドック: LD A,n / OR A / JP NZ
 *  (n は割込みが書き換える)、ゼビウス: DEC HL / LD A,L / OR H / JR NZ)を、回れる周回ぶんまとめて進める。
 *  結果(レジスタ・R・PC・残りサイクル)は 1 命令ずつ実行したときと同じ:
 *  ・JP / JR / DJNZ が 8 バイト以内に戻るとき、ループの中身を 1 回だけ解読する。書き込み・PUSH・CALL・分岐・EI・DAA
 *    などを含まず、I/O は IN A,(n) と IN r,(C)(C はループの中で変えない)だけ。ポートは 1 ラインの中で値が変わらず
 *    読んでも何も起きないもの(BCh〜BFh・C8h・E0h〜E2h。mz_core.cpp mzIoPeek)だけ
 *  ・種類 1(状態が変わらない待ち): 頭に 2 回続けて戻ったとき、その間がちょうど 1 周ぶんのサイクル(z80core の表と
 *    M1 のウェイトから計算した値。Z80 の通算のサイクルで比べるのでラインをまたいでもよい。VRAM などのウェイトや割込みの
 *    処理が入ると合わない)で、レジスタが全部同じで、ループが読む I/O の値も前に通ったときと同じなら、残りのサイクルで
 *    回れる周回をまとめて引く。ライン(cpuTask の 1 回の実行)の中では RAM を書くものも I/O の値を変えるものもないので、
 *    以後の周回も同じ。ラインの境目では I/O の値(垂直帰線など)だけが変わりうるので、それは読んで比べる
 *  ・種類 2(16 ビットの時間待ち): DEC rr / LD A,上か下 / OR 下か上 / JR NZ か JP NZ
 *  ・種類 3(8 ビットの回数待ち): ... / DEC r / JR NZ か JP NZ、または ... / DJNZ。ほかの命令は数えるレジスタを読み
 *    書きしない。2 回続けて、数えるレジスタがちょうど 1 減り、ほか(DEC のときは F の C 以外を除く)が同じなら、
 *    回数が 1 になるまでと残りサイクルの少ないほうだけ進め、F は最後の DEC のあとと同じにする
 *  ・最後の周回(ループを出る周回)と端数は、ふつうに 1 命令ずつ実行する。割込みが入ったら調べ直す。
 *    4 回続けて条件に合わなかったループは以後調べない
 * ============================================================================ */
#include "z80core.h"
#include "z80core_config.h"
#if defined(ARDUINO)
#include <esp_attr.h>
#else
#define IRAM_ATTR
#endif

int mz_sp_have;
static unsigned sp_clk, sp_base;

int mzZ80Run(z80_t *z, int cycles)
{
#if MZ_IDLE_SKIP
  sp_base = sp_clk + (unsigned)cycles;
  const int left = z80core_run(z, cycles);
  sp_clk = sp_base - (unsigned)left;
  return left;
#else
  return z80core_run(z, cycles);
#endif
}

#if MZ_IDLE_SKIP
#define ZF_ 0x40
#define HF_ 0x10
#define PF_ 0x04
#define NF_ 0x02
#define CF_ 0x01
typedef struct { uint16_t t, b; signed char kind, fails; uint8_t len, cnt, nio, ndyn, djnz, rr, m1; short port[2]; int expC; uint8_t code[12]; } MzSpin;
#define MZ_SPIN_N 8
#define SP_PORT_C 0x100
static MzSpin sp_tab[MZ_SPIN_N];
static MzSpin *sp_cur;
static unsigned sp_stamp;
static uint16_t sp_r[11];
static uint8_t sp_pv[2];
uint8_t mzIoPeek(uint8_t port);
static inline int spPeek(uint16_t a, uint8_t *v) { const uint8_t *p = mz_rd_map[a >> 11]; if (!p) return 0; *v = p[a & 0x7FF]; return 1; }
static inline int spPortOK(uint8_t n) { return (n >= 0xBC && n <= 0xBF) || n == 0xC8 || (n >= 0xE0 && n <= 0xE2); }
enum { RB_B = 1, RB_C = 2, RB_D = 4, RB_E = 8, RB_H = 16, RB_L = 32, RB_A = 128, RB_F = 256, RB_SP = 512, RB_ALT = 1024 };
static const unsigned short spRegBit[8] = { RB_B, RB_C, RB_D, RB_E, RB_H, RB_L, RB_H | RB_L, RB_A };
static const unsigned short spPairBit[4] = { RB_B | RB_C, RB_D | RB_E, RB_H | RB_L, RB_SP };
static int spDecode(uint16_t a, int *c, unsigned *rd, unsigned *wr, int *io)
{
  uint8_t op, v;
  *rd = *wr = 0; *io = 0;
  if (!spPeek(a, &op)) return 0;
  if (op == 0xCB) {
    if (!spPeek((uint16_t)(a + 1), &v) || v < 0x40 || v > 0x7F) return 0;
    *c = z80core_cyc_cb[v]; *rd = spRegBit[v & 7]; *wr = RB_F; return 2;
  }
  if (op == 0xED) {
    if (!spPeek((uint16_t)(a + 1), &v) || (v & 0xC7) != 0x40 || v == 0x70) return 0;
    *c = z80core_cyc_ed[v]; *rd = RB_B | RB_C | RB_F; *wr = spRegBit[(v >> 3) & 7] | RB_F; *io = -1; return 2;
  }
  if (op == 0xDB) {
    if (!spPeek((uint16_t)(a + 1), &v) || !spPortOK(v)) return 0;
    *c = z80core_cyc_op[op] + (v == 0xC8 ? 1 : 0); *wr = RB_A; *io = v + 1; return 2;
  }
  *c = z80core_cyc_op[op];
  if (op >= 0x40 && op <= 0x7F) {
    if (op == 0x76 || (op & 0xF8) == 0x70) return 0;
    *rd = spRegBit[op & 7]; *wr = spRegBit[(op >> 3) & 7]; return 1;
  }
  if (op >= 0x80 && op <= 0xBF) {
    const int k = (op >> 3) & 7;
    *rd = RB_A | spRegBit[op & 7] | ((k == 1 || k == 3) ? RB_F : 0); *wr = RB_F | (k == 7 ? 0 : RB_A); return 1;
  }
  switch (op) {
    case 0x00: return 1;
    case 0x07: case 0x0F: *rd = RB_A | RB_F; *wr = RB_A | RB_F; return 1;
    case 0x17: case 0x1F: *rd = RB_A | RB_F; *wr = RB_A | RB_F; return 1;
    case 0x2F: *rd = RB_A | RB_F; *wr = RB_A | RB_F; return 1;
    case 0x37: *rd = RB_A | RB_F; *wr = RB_F; return 1;
    case 0x3F: *rd = RB_A | RB_F; *wr = RB_F; return 1;
    case 0x0A: *rd = RB_B | RB_C; *wr = RB_A; return 1;
    case 0x1A: *rd = RB_D | RB_E; *wr = RB_A; return 1;
    case 0x3A: *wr = RB_A; return 3;
    case 0x2A: *wr = RB_H | RB_L; return 3;
    case 0x01: case 0x11: case 0x21: case 0x31: *wr = spPairBit[op >> 4]; return 3;
    case 0x03: case 0x13: case 0x23: case 0x33: case 0x0B: case 0x1B: case 0x2B: case 0x3B:
      *rd = *wr = spPairBit[(op >> 4) & 3]; return 1;
    case 0xEB: *rd = *wr = RB_D | RB_E | RB_H | RB_L; return 1;
    case 0x08: case 0xD9:
      *rd = *wr = RB_ALT | RB_A | RB_F | RB_B | RB_C | RB_D | RB_E | RB_H | RB_L; return 1;
    case 0xC6: case 0xD6: case 0xE6: case 0xEE: case 0xF6: case 0xFE:
      *rd = RB_A; *wr = RB_F | (op == 0xFE ? 0 : RB_A); return 2;
    case 0xCE: case 0xDE: *rd = RB_A | RB_F; *wr = RB_A | RB_F; return 2;
  }
  if ((op & 0xC6) == 0x04 && (op & 0x38) != 0x30) {
    *rd = *wr = spRegBit[(op >> 3) & 7] | RB_F; return 1;
  }
  if ((op & 0xC7) == 0x06 && op != 0x36) { *wr = spRegBit[(op >> 3) & 7]; return 2; }
  return 0;
}
static __attribute__((noinline)) void spClassify(MzSpin *s, uint16_t t, uint16_t b)
{
  uint8_t op;
  uint16_t a = t, last = t;
  int c = 0, blen, bc, n, ci, io, m1 = 1;
  unsigned rd, wr, rdAll = 0, wrAll = 0, rdOther = 0, wrOther = 0, lastRd = 0, lastWr = 0;
  s->t = t; s->b = b; s->kind = 0; s->fails = 0; s->nio = 0; s->ndyn = 0; s->djnz = 0;
  if (!spPeek(b, &op)) return;
  switch (op) {
    case 0x18: blen = 2; bc = z80core_cyc_op[op]; break;
    case 0x20: case 0x28: case 0x30: case 0x38: blen = 2; bc = z80core_cyc_op[op] + 5; break;
    case 0x10: blen = 2; bc = z80core_cyc_op[op] + 5; s->djnz = 1; break;
    case 0xC3: case 0xC2: case 0xCA: case 0xD2: case 0xDA: case 0xE2: case 0xEA: case 0xF2: case 0xFA:
      blen = 3; bc = z80core_cyc_op[op]; break;
    default: return;
  }
  while (a != b) {
    if ((uint16_t)(b - a) > MZ_SP_MAXLEN) return;
    n = spDecode(a, &ci, &rd, &wr, &io);
    if (!n) return;
    rdOther |= lastRd; wrOther |= lastWr;
    lastRd = rd; lastWr = wr; last = a;
    c += ci; rdAll |= rd; wrAll |= wr;
    spPeek(a, &op);
    m1 += (op == 0xCB || op == 0xED) ? 2 : 1;
    if (io) {
      if (s->nio >= 2) return;
      s->port[s->nio++] = (short)(io < 0 ? SP_PORT_C : io - 1);
      if (io < 0) s->ndyn++;
    }
    a = (uint16_t)(a + n);
  }
  if (s->ndyn && (wrAll & RB_C)) return;
  s->len = (uint8_t)((uint16_t)(b - t) + blen);
  for (int k = 0; k < s->len; k++) if (!spPeek((uint16_t)(t + k), &s->code[k])) return;
  s->expC = c + bc + m1 * Z80CORE_M1_WAIT;
  s->m1 = (uint8_t)m1;
  if (s->djnz) {
    if ((rdAll | wrAll) & (RB_B | RB_ALT)) return;
    s->kind = 3; s->cnt = 0; return;
  }
  op = s->code[(uint16_t)(b - t)];
  if (b - t == 3 && (op == 0x20 || op == 0xC2)) {
    const uint8_t d = s->code[0], v = s->code[1], w = s->code[2];
    if ((d == 0x0B && ((v == 0x78 && w == 0xB1) || (v == 0x79 && w == 0xB0))) ||
        (d == 0x1B && ((v == 0x7A && w == 0xB3) || (v == 0x7B && w == 0xB2))) ||
        (d == 0x2B && ((v == 0x7C && w == 0xB5) || (v == 0x7D && w == 0xB4)))) { s->kind = 2; s->rr = (uint8_t)(d >> 4); return; }
  }
  if ((op == 0x20 || op == 0xC2) && b != t) {
    uint8_t d;
    if (spPeek(last, &d) && (d & 0xC7) == 0x05 && d != 0x35) {
      const unsigned rb = spRegBit[(d >> 3) & 7];
      if (!((rdOther | wrOther) & (rb | RB_ALT))) { s->kind = 3; s->cnt = (uint8_t)((d >> 3) & 7); return; }
    }
  }
  s->kind = 1;
}
static inline void spSave(const z80_t *z, uint16_t *r)
{
  r[0] = z->af.w; r[1] = z->bc.w; r[2] = z->de.w; r[3] = z->hl.w; r[4] = z->ix.w; r[5] = z->iy.w; r[6] = z->sp.w;
  r[7] = z->af2; r[8] = z->bc2; r[9] = z->de2; r[10] = z->hl2;
}
static const signed char spCntWord[8] = { 1, 1, 2, 2, 3, 3, -1, 0 };
static inline uint8_t *spCntPtr(z80_t *z, int r)
{
  switch (r) { case 0: return &z->bc.b.h; case 1: return &z->bc.b.l; case 2: return &z->de.b.h; case 3: return &z->de.b.l;
               case 4: return &z->hl.b.h; case 5: return &z->hl.b.l; default: return &z->af.b.h; }
}
static uint8_t spSz53(uint8_t v) { return (uint8_t)((v & 0xA8) | (v ? 0 : ZF_)); }
__attribute__((noinline)) int IRAM_ATTR mzSpinHook(z80_t *z, uint16_t b, uint16_t t, int cyc)
{
  MzSpin *s = &sp_tab[(t ^ (t >> 3)) & (MZ_SPIN_N - 1)];
  if (s != sp_cur || s->t != t || s->b != b) {
    if (s->t != t || s->b != b) spClassify(s, t, b);
    sp_cur = s; mz_sp_have = 0;
  }
  if (s->kind <= 0) return cyc;
  const unsigned now = sp_base - (unsigned)cyc;
  uint8_t pv[2] = { 0, 0 };
  int pok = 1;
  for (int k = 0; k < s->nio; k++) {
    const uint8_t p = (uint8_t)(s->port[k] == SP_PORT_C ? z->bc.b.l : s->port[k]);
    if (!spPortOK(p)) pok = 0; else pv[k] = mzIoPeek(p);
  }
  const int expC = s->expC + (s->ndyn && z->bc.b.l == 0xC8 ? s->ndyn : 0);
  if (mz_sp_have && now - sp_stamp == (unsigned)expC && cyc > expC) {
    int same = pok;
    for (int k = 0; k < s->len; k++) { uint8_t v; if (!spPeek((uint16_t)(t + k), &v) || v != s->code[k]) { spClassify(s, t, b); mz_sp_have = 0; return cyc; } }
    for (int k = 0; k < s->nio; k++) if (pv[k] != sp_pv[k]) same = 0;
    uint16_t r[11];
    spSave(z, r);
    if (s->kind == 1) {
      for (int k = 0; k < 11; k++) if (r[k] != sp_r[k]) same = 0;
      if (same) {
        const int n = (cyc - 1) / expC;
        cyc -= n * expC; z->r = (uint8_t)(z->r + n * s->m1); s->fails = 0;
      } else if (++s->fails >= 4) { s->kind = 0; return cyc; }
    } else if (s->kind == 2) {
      z80_pair_t *rr = s->rr == 0 ? &z->bc : s->rr == 1 ? &z->de : &z->hl;
      if (rr->w == (uint16_t)(sp_r[s->rr + 1] - 1) && rr->w >= 2) {
        int n = (cyc - 1) / expC;
        if (n > rr->w - 1) n = rr->w - 1;
        if (n > 0) {
          rr->w = (uint16_t)(rr->w - n);
          z->af.b.h = (uint8_t)(rr->b.h | rr->b.l);
          z->af.b.l = (uint8_t)(spSz53(z->af.b.h) | (__builtin_parity(z->af.b.h) ? 0 : PF_));
          z->r = (uint8_t)(z->r + n * s->m1);
          cyc -= n * expC;
        }
      }
    } else {
      const int w = spCntWord[s->cnt];
      const int hi = s->cnt == 7 || !(s->cnt & 1);
      uint8_t *cp = spCntPtr(z, s->cnt);
      const uint8_t prev = (uint8_t)(hi ? (sp_r[w] >> 8) : (sp_r[w] & 0xFF));
      uint16_t m[11];
      for (int k = 0; k < 11; k++) m[k] = 0xFFFF;
      m[w] &= (uint16_t)(hi ? 0x00FF : 0xFF00);
      if (!s->djnz) m[0] &= (uint16_t)(0xFF00 | CF_);
      for (int k = 0; k < 11; k++) if ((r[k] ^ sp_r[k]) & m[k]) same = 0;
      if (same && *cp == (uint8_t)(prev - 1) && *cp >= 2) {
        int n = (cyc - 1) / expC;
        if (n > *cp - 1) n = *cp - 1;
        if (n > 0) {
          *cp = (uint8_t)(*cp - n);
          if (!s->djnz) {
            const uint8_t v = *cp;
            z->af.b.l = (uint8_t)(NF_ | (z->af.b.l & CF_) | spSz53(v) | (v == 0x7F ? PF_ : 0) | ((v & 0x0F) == 0x0F ? HF_ : 0));
          }
          z->r = (uint8_t)(z->r + n * s->m1);
          cyc -= n * expC; s->fails = 0;
        }
      } else if (++s->fails >= 4) { s->kind = 0; return cyc; }
    }
  }
  spSave(z, sp_r);
  sp_pv[0] = pv[0]; sp_pv[1] = pv[1];
  sp_stamp = sp_base - (unsigned)cyc; mz_sp_have = 1;
  return cyc;
}
#endif
