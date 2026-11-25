/* z80core_config.h - MZ-2500: the memory map, I/O and hooks of the Z80 for z80core
   Memory: 2 KB page tables mz_rd_map / mz_wr_map (mz_core.cpp). A NULL page (GVRAM, the VRAM read / write circuit,
   text VRAM, PCG) calls MzRdSlow / MzWrSlow, which take the VRAM wait (until the horizontal blank) from z->icount.
   I/O: InZ80 / OutZ80 (mz_core.cpp) see the cycles left in the line and take their own waits from z->icount.
   Every M1 has one wait state (6 MHz; the old Z80 core had it in its cycle tables).
   RETI frees the in-service device of the daisy chain (mzReti). After EI and the next instruction the interrupt
   controller is asked once (mzIrqTake: vector or -1), as in the PC-8801 version.
   The ED opcodes run from IRAM (LDIR / LDI to the VRAM).
   MZ_IDLE_SKIP 1: a taken JP / JR / DJNZ back by at most MZ_SP_MAXLEN bytes calls the idle-loop skipper (mz_spin.c);
   0 = run every loop instruction by instruction. The results are the same either way. */
#ifndef Z80CORE_CONFIG_H
#define Z80CORE_CONFIG_H
#include <stdint.h>

#ifndef MZ_IDLE_SKIP
#define MZ_IDLE_SKIP 1
#endif
#define MZ_SP_MAXLEN 8

extern uint8_t *mz_rd_map[], *mz_wr_map[];
uint8_t MzRdSlow(uint16_t addr);
void MzWrSlow(uint16_t addr, uint8_t value);
uint8_t InZ80(uint16_t port);
void OutZ80(uint16_t port, uint8_t value);
int mzIrqTake(void);
void mzReti(void);
extern int mz_sp_have;
int mzSpinHook(struct z80 *z, uint16_t b, uint16_t t, int cyc);

#define Z80CORE_M1_WAIT 1
#define Z80CORE_RD(a) ({ const uint8_t *pr_ = mz_rd_map[(a) >> 11]; uint8_t vr_; \
  if (__builtin_expect(pr_ != 0, 1)) vr_ = pr_[(a) & 0x07FF]; else { Z80CORE_SYNC_OUT_MEM(); vr_ = MzRdSlow(a); Z80CORE_SYNC_IN_MEM(); } vr_; })
#define Z80CORE_WR(a, v) do { uint8_t *pw_ = mz_wr_map[(a) >> 11]; \
  if (__builtin_expect(pw_ != 0, 1)) pw_[(a) & 0x07FF] = (v); else { Z80CORE_SYNC_OUT_MEM(); MzWrSlow((a), (v)); Z80CORE_SYNC_IN_MEM(); } } while (0)
#define Z80CORE_IN(p) ({ Z80CORE_SYNC_OUT(); const uint8_t vi_ = InZ80(p); Z80CORE_SYNC_IN(); vi_; })
#define Z80CORE_OUT(p, v) do { Z80CORE_SYNC_OUT(); OutZ80((p), (v)); Z80CORE_SYNC_IN(); } while (0)
#define Z80CORE_RETI() mzReti()
#define Z80CORE_EI() (z->irq = 1)
#define Z80CORE_IRQ_ACK() ({ Z80CORE_SYNC_OUT(); const int va_ = mzIrqTake(); Z80CORE_SYNC_IN(); z->irq = 0; va_; })
#if MZ_IDLE_SKIP
#define Z80CORE_SPIN(b, t) do { if ((uint16_t)((b) - (t)) <= MZ_SP_MAXLEN) { Z80CORE_R_OUT(); cyc = mzSpinHook(z, (b), (t), cyc); Z80CORE_R_IN(); } } while (0)
#define Z80CORE_ON_INT() (mz_sp_have = 0)
#endif
#if defined(ARDUINO) && !(defined(BOARD_HAS_PSRAM) && !defined(EMU_PSRAM_NO_ERRATA_FIX))
#include <esp_attr.h>
#define Z80CORE_ED_ATTR IRAM_ATTR __attribute__((noinline))
#endif

#endif
