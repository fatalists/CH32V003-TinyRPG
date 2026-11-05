# Notes for AI coding assistants

Tiny RPG is an action RPG for the CH32V003 Mini Game Console. The player-facing
description is in [README.md](README.md). This file covers what you need before
changing the code.

## Hardware

| | |
|---|---|
| MCU | CH32V003J4M6, QingKe V2A, RV32EC + WCH's XW extension, 12 MHz |
| Memory | 16 KB flash (the last 64-byte page is the save slot), 2 KB SRAM |
| Display | SSD1306 128x64 mono over I2C, drawn from a 1 KB page-major framebuffer `fb` |
| Input | joystick on one ADC pin (resistor ladder, constants `JOY_N`...`JOY_NW` in `src/driver.h`), ACT on its own GPIO |
| Sound | passive buzzer, `JOY_sound()` blocks while it plays |

## Build

- Use WCH's GCC (`riscv32-wch-elf`, MounRiver Studio 2) with its `bin` folder on `PATH`.
  `-march=rv32ecxw` is required: without XW the image is 732 bytes too big.
- Run `make all` and read the `FLASH:` line after **every** change. The image is
  **16316 of 16320 bytes**, so there are 4 bytes to spare. A change that does not fit
  has to pay for itself somewhere else.
- `bin/tiny_rpg.bin` and `.hex` are the released firmware. Rebuild and commit them
  together with the source change that produced them.

## Generated files: do not edit by hand

```
map/*.txt          -> tools/mapconv.py  -> src/worldmap.h    (PackBits RLE per row)
tools/gentiles.py                       -> src/worldtiles.h  (tiles, hero, monsters)
tools/genui.py                          -> src/ui_art.h      (font, banner, item icons)
```

The art lives as ASCII drawings inside the two Python generators. `mapconv.py` needs
all six maps in order (see README) and reports walkable tiles that cannot be reached
from the spawn. Map legend and metadata (`;zone`, `;mobs`, `;boss`, `;door`...) are
documented at the top of `map/world.txt` and in `tools/mapconv.py`.

## Code layout

Everything is in `src/main.c`. The other files in `src/` are Stefan Wagner's driver
layer and WCH's device header, and they don't need to change. `src/rle.S` is the map
row decoder in assembly.

Look in `main.c` for:

- **Tables at the top:**
  - items: `item_names`, `items[]`, `item_price[]`, `shop_id[]`;
  - classes: `class_stat`;
  - monsters: `mon_hp0`, `mon_str0`, `mon_def0`, `mon_xp0`, `mon_flag`;
  - loot: `drop_ofs` / `drop_tab`.
- **State:** `PLAY P` is the whole run, in save-page order. `save_write()` copies it
  in one go, and `save_read()` checks every field.
- **Formulas:** `stat_of`, `maxhp`, `dmg_roll`, `xp_need`, `atk_gain`.
- **World:** `zone_enter`, `draw_world`, `mon_act` (chase AI with a held sidestep
  around pillars), `do_attack`, `shot_step`.
- **Screens:** `screen_title`, `screen_class`, `screen_character`, `screen_inventory`,
  `screen_shop`.

## Rules that are easy to break

- **No division or modulo.** RV32EC has no M extension, and pulling in `__udivsi3`
  does not fit. Use shifts, masks and accumulators (see `walk_acc`, `atk_acc`, `regen`).
- **No floats and no libc.** The build is `-nostdlib`.
- `xp_need()` returns `uint16_t`, and the raw curve overflows at level 63. `XP_LAST`
  must stay at 62 or below.
- `plvl` must never be 0: `xp_need(0)` wraps its loop counter and returns garbage,
  and the HUD calls it every frame.
- The flash controller programs at `0x08003FC0`, while the CPU reads the same page
  through the alias at `0x00003FC0`. Writing through the alias silently does nothing.
- Read `ADC_read()` once per frame. Every `JOY_*_pressed()` call starts a new conversion.
- On this 16-register core, a helper shared by two call sites is often **larger** than
  the duplicated code. Measure before refactoring for size. Sweeping
  `__attribute__((noinline))` per function and re-checking the `-fno-tree-*` flags in
  the makefile has paid off more than once.
- Keep the code's comment style: comments say why, in full sentences.

## Host build

`main.c` has `SIM_HOST` / `SAVE_ADDR` hooks for compiling it on a PC, with a stand-in
`driver.h` that supplies input and captures `fb`. Use such a build to test whole game
paths, such as starting a new game, dying, saving and loading. Testing a single helper
is not enough. Run it under `-fsanitize=address,undefined`.
