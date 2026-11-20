# Sharp MZ-2500 (SuperMZ) Emulator for ESP32

[ [English](#english) | [日本語](#日本語) ]

---

<a id="english"></a>
# English

An emulator for the Sharp MZ-2500 (SuperMZ) using ESP32 and the **FabGL** library (Work in Progress). It supports VGA output (640x400, 16 colours), a PS/2 keyboard and mouse, D88 disk images, the YM2203 sound and a serial-connected external joystick.

## 📝 Overview

- **Frame rate**: 55.5 Hz, the same as an MZ-2500 with its high-resolution (24 kHz) monitor (448 lines per frame, 400 displayed). VGA output is 640x400 with the 640x480 horizontal timing (31.5 kHz).
- **Emulated** (MZ-2500 mode): Z80 at 6 MHz with the one wait state on every opcode fetch and the VRAM access waits, 8 KB x 8 bank memory (B4h/B5h: main RAM 256 KB, graphic VRAM 128 KB, IPL ROM, text VRAM, PCG, kanji ROM and dictionary ROM windows), interrupt daisy chain (Z80 PIO → SIO → interrupt controller C6h/C7h), keyboard through the Z80 PIO, i8255, i8253 timer, RP5C15 clock, YM2203 (FM 3 ch + SSG 3 ch, 2 MHz) and the 1-bit beeper, MB8876 FDC with D88 images (two internal drives), joystick port (EFh), mouse (Z80 SIO channel B).
- **Display**: text in 80 / 40 columns with kanji and PCG (including 8-colour PCG and the 40-column two-screen modes), graphics in 640x400 16 / 4 colours, 640x200 16 colours and 320x200 16 colours (two screens), VRAM read-modify-write circuit, text and graphics hardware scroll (including horizontal scroll), palette and priority.
  - The 4096-colour palette is supported; the TTGO VGA32 can show 64 colours, so its colours are rounded to the nearest of them.
  - The 256-colour modes (320x200 / 320x400) are shown with a 2x2 pattern of 4 colours per dot (an ordered dither). In these modes the 16 output colours are switched to the CGA 16 colours, which the TTGO VGA32 shows exactly, and for each of the 512 RGB values the 4 colours whose average is closest (weighted by brightness) are used, so intermediate colours such as skin tones and sky blue come out close. Text shown over these modes uses the nearest CGA colour. Set `MZ_TILE256` in `mz.h` to 0 to use the single nearest colour instead.
- **Drive swap**: bit 1 of YM2203 port A swaps drives 1/2 with 3/4, as on the real machine.
- **Not emulated**: MZ-80B / MZ-2000 modes, cassette tape, RS-232C, printer, 2HD disks.
- **Audio**: 16384 Hz (FabGL default). FM and SSG are mixed with the same balance as Common Source Project (SSG 8 dB below FM).
- **Speed**:
  - Only the lines whose content changed since they were last drawn are drawn again (the VRAM, text and PCG writes, the display registers and the colours are tracked for each line). A frame in which nothing on the screen changed is not drawn at all.
  - Lines are drawn 8 dots at a time, and the graphics / text of the previous line are reused when they are the same. When a line would come out exactly the same as the line above (200-line graphics and doubled 8-line characters), the line above is copied instead of being drawn.
  - When only the text of a line changed, that line of text is built first, and the line is not drawn again if it comes out the same (characters and PCG often change without changing a given line). For each row of text, the characters that can show dots are listed once, and only those are built on each line.
  - When a frame runs late, its drawing is skipped (never two frames in a row), so the software keeps the real machine's speed.
- **Status**: work in progress. Runs on real hardware; speed tuning is still going on.

> ⚠️ **Notice Regarding Development & Licenses**
>
> Most of the main source code is built and modified based on AI-generated code. Therefore, the developer does not completely grasp all internal logic and cannot answer detailed questions regarding source code specifications. Thank you for your understanding.
> Please refer to the header comments at the beginning of the source code for license details and agree to them before use.

---

## 🤝 Credits & Acknowledgments

- **Graphics/Audio Engine**: Uses [FabGL](https://github.com/fdivitto/FabGL) by Fabrizio Di Vittorio.
- **Z80 CPU Core**: z80core, written for this collection (MIT License, Copyright Kure Ichiro; `z80core.c` / `z80core.h`).
- **Video, FDC, I/O and the rest**: newly written for this project. The hardware behaviour follows **Common Source Project** (EmuZ-2500, TAKEDA, toshiya) and **MAME** (mz2500.cpp). The YM2203 FM part is shared with the PC-8801 emulator.

### 📜 Core Licenses
The Z80 core (z80core) is under the **MIT License**; FabGL follows its own license.

---

## 🛠️ Environment & Build Settings

- **Board Manager**: ESP32 by Espressif Systems (**v2.0.5 Recommended**)
- **Library**: FabGL v1.0.9, **with one file replaced** (required): copy `fabgl_patch/displaycontroller.cpp` over `FabGL/src/displaycontroller.cpp` in your Arduino libraries folder. The stock FabGL keeps its drawing routines in the ESP32's internal instruction RAM (IRAM), and with them this sketch does not fit (the link fails with "iram0_0_seg overflowed"). The patched file only keeps those routines in flash instead (about 8 KB of IRAM freed); nothing else changes, and other FabGL sketches keep working. It stays under FabGL's GPL v3 license (see `fabgl_patch/LICENSE`).
- **Board**: `ESP32 Dev Module` (TTGO VGA32)
- **PSRAM** (ON = `"Enabled"`, OFF = `"Disabled"`): With PSRAM ON, a software workaround for the chip errata (Espressif errata CPU-3.9) is built in, and the speed drops by about 20%. With PSRAM OFF, the sketch enables PSRAM by itself and the software workaround is not built in. However, this causes failures unless the ESP32 is chip revision 3 or later, so on any other revision a PSRAM OFF build is made not to start. This project uses frame skipping to keep the feel close to the real machine, but with PSRAM ON it can still slow down. Turning PSRAM OFF brings it closer to 55 FPS.
- **Partition Scheme**: the default is fine.
- **SD card pins** (`SD_SCLK` / `SD_MISO` / `SD_MOSI` / `SD_CS` near the top of the `.ino`): they differ by board. The default is TTGO VGA32 (14 / 2 / 12 / 13). ORANGE ESPer: 14 / 35 / 12 / 13. Switch Science FabGL development board: 14 / 35 / 2 / 13. The FabGL development board wires the SD card to MISO = 16 and MOSI = 17, but on an ESP32-WROVER these two pins are not connected (they are used inside the module for the PSRAM), so jumper 16 → 35 and 17 → 2 on the FabGL board. Enable the block for your board and comment out the others.
- **Switching machines with EmuLauncher** (optional): copy `emu_switch.h` and `partitions.csv` from the `EmuLauncher` folder into this sketch folder. The boot menu then gets **Next** (the machine to start next), and the flash is laid out as in `partitions.csv` (the Partition Scheme setting is not used). Without the two files the sketch works on its own as before. See `EmuLauncher/README.md`.

---

## 📂 SD Card Structure

```text
/ (SD Root)
└── /MZ25
    ├── IPL.ROM          : IPL ROM 32KB (required)
    ├── KANJI.ROM        : Kanji ROM 256KB (without it kanji are not shown)
    ├── DICT.ROM         : Dictionary ROM 256KB (optional)
    ├── PHONE.ROM        : 32KB ROM at pages 3Ch-3Fh (optional; FILE.ROM is also accepted)
    ├── JOYKEY.CFG       : JoyToKey settings (created automatically)
    └── <folders>/*.D88  : Disk images (sub-folders are allowed)
```

MSX1 uses `/MSX1`, MSX2 `/MSX2`, Mark III `/SMS`, PC-8801 `/PC88` and X1 `/X1`, so one card can hold every machine.

---

## 🚀 Boot Menu

| Item | Description |
| :--- | :--- |
| FD1 / FD2 | Choose a D88 for each drive. For a file containing several disks, choose which one. If FD1 is such a file and FD2 is empty, the next disk goes to FD2 automatically. |
| JoyToKey | ON: the directions of the 1P controller press numeric keypad 1-9 (diagonals = 1, 3, 7, 9), and buttons 1-3 press the keys below. While ON, nothing is connected to joystick port 1. |
| Joy B1-B3 | Keys pressed by buttons 1-3 with JoyToKey (default SPACE / RETURN / ESC). Press Enter, then the key to assign (F12 cancels). Saved to `/MZ25/JOYKEY.CFG`. |
| LatchCpy | Only when built with `MZ_LATCH_COPY` = 1 (see For Developers). ON / OFF, default OFF. Not saved. |
| Next | The machine to start next. Shown only with EmuLauncher's two files (see Build Settings). Enter changes it; it takes effect at the next power-on or reset. |
| Run | Start |

The boot menu and the disk selectors can also be operated with the 1P controller: Up/Down = select (repeats while held), Left/Right = page up/down, button 1 = Enter, button 2 = cancel (ESC).

---

## 🕹️ Operations Guide

- **`[F12]`** : Pause / Resume
- **`[Shift] + [F12]`** : Reset (reboots the ESP32 and returns to the boot menu)
- **While paused `[F10]` / `[F11]`** : Change the disk in FD1 / FD2 (`<NONE>` ejects it)
- **While paused `[Shift] + [F10]` / `[Shift] + [F11]`** : Write FD1 / FD2 back to the SD card (only that disk inside a multi-disk file is overwritten)
- When not paused, F1-F10 are the MZ-2500 function keys and F11 is HELP.

### Keys that work differently from their keytop (Japanese 106 keyboard)

| PC keyboard | MZ-2500 |
| :--- | :--- |
| End | BREAK |
| PrintScreen | COPY |
| Home | CLR |
| Insert | INST |
| BackSpace / Delete | DEL |
| Alt | GRPH |
| CapsLock | LOCK (the Caps Lock LED shows it) |
| Katakana/Hiragana | KANA (the Scroll Lock LED shows it) |
| Muhenkan / Henkan | KJ1 / KJ2 |
| Hankaku/Zenkaku | ARGO (logo) key |
| Ro (`\`) | `_` |
| Numeric keypad Enter | RETURN |
| F11 | HELP |
| F12 | Emulator operations (above); not sent to the MZ-2500 |

The numeric keypad `,` key of the MZ-2500 is not on the 106 keyboard. LOCK and KANA are ordinary keys on the MZ-2500 (the software keeps their on/off state), so the LEDs are simply toggled on each press; they can disagree with the software after a reset or when the software switches the mode by itself.

---

## 🖱️ Mouse

A PS/2 mouse on the mouse port of the board is connected to the emulated mouse (Z80 SIO channel B, as in Common Source Project). When software selects the mouse (bit 3 of YM2203 port A) and raises DTR of SIO channel B, three bytes (buttons, X movement, Y movement) are received at the speed set through port CDh. The SIO appears at A0h-A3h or B0h-B3h as selected by bit 7 of CDh. Checked on the PC test harness; not yet tested on the ESP32.

## 🔌 Joystick (External Controller)

Same converter and protocol as the MSX / Mark III / PC-8801 / X1 emulators: serial `IO39`, 115200 bps, 1 byte per port (b7 = port, b6-b4 = buttons 3-1, b3-b0 = right/left/down/up, active low). Ports 1 and 2 are read through the joystick port (EFh): directions and triggers A / B (buttons 1 / 2).

---

## 🖥️ Display Settings (`MZ2500.ino`)

- `MZ_VGA_HZ`: vertical frequency of the VGA output, 55 (default, the real machine's speed) / 56 / 57 / 60. Use a higher value only for a monitor that does not accept 55.5 Hz; the software then runs faster by the same ratio.
- The pause message is shown at the bottom of the screen with the darkest and brightest of the current colours.

---

## 🔧 For Developers

- With `USE_SERIAL_DEBUG` (on by default), the serial log (115200 bps) shows a `SPEED` and a `PROF` line every 5 seconds: Z80 and drawing time per frame, drawn / unchanged / skipped frames, how many lines took the fast drawing path, how many lines were left as they were because nothing in them changed (`unchanged`), how many lines were copied from the line above (`copied`), how many odd lines of a 200-line screen showed the even line's buffer (`paired`), and how many lines took the graphics of their row from the row cache instead of reading GVRAM (`cgCache`).
- `MZ_LATCH_COPY` (`mz.h`, default 0): an extension that the real machine does not have. With 1, the boot menu shows `LatchCpy` (ON / OFF, default OFF). When ON, every read through the graphics read/write window (pages 30h–33h) copies the four latched planes into the write registers (CG registers 0–3), so one `LDI` copies all four planes from VRAM to VRAM. When not comparing (CG register 7 bit4 = 0), the byte read is the OR of the four planes (1 = a dot that is not colour 0) instead of the selected plane. The byte written (= the byte read) works as the mask of the write mode, so PSET copies with colour 0 transparent, and REPLACE (CG register 6 = FFh) copies everything. With comparing, the byte read is the same as on the real machine. Ordinary software behaves differently while it is ON.

---

## Disclaimer

IN NO EVENT SHALL FABRIZIO DI VITTORIO OR THE DEVELOPER BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, OR CONSEQUENTIAL DAMAGES ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE. USE AT YOUR OWN RISK.

---

<a id="日本語"></a>
# 日本語

ESP32 と **FabGL** ライブラリを使った シャープ MZ-2500（SuperMZ）のエミュレータです（開発中）。VGA 出力（640×400・16 色）、PS/2 キーボード・マウス、D88 ディスクイメージ、YM2203 の音、シリアル接続の外部ジョイスティックに対応しています。

## 📝 概要

- **フレームレート**: 55.5Hz（高解像度（24kHz）モニタをつないだ MZ-2500 と同じ。1 フレーム 448 ライン・表示 400 ライン）。VGA の出力は 640×400 で、横は 640×480 と同じタイミング（31.5kHz）です。
- **再現しているもの**（MZ-2500 モード）:
  - Z80 6MHz（命令を読むたびの 1 ウエイトと、VRAM のアクセスのウエイトも）
  - 8KB × 8 のバンクメモリ（B4h/B5h。メイン RAM 256KB、グラフィック VRAM 128KB、IPL ROM、テキスト VRAM、PCG、漢字 ROM・辞書 ROM の窓）
  - 割込みのデイジーチェーン（Z80 PIO → SIO → 割込みコントローラ C6h/C7h）
  - Z80 PIO によるキーボード、i8255、i8253 タイマー、RP5C15 時計
  - YM2203（FM 3ch + SSG 3ch、2MHz）と 1 ビットの音
  - MB8876 FDC と D88 イメージ（内蔵 2 ドライブ）
  - ジョイスティック端子（EFh）
  - マウス（Z80 SIO のチャンネル B）
- **画面**:
  - テキストは 80 / 40 桁、漢字、PCG（8 色の PCG、40 桁の 2 画面の重ね合わせも）です。
  - グラフィックは 640×400 の 16 / 4 色、640×200 の 16 色、320×200 の 16 色（2 画面）です。
  - VRAM の読み書き回路、テキストとグラフィックのハードウェアスクロール（横スクロールも）、パレットと優先順位にも対応しています。
  - 4096 色のパレットにも対応しています。TTGO VGA32 は 64 色なので、いちばん近い色に丸めて表示します。
  - 256 色のモード（320×200 / 320×400）は、1 ドットを 4 色の 2×2 の模様（ディザ）で表示します。このモードのときは出力の 16 色を CGA の 16 色（TTGO VGA32 でそのまま出せる色）に切り替え、512 通りの色それぞれについて、4 色の平均が明るさの重みでいちばん近くなる組を使うので、肌色や空色などの中間の色も近い色になります。このモードの上に重なるテキストは、いちばん近い CGA の色になります。`mz.h` の `MZ_TILE256` を 0 にすると、いちばん近い 1 色で表示します。
- **ドライブの入れ替え**: 実機と同じく、YM2203 のポート A の bit1 でドライブ 1・2 と 3・4 が入れ替わります。
- **再現していないもの**: MZ-80B / MZ-2000 モード、カセットテープ、RS-232C、プリンタ、2HD のディスク。
- **サウンド**: 16384Hz（FabGL の標準）。FM と SSG の音量の比は Common Source Project と同じです（SSG は FM より 8dB 小さい）。
- **速さの工夫**:
  - 前に描いたときから中身が変わったラインだけを描き直します（VRAM・テキスト・PCG の書き換え、表示のレジスタ、色をラインごとに見ています）。画面が何も変わっていないフレームは描きません。
  - ラインは 8 ドットずつまとめて描き、前のラインと同じグラフィック・テキストは作り直しません。すぐ上のラインとまったく同じになるライン（200 ラインのグラフィックと、8 ラインの文字を 2 倍にしたテキスト）は、描かずに上のラインを写します。
  - テキストだけが変わったラインは、先にそのラインの文字を作り、前と同じ絵なら描き直しません（文字や PCG が変わっても、そのラインの絵は同じことが多いため）。文字の行ごとに、点が出うる文字の一覧を 1 回だけ作り、毎ラインその文字だけを作ります。
  - フレームが遅れたときはそのフレームの描画を飛ばすので（2 フレーム続けては飛ばさない）、ソフトは実機の速さで動きます。
- **動作状況**: 開発中です。実機で動いていて、速さの調整を続けています。

> ⚠️ **開発・ライセンスに関するご注意**
>
> メインソースコードの大部分は AI 生成コードをベースに構築・改変しているため、開発者もすべてのロジックを完全に把握しているわけではありません。そのため、ソースコードの詳細な仕様や内部ロジックに関する質問にはお答えできかねます。あらかじめご了承ください。
> ライセンスの詳細な条件はソースコード冒頭のヘッダコメントを参照し、同意の上でご利用ください。

---

## 🤝 クレジットと謝辞

- **グラフィック/オーディオエンジン**: Fabrizio Di Vittorio 氏による [FabGL](https://github.com/fdivitto/FabGL) を使用。
- **Z80 CPU コア**: このシリーズのために書いた z80core（MIT ライセンス、Copyright Kure Ichiro。`z80core.c`・`z80core.h`）。
- **画面・FDC・I/O など**: 本プロジェクトで新規に作成。実機の動作は **Common Source Project**（EmuZ-2500。TAKEDA, toshiya 氏）と **MAME**（mz2500.cpp）に合わせています。YM2203 の FM の部分は PC-8801 版と共通です。

### 📜 各コアのライセンスについて
Z80 コア（z80core）は **MIT ライセンス**、FabGL はそのライセンスに従います。

---

## 🛠️ 開発環境とビルド設定

- **ボードマネージャ**: ESP32 by Espressif Systems（**v2.0.5 推奨**）
- **ライブラリ**: FabGL v1.0.9。**ファイルを 1 つ差し替えてください**（必須）: Arduino のライブラリのフォルダの `FabGL/src/displaycontroller.cpp` を、`fabgl_patch/displaycontroller.cpp` で上書きします。元の FabGL は描画の処理を ESP32 の内部の命令 RAM（IRAM）に置くため、そのままではこのスケッチが入りません（リンクで「iram0_0_seg overflowed」になります）。差し替えたファイルはその処理をフラッシュに置くだけで（IRAM が約 8KB 空きます）、ほかは何も変わらず、ほかの FabGL のスケッチもそのまま動きます。ライセンスは FabGL と同じ GPL v3 です（`fabgl_patch/LICENSE`）。
- **ボード**: `ESP32 Dev Module`（TTGO VGA32）
- **PSRAM**（ON = `"Enabled"`、OFF = `"Disabled"`）: PSRAMをONにするとソフトでのエラッタ対策が入り、20%程度速度が低下します。PSRAMをOFFにするとソフトでPSRAMを有効にし、ソフトでのエラッタ対策は入りません。但し、ESP32のリビジョンが3以降でないと不具合が発生するため、それ以外のリビジョンでPSRAMをOFFにしても起動しなくしてあります。本プロジェクトではフレームスキップで実機に近い操作感を実現していますが、PSRAMがONの時は処理落ちすることがあります。PSRAMをOFFにすることでより55FPSに近くなります。
- **Partition Scheme**: 既定のままでかまいません。
- **SD カードのピン**（`.ino` の先頭近くの `SD_SCLK` / `SD_MISO` / `SD_MOSI` / `SD_CS`）: ボードによって違います。既定は TTGO VGA32（14 / 2 / 12 / 13）です。ORANGE ESPer は 14 / 35 / 12 / 13、スイッチサイエンスの FabGL 開発ボードは 14 / 35 / 2 / 13 です。FabGL 開発ボードは SD カードを MISO = 16、MOSI = 17 で配線していますが、ESP32-WROVER ではこの 2 本はモジュールの中で PSRAM に使われていて外にはつながっていない（NC）ため、FabGL ボード上で 16 → 35、17 → 2 にジャンパしてください。使うボードのブロックを有効にし、ほかはコメントにしてください。
- **EmuLauncher での機種の切り替え**（使うときだけ）: `EmuLauncher` のフォルダの `emu_switch.h` と `partitions.csv` を、このスケッチのフォルダにコピーします。起動メニューに **Next**（次に起動する機種）が出て、フラッシュの分け方が `partitions.csv` のとおりになります（Partition Scheme の設定は使われません）。2 つのファイルが無ければ、今までどおり単体で動きます。詳しくは `EmuLauncher/README.md` を見てください。

---

## 📂 SD カードの構成

```text
/ (SD ルート)
└── /MZ25
    ├── IPL.ROM          : IPL ROM 32KB（必須）
    ├── KANJI.ROM        : 漢字 ROM 256KB（ないと漢字が出ない）
    ├── DICT.ROM         : 辞書 ROM 256KB（なくてもよい）
    ├── PHONE.ROM        : ページ 3Ch〜3Fh の 32KB の ROM（なくてもよい。FILE.ROM という名前でもよい）
    ├── JOYKEY.CFG       : JoyToKey の設定（自動で作られます）
    └── <フォルダ>/*.D88 : ディスクイメージ（フォルダ分けしてよい）
```

MSX1 は `/MSX1`、MSX2 は `/MSX2`、マーク III は `/SMS`、PC-8801 は `/PC88`、X1 は `/X1` なので、1 枚のカードに全機種を入れられます。

---

## 🚀 起動メニュー

| 項目 | 内容 |
| :--- | :--- |
| FD1 / FD2 | 各ドライブの D88 を選ぶ。複数枚入ったファイルは何枚目かを選ぶ。FD1 に複数枚入ったファイルを選び FD2 が空なら、次の 1 枚を自動で FD2 に入れる。 |
| JoyToKey | ON: 1P のコントローラの方向でテンキーの 1〜9（斜めは 1・3・7・9）、ボタン 1〜3 で下の Joy B1〜B3 のキーを押す。ON の間、ジョイスティック端子 1 には何もつながっていないことになる。 |
| Joy B1〜B3 | JoyToKey のボタン 1〜3 で押すキー（既定は SPACE / RETURN / ESC）。Enter を押してから割り当てるキーを押す（F12 で取り消し）。`/MZ25/JOYKEY.CFG` に保存。 |
| LatchCpy | `MZ_LATCH_COPY` を 1 にして組み立てたときだけ出ます（「開発する人向け」を参照）。ON / OFF（既定 OFF）。保存しません。 |
| Next | 次に起動する機種。EmuLauncher の 2 つのファイルを置いたときだけ出ます（ビルド設定を参照）。Enter で変わり、電源を入れ直すかリセットすると切り替わります。 |
| Run | 起動 |

起動メニューとディスクの選択画面は、1P のコントローラでも操作できます：↑↓ = 選択（押し続けると連続）、←→ = 1画面送り、ボタン 1 = 決定（Enter）、ボタン 2 = 取り消し（ESC）。

---

## 🕹️ 操作方法

- **`[F12]`** : 一時停止 / 再開
- **`[Shift] + [F12]`** : リセット（ESP32 ごと再起動して起動メニューへ）
- **一時停止中に `[F10]` / `[F11]`** : FD1 / FD2 のディスクを入れ替え（`<NONE>` で取り出し）
- **一時停止中に `[Shift] + [F10]` / `[Shift] + [F11]`** : FD1 / FD2 の内容を SD へ書き戻し（複数枚入ったファイルは、そのディスクの部分だけ上書き）
- 一時停止していないときは、F1〜F10 が MZ-2500 のファンクションキー、F11 が HELP です。

### 印字と機能が違うキー（日本語 106 キーボード）

| PC のキーボード | MZ-2500 |
| :--- | :--- |
| End | BREAK |
| PrintScreen | COPY |
| Home | CLR |
| Insert | INST |
| BackSpace / Delete | DEL |
| Alt | GRPH |
| CapsLock | LOCK（Caps Lock のランプが連動） |
| カタカナひらがな | カナ（Scroll Lock のランプが連動） |
| 無変換 / 変換 | KJ1 / KJ2 |
| 半角/全角 | アルゴキー（ロゴのキー） |
| ろ（`\`） | `_` |
| テンキーの Enter | RETURN |
| F11 | HELP |
| F12 | エミュレータの操作（上の一覧）。MZ-2500 には送りません |

MZ-2500 のテンキーの `,` は 106 キーボードにありません。MZ-2500 の LOCK とカナはふつうのキーで、入/切はソフトが覚えているため、ランプは押すたびに切り替えているだけです。リセットのあとや、ソフトが自分で切り替えたときは、ソフトの状態と合わないことがあります。

---

## 🖱️ マウス

ボードのマウス端子につないだ PS/2 マウスを、MZ-2500 のマウス（Z80 SIO のチャンネル B。Common Source Project と同じつなぎ方）として扱います。ソフトがマウスを選び（YM2203 のポート A の bit3）、SIO のチャンネル B の DTR を立てると、3 バイト（ボタン・X の移動量・Y の移動量）がポート CDh で決めた速さで届きます。SIO は CDh の bit7 で A0h〜A3h か B0h〜B3h に出ます。PC の検証環境では確認済みで、ESP32 での確認はまだです。

## 🔌 ジョイスティック（外部コントローラ）

MSX・マーク III・PC-8801・X1 版と同じ変換器・同じ通信仕様です：シリアル `IO39`、115200bps、ポートごとに 1 バイト（b7 = ポート、b6〜b4 = ボタン 3〜1、b3〜b0 = 右/左/下/上、0 = 押下）。ポート 1・2 はジョイスティック端子（EFh）から読みます（方向とトリガ A / B = ボタン 1 / 2）。

---

## 🖥️ 画面の設定（`MZ2500.ino`）

- `MZ_VGA_HZ`: VGA の縦の周波数。55（既定。実機と同じ速さ）/ 56 / 57 / 60。55.5Hz が映らないモニタのときだけ大きくしてください（そのぶんソフトも速くなります）。
- 一時停止の表示は、今の色のうちいちばん暗い色といちばん明るい色で、画面のいちばん下に出ます。

---

## 🔧 開発する人向け

- `USE_SERIAL_DEBUG`（既定で有効）のとき、シリアル（115200bps）に 5 秒ごとに `SPEED` と `PROF` の行が出ます。内容は次のとおりです。
  - 1 フレームあたりの Z80 と描画の時間
  - 描いたフレーム / 変化がなく描かなかったフレーム / 遅れて飛ばしたフレームの数
  - 速い描画で描いたラインの数と、中身が変わっていないので描かなかったラインの数（`unchanged`）、すぐ上のラインを写したラインの数（`copied`）、200 ラインの画面で偶数ラインのバッファを出した奇数ラインの数（`paired`）、GVRAM を読まずに行の作り置きからグラフィックを取ったラインの数（`cgCache`）
- `MZ_LATCH_COPY`（`mz.h`。既定 0）: 実機にない拡張です。1 にすると起動メニューに `LatchCpy`（ON / OFF。既定 OFF）が出ます。ON のときは、グラフィックの読み書き回路（ページ 30h〜33h）で読むたびに、ラッチした 4 プレーンを書き込みのレジスタ（CG レジスタ 0〜3）へ写します。これで `LDI` 1 回で VRAM から VRAM へ 4 プレーンを転送できます。比較しないとき（CG レジスタ 7 の bit4 = 0）の読んだ値は、選んだ 1 プレーンではなく 4 プレーンの OR（色 0 以外のドットが 1）になります。書く値（= 読んだ値）は書き込みのモードのマスクとして働くので、PSET なら色 0 を透明にした転送、REPLACE（CG レジスタ 6 = FFh）ならそのままの転送になります。比較するときの読んだ値は実機どおりです。ON の間は普通のソフトの動きが変わります。

---

## 免責事項

本ソフトウェアの使用によって生じたいかなる損害についても、FABRIZIO DI VITTORIO 氏および開発者は一切の責任を負いません。自己責任でご利用ください。
