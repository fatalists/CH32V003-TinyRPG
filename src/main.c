// ===================================================================================
// Project:   Tiny RPG
// Version:   v1.0
// Year:      2026
// Authors:   Sergey S. and Claude Opus 5
// ===================================================================================
//
// A small action RPG for the CH32V003 Mini Game Console, in 16 KB of flash:
//   * title screen with a save-aware Continue, and two classes: a warrior with a
//     sword and a mage with a staff that fires bolts
//   * six hand-drawn zones (meadow and town, desert, snowfield, Valhalla and two
//     boss caves) with a pixel-accurate scrolling camera
//   * nine kinds of monster and three bosses; Malenia in Valhalla ends the game
//   * character screen on a long press of ACT: four stats, inventory with a 2x2
//     paperdoll and a 4x3 bag, and SAVE AND EXIT, the only place the game saves
//   * a shop in town that buys and sells, timed stat scrolls, levels up to 100
//   * permadeath: dying erases the save unless an aegis is in the bag
//
// The maps live in flash, PackBits-compressed per row (map/*.txt plus
// tools/mapconv.py), and cost no RAM. Tiles and sprites come from tools/gentiles.py,
// interface art from tools/genui.py. Never edit the generated headers by hand.
//
// Hardware / driver layer 2023 by Stefan Wagner: https://github.com/wagiminator
// ===================================================================================

#include "driver.h"
#include "worldtiles.h"
#include "worldmap.h"
#include "ui_art.h"

// ===================================================================================
// Constants
// ===================================================================================
#define SCR_W       128
#define SCR_H       64
#define WORLD_PX_W  ((int16_t)(Z->w << 3))
#define WORLD_PX_H  ((int16_t)(Z->h << 3))

#define ANIM_RATE   6
#define HOLD_FRAMES 40                    // ACT held this long opens the character screen

#define FACE_DOWN   0
#define FACE_UP     1
#define FACE_LEFT   2
#define FACE_RIGHT  3

#define FEET_X0     1
#define FEET_X1     6
#define FEET_Y0     4
#define FEET_Y1     7
#define SOLID_FROM  TW_SOLID

#define S_STR   0                         // melee damage
#define S_VIT   1                         // hit points
#define S_INT   2                         // staff damage
#define S_AGI   3                         // melee swing rate and walking speed
#define S_ARMOR 4                         // not a stat: what armour pieces add

#define SLOT_HELM  0
#define SLOT_ARMOR 1
#define SLOT_WEAP  2
#define SLOT_BOOT  3
#define SLOT_NONE  4

#define BAG_COLS 4
#define BAG_ROWS 3
#define BAG_N    (BAG_COLS * BAG_ROWS)

#define CLS_WARRIOR 0
#define CLS_MAGE    1
#define CLS_N       2

#define ATK_BASE  42                      // frames a swing takes at no agility
#define ATK_MIN    6                      // and at a hundred, where the line ends
// Agility buys swings per second along a straight line, which is a division to state
// as a cooldown, and this image has no room for one. So the swing fills the way the
// legs walk: a share of a bar every frame, and a swing when the bar is full. The
// share is what a frame is worth at that agility, so the rate is the line exactly.
// A floor of six frames is what makes the scale cheap: the rise per point comes out at
// three, which is a shift and an add, where a floor of five wanted thirty seven.
#define ATK_STEP  50                           // what a frame earns at no agility
#define ATK_FULL  (ATK_BASE * ATK_STEP)        // 2100, a bar, and it holds in sixteen bits
#define ATK_RISE  ((ATK_FULL / ATK_MIN - ATK_STEP) / 100)   // 3 more for every point
#define SPEED_MIN  1                      // steps per frame at no agility

#define LVL_CAP      100
#define XP_LAST     50                    // the last level that costs more than the one before
#define PTS_PER_LEVEL 1

#define IN_UP    1
#define IN_DOWN  2
#define IN_LEFT  3
#define IN_RIGHT 4
#define IN_ACT   5

// The pad is read through an ADC ladder, so a press has to hold steady for a few
// samples before it counts. Without this a single press occasionally registered twice.
#define DEB_N  3
#define DEB_MS 3

// Attacks. A tap of ACT swings or shoots, holding it opens the character screen.
#define SLASH_FRAMES 6
#define ARROW_SPD    3
#define ARROW_LIFE   26
#define SHOOT_RANGE  56                   // pixels at which a spitter opens fire

// The fields around town. Nothing here starts a fight, but everything fights back.
#define MON_N       9                    // the last slot belongs to the zone's boss
#define BOSS_SLOT   (MON_N - 1)
#define IS_BOSS(t)  ((t) >= M_MALENIA)
#define MON_PERIOD   4                    // main loop ticks between monster actions
#define MON_BITE    15                    // ticks between bites, keeps the old cadence
#define MON_FAST     5                    // pixels a hunter covers per tick
#define MON_SLOW     3                    // and a placid one
#define MON_RESPAWN 45                    // monster ticks before a dead one returns

#define M_RABBIT   0
#define M_PORING   1
#define M_BIRD     2
#define M_SCORPION 3
#define M_SNAKE    4
#define M_WOLF     5
#define M_YETI     6
#define M_MAMMOTH  7
#define M_VALKYR   8
#define M_MALENIA  9
#define M_SANDLORD 10
#define M_FROSTKING 11
#define M_TYPES    12

// ===================================================================================
// Items and classes
// ===================================================================================
typedef struct {
  uint8_t  nameofs;                        // into item_names
  uint8_t  slot, stat, bonus;             // the icon index is the item id itself
} ITEM;

#define IT_NONE     0
#define IT_SWORD    1
#define IT_STAFF    2
#define IT_CAP      3
#define IT_TUNIC    4
#define IT_BOOTS    5
#define IT_POTION   6
#define IT_MASK     7
#define IT_TRAVELS  8
#define IT_ELIXIR   9
#define IT_TARRASQ  10
#define IT_RAPIRA   11
#define IT_BKB      12
#define IT_AEGIS    13
#define IT_MCU      14
#define IT_COIN     15
#define IT_AGANIM   16
#define IT_DESOL    17
#define IT_TREADS   18
#define IT_CIRCLET  19
#define IT_CUIRASS  20
#define IT_SCR_AGI  21
#define IT_SCR_DEF  22
#define IT_SCR_INT  23
#define IT_COUNT    24
// The scrolls sit last so one comparison tells them from anything else you drink
#define IT_SCROLL   IT_SCR_AGI

//                                                     stat    bonus
// Names live in one blob so an item carries a one byte offset, not a four byte
// pointer. Regenerate both together if you ever add an item.
const char item_names[] =
  "-\0"                 // 0
  "SWORD\0"             // 2
  "STAFF\0"             // 8
  "CAP\0"               // 14
  "TUNIC\0"             // 18
  "BOOTS\0"             // 24
  "POTION\0"            // 30
  "MASK\0"              // 37
  "TRAVELS\0"           // 42
  "ELIXIR\0"            // 50
  "TARRASQUE\0"         // 57
  "RAPIRA\0"            // 67
  "BLACK KING BAR\0"    // 74
  "AEGIS\0"             // 89
  "RISC-V MCU\0"        // 95
  "COIN\0"              // 106
  "AGANIM\0"            // 111
  "DESOLATOR\0"         // 118
  "TREADS\0"            // 128
  "CIRCLET\0"           // 135
  "CUIRASS\0"           // 143
  "AGI +10 SCROLL\0"    // 151
  "DEF +30 SCROLL\0"    // 166
  "INT +25 SCROLL\0"    // 181
  ;

const ITEM items[IT_COUNT] = {
  {   0, SLOT_NONE,  0,        0 },   // -
  {   2, SLOT_WEAP,  S_STR,    1 },   // SWORD
  {   8, SLOT_WEAP,  S_INT,    1 },   // STAFF
  {  14, SLOT_HELM,  S_ARMOR,  1 },   // CAP
  {  18, SLOT_ARMOR, S_ARMOR,  2 },   // TUNIC
  {  24, SLOT_BOOT,  S_ARMOR,  1 },   // BOOTS
  {  30, SLOT_NONE,  0,       10 },   // POTION
  {  37, SLOT_HELM,  S_VIT,    7 },   // MASK
  {  42, SLOT_BOOT,  S_AGI,    6 },   // TRAVELS
  {  50, SLOT_NONE,  0,       50 },   // ELIXIR
  {  57, SLOT_ARMOR, S_ARMOR, 20 },   // TARRASQUE
  {  67, SLOT_WEAP,  S_STR,   40 },   // RAPIRA
  {  74, SLOT_WEAP,  S_INT,   30 },   // BLACK KING BAR
  {  89, SLOT_NONE,  0,        0 },   // AEGIS
  {  95, SLOT_NONE,  0,        0 },   // RISC-V MCU
  { 106, SLOT_NONE,  0,        0 },   // COIN
  { 111, SLOT_WEAP,  S_INT,    8 },   // AGANIM
  { 118, SLOT_WEAP,  S_STR,   12 },   // DESOLATOR
  { 128, SLOT_BOOT,  S_ARMOR,  4 },   // TREADS
  { 135, SLOT_HELM,  S_AGI,    4 },   // CIRCLET
  { 143, SLOT_ARMOR, S_VIT,    6 },   // CUIRASS
  { 151, SLOT_NONE,  S_AGI,   10 },   // AGI +10 SCROLL
  { 166, SLOT_NONE,  S_ARMOR, 30 },   // DEF +30 SCROLL
  { 181, SLOT_NONE,  S_INT,   25 },   // INT +25 SCROLL
};

// The counter in town shows its stock in the bag's own grid of twelve cells.
#define SHOP_N 12                       // three rows, the bag's own grid
// What a thing is worth. The counter sells at this and buys back at a quarter of
// it, so nothing can be traded round for a profit. Things it does not stock are
// priced too, because it still buys them.
const uint16_t item_price[IT_COUNT] = {
      0,     8,     8,     8,    12,     8,
      4,   180,   300,    20,   600,  1600,
   1600,     0,     0,     0,   450,   450,
     30,    65,    40,    20,    20,    20,
};
#define BUY(id)  item_price[id]
// The shelf, a column to a kind: the two weapons, the head, the feet, then the
// body and what you drink.
const uint8_t shop_id[SHOP_N] = {
  IT_AGANIM,  IT_CIRCLET, IT_TREADS,  IT_CUIRASS,
  IT_DESOL,   IT_MASK,    IT_TRAVELS, IT_ELIXIR,
  IT_SCR_AGI, IT_SCR_DEF, IT_SCR_INT, IT_NONE };





// Fixed-stride blobs, not pointer tables: four pointers cost as much as the four
// names they point at, and the index is already in hand at every call site.
const char stat_name[] = "STR\0VIT\0INT\0AGI";
// Where a thing is worn, in the order of the SLOT_ constants, so the index is already
// in hand. Four names of four letters sit at a stride of five.
const char slot_name[] = "HEAD\0BODY\0ARM\0\0FEET";   // stride five, ARM is a letter short
#define SLOT_NAME(s)  (slot_name + ((s) * 5))
#define SLOT_NAME_W   (4 * FONT_ADV)
#define STAT_NAME(i)  (stat_name + ((i) << 2))
const char class_name[] = "WARRIOR\0MAGE";
#define CLASS_NAME(c) (class_name + ((c) << 3))
const uint8_t class_stat[CLS_N][4] = {
  { 4, 4, 0, 2 },                         // warrior: strong, tough, slow
  { 0, 2, 5, 3 },                         // mage: fragile, hits from a distance
};
const uint8_t class_weapon[CLS_N] = { IT_SWORD, IT_STAFF };

//                                rabbit poring bird scorp snake wolf yeti mammoth
//                                  ..  ..  ..  ..  ..  ..  ..  ..  valk malenia
const uint16_t mon_hp0[M_TYPES]  = { 10, 18, 20, 72, 44,100,150,174,220,4300,1200,1500 };
const uint8_t mon_str0[M_TYPES]  = {  4,  5,  8,  9, 12, 19, 29, 25, 40, 60, 40, 55 };
const uint8_t mon_def0[M_TYPES]  = {  1,  2,  2,  6,  5,  7,  9, 11, 14, 20,  7, 11 };
const uint16_t mon_xp0[M_TYPES]  = {  4,  6,  9, 25, 18, 45, 70, 65,260,3000,1500,2700 };
// Each creature owns a slice of drop_tab, two bytes an entry: what it leaves and how
// likely out of 255. Giving a creature one more drop costs two bytes and an offset.
const uint8_t drop_ofs[M_TYPES + 1] = { 0, 0, 2, 6, 12, 18, 18, 24, 32, 42, 44, 52, 58 };
const uint8_t drop_tab[] = {
  /* rabbit     nothing at all, on purpose */
  IT_COIN,    255,                                     /* poring */
  IT_POTION,   26, IT_COIN,    255,                    /* bird */
  IT_TRAVELS,   0, IT_POTION,  178, IT_COIN,    255,   /* scorpion */
  IT_POTION,  127, IT_COIN,    255, IT_MASK,      0,   /* snake, the one that hides it */
  /* wolf       nothing at all, on purpose */
  IT_POTION,  178, IT_COIN,    255, IT_SCR_AGI,  25,   /* yeti */
  IT_ELIXIR,   51, IT_POTION,  102, IT_COIN,    255, IT_TREADS,   50,   /* mammoth */
  IT_ELIXIR,  153, IT_COIN,    255, IT_SCR_DEF,  76,
  IT_SCR_AGI,  76, IT_SCR_INT,  76,                    /* valkyrie, the scroll keeper */
  IT_MCU,     255,                                     /* Malenia, no coin */
  IT_TARRASQ, 255, IT_AEGIS,   255, IT_ELIXIR,  153, IT_COIN,    255, /* sand lord */
  IT_AEGIS,   255, IT_ELIXIR,  128, IT_COIN,    255,   /* frost king */
};
// Who spits instead of only biting. A passive one still has to be provoked first.
// Two one-bit facts per type, so one table rather than two: bit 0 shoots, bit 1 hunts.
#define M_SHOOTS 1
#define M_HUNTS  2
const uint8_t mon_flag[M_TYPES] = {  0,  0,  1,  2,  1,  2,  3,  0,  3,  3,  0,  2 };
#define HUNT_RANGE 4                      // tiles at which a hunter notices you

// ===================================================================================
// State
// ===================================================================================
uint8_t  fb[SCR_W * 8] __attribute__((aligned(4)));                   // 128x64 frame, page major
int16_t  cam_x, cam_y;

// Everything a run consists of, in the order the save page stores it, so writing the
// page is one copy rather than a field at a time. Members are named apart from the
// game's own names, which are macros onto them, or the page side of save_read would
// expand too.
typedef struct {
  uint8_t  cls, lvl, points, hp, zone, bosses;
  uint8_t  stat[4];
  uint16_t xp, coin;
  int16_t  px, py;
  uint8_t  worn[4];
  uint8_t  sack[BAG_N];
  uint16_t bosshp;                        // what the zone's boss had left, if anything
} PLAY;
PLAY P;
#define pclass    P.cls
#define plvl      P.lvl
#define ppoints   P.points
#define php       P.hp
#define cur_zone  P.zone
#define boss_dead P.bosses
#define st        P.stat
#define pxp       P.xp
#define gold      P.coin
#define hx        P.px
#define hy        P.py
#define equip     P.worn
#define bag       P.sack
uint8_t  facing, frame, anim;
uint8_t  atk_timer;                       // frames the swing arc stays up
uint16_t atk_acc;                         // how full the swing is; a full bar is a swing
// Slot 0 carries the hero's fireball, slot 1 whatever a monster last spat. One each
// at a time, which is what keeps this to a pair of shorts instead of a per creature
// projectile.
#define SHOT_N   6                       // one flight of bolts, shared by everyone
uint8_t  shot_life[SHOT_N], shot_dir[SHOT_N], shot_pow[SHOT_N];
int16_t  shot_x[SHOT_N], shot_y[SHOT_N];

// Launch a bolt from wherever, however hard. The hero may take any slot, a monster
// only the ones above zero, so his shot is never crowded out. Returns 0 when the air
// is full and nothing left the barrel.
static uint8_t shot_fire(uint8_t from, int16_t x, int16_t y, uint8_t dir, uint8_t pow) {
  uint8_t s;
  for(s = from; s < SHOT_N; s++) {
    if(shot_life[s]) continue;
    shot_x[s] = x; shot_y[s] = y;
    shot_dir[s] = dir;
    shot_life[s] = ARROW_LIFE;
    shot_pow[s] = pow;                    // zero marks it as the hero's
    return 1;
  }
  return 0;
}

typedef struct {
  int16_t x, y;
  uint8_t type;
  uint16_t hp;                            // a boss can outgrow a byte
  uint8_t aggro, cd, frame;
  int8_t  slide;                          // the held sidestep, in pixels, on the spare byte
} MONST;
MONST    mons[MON_N];
uint8_t  mon_tick;
// A scroll lasts thirty real seconds, timed against the free-running system
// counter rather than against frames, whose length varies with what is on screen.
// buff[] holds what the scroll grants, which is what stat_of adds, and zeroing it is
// what expiry means; the deadline alone would come round again when the counter wraps.
#ifdef SIM_HOST
uint32_t sim_clock;
#define NOW()          (sim_clock)
#define SCROLL_TICKS   (30UL * 12000000UL / 1000UL * 1000UL)
#define MENU_GAP       (12000000UL / 5)          // longer than any frame: a menu ran
#else
#define NOW()          (STK->CNT)
#define SCROLL_TICKS   (30UL * 1000UL * DLY_MS_TIME)
#define MENU_GAP       (200UL * DLY_MS_TIME)     // longer than any frame: a menu ran
#endif
uint8_t  buff[5];
uint32_t buff_left[5];
uint32_t clk;             // the clock at the previous pass round the loop
// Healing runs off the redraw, whose pace the display bus fixes at about 37 a second.
// Adding maxhp each time and spending a full REGEN_FULL for one point means a whole
// bar always takes REGEN_FULL frames, whatever the bar is worth. No division needed.
#define REGEN_FULL 4400                   // two minutes from empty to full
uint16_t regen;
uint8_t  snow_t;
uint8_t  walk_acc;                        // quarter pixels the legs have not spent yet
uint8_t  door_lock;                       // set while standing in a doorway
uint16_t entropy;                         // stirred while a menu waits for input

const ZONE* Z;                            // the zone we are standing in
// Rows are stored back to back with only their lengths, so the map carries one byte
// of index per row instead of two. Walking that into pointers once on entry keeps
// every later lookup as cheap as before, and the extra entry gives each row its end.
// A length of zero means the row is the one above over again, so its bytes were never
// stored. Hand-drawn maps repeat rows constantly - a corridor, a wall, a shore - and
// each repeat used to cost its full compressed size.
const uint8_t* row_ptr[ZONE_MASKH + 1];
uint8_t        row_len[ZONE_MASKH + 1];

static void zone_set(uint8_t id) {
  const uint8_t* p;
  uint8_t i, n = 0;
  cur_zone = id;
  Z = &zones[id];
  p = map_rle + Z->rleofs;
  for(i = 0; i < Z->h; i++) {
    uint8_t k = map_len[Z->rowofs + i];
    if(k) { n = k; p += k; }
    row_ptr[i] = p - n;
    row_len[i] = n;
  }
}

#define COIN_MAX 0xFFFF


// ===================================================================================
// Frame buffer primitives
// ===================================================================================
static void fb_clear(void) {
  uint16_t i;
  uint32_t* w = (uint32_t*)(void*)fb;     // four bytes a go, the buffer is aligned
  for(i = 0; i < (SCR_W * 8) / 4; i++) w[i] = 0;
}

static void fb_flush(void) {
  uint8_t page, i;
  for(page = 0; page < 8; page++) {
    const uint8_t* p = fb + ((uint16_t)page << 7);
    JOY_OLED_data_start(page);
    for(i = 0; i < SCR_W; i++) JOY_OLED_send(p[i]);
    JOY_OLED_end();
  }
}

static void fb_px(uint8_t x, uint8_t y) {
  if(x < SCR_W && y < SCR_H) fb[((uint16_t)(y >> 3) << 7) + x] |= (uint8_t)(1 << (y & 7));
}

static void fb_hline(uint8_t x, uint8_t y, uint8_t w) {
  while(w--) fb_px(x++, y);
}

static void fb_vline(uint8_t x, uint8_t y, uint8_t h) {
  while(h--) fb_px(x, y++);
}

static void fb_box(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
  fb_hline(x, y, w);
  fb_hline(x, (uint8_t)(y + h - 1), w);
  fb_vline(x, y, h);
  fb_vline((uint8_t)(x + w - 1), y, h);
}

static void fb_invert(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
  uint8_t i, j;
  for(j = 0; j < h; j++)
    for(i = 0; i < w; i++) {
      uint8_t px = (uint8_t)(x + i), py = (uint8_t)(y + j);
      if(px < SCR_W && py < SCR_H)
        fb[((uint16_t)(py >> 3) << 7) + px] ^= (uint8_t)(1 << (py & 7));
    }
}

// Blit page-major art at any y, shifting across the page boundary as needed
static void fb_blit(const uint8_t* src, uint8_t sw, uint8_t spages, uint8_t dx, uint8_t dy) {
  uint8_t p, i;
  uint8_t sh = (uint8_t)(dy & 7), pg = (uint8_t)(dy >> 3);
  for(p = 0; p < spages; p++) {
    for(i = 0; i < sw; i++) {
      uint8_t v = src[i];
      uint8_t x = (uint8_t)(dx + i);
      if(x >= SCR_W) continue;
      if(pg + p < 8) fb[((uint16_t)(pg + p) << 7) + x] |= (uint8_t)(v << sh);
      if(sh && pg + p + 1 < 8)
        fb[((uint16_t)(pg + p + 1) << 7) + x] |= (uint8_t)(v >> (8 - sh));
    }
    src += sw;
  }
}

// The banner is stored compressed and lands straight in the frame: it is full width
// and starts at the top, so its byte order already matches pages 0..4 of the buffer.
// Icons arrive twelve bits per column, so spread them back into two page rows.
static void fb_icon(uint8_t id, uint8_t x, uint8_t y) {
  const uint8_t* s = ui_icons + (uint16_t)(id - 1) * ICON_BYTES;   // no icon for nothing
  uint8_t buf[ICON_W * 2], c;
  for(c = 0; c < ICON_W; c++) {
    uint8_t b = (uint8_t)(c * 12);
    uint16_t v = (uint16_t)((uint16_t)s[b >> 3] | ((uint16_t)s[(b >> 3) + 1] << 8));
    v = (uint16_t)(v >> (b & 7));
    buf[c] = (uint8_t)v;
    buf[ICON_W + c] = (uint8_t)((v >> 8) & 0x0F);
  }
  fb_blit(buf, ICON_W, 2, x, y);
}

// ===================================================================================
// Text
// ===================================================================================
static uint8_t glyph_of(char c) {
  if(c >= 'A' && c <= 'Z') return (uint8_t)(1 + (c - 'A'));
  if(c >= '0' && c <= '9') return (uint8_t)(27 + (c - '0'));
  switch(c) {
    case '.': return 37;
    case '+': return 38;
    case '-': return 39;
    case '/': return 40;
    default:  return 0;                   // space
  }
}

// Hands back the x just past the last letter, so callers that want to write on after
// a word do not have to measure it themselves.
static uint8_t fb_text(uint8_t x, uint8_t y, const char* s) {
  while(*s) {
    fb_blit(ui_font + ((uint16_t)glyph_of(*s++) << 2), 4, 1, x, y);
    x = (uint8_t)(x + FONT_ADV);
  }
  return x;
}

// The title is the ordinary font with every pixel grown into a three by three
// block. Cheaper than a picture of the same words by two hundred bytes.
static void fb_text3(uint8_t x, uint8_t y, const char* t) {
  while(*t) {
    const uint8_t* g = ui_font + ((uint16_t)glyph_of(*t++) << 2);
    uint8_t c, r, i, j;
    for(c = 0; c < 4; c++)
      for(r = 0; r < 8; r++)
        if((g[c] >> r) & 1)
          for(i = 0; i < 3; i++)
            for(j = 0; j < 3; j++)
              fb_px((uint8_t)(x + c * 3 + i), (uint8_t)(y + r * 3 + j));
    x = (uint8_t)(x + 15);
  }
}


// Divide by ten with shifts only; RV32EC has no divider

// Returns the x just past the number so callers can lay text out left to right.
// With NUM_RIGHT set, x is instead the right edge: the width is already known here,
// so aligning inside costs far less than dividing a second time at the call site.
#define NUM_RIGHT 0x80
static uint8_t fb_num(uint8_t x, uint8_t y, uint16_t v) {
  char buf[6];
  uint8_t i = 5;
  buf[5] = 0;
  // Numbers are only ever drawn in menus, so the digits come off by subtracting ten
  // rather than by a reciprocal: thousands of loop passes cost nothing here, and the
  // shift-and-add division cost real flash.
  do {
    uint16_t q = 0;
    while(v >= 10) { v = (uint16_t)(v - 10); q++; }
    buf[--i] = (char)('0' + (uint8_t)v);
    v = q;
  } while(v && i);
  {
    uint8_t w = (uint8_t)((5 - i) * FONT_ADV);
    if(x & NUM_RIGHT) {                   // x was the right edge: report the left one
      x = (uint8_t)((x & 0x7F) - w);
      fb_text(x, y, buf + i);
      return x;
    }
    fb_text(x, y, buf + i);
    return (uint8_t)(x + w);
  }
}

// Five places blank the screen for one centred line, so they share this.
static void msg_screen(uint8_t x, const char* t) {
  fb_clear();
  fb_text(x, 28, t);
  fb_flush();
}


// ===================================================================================
// Character helpers
// ===================================================================================
// What the four worn things add to one stat. Armour is a stat like any other here,
// which is all defence ever was: the same walk over the same four slots.
static uint8_t equip_bonus(uint8_t stat) {
  uint8_t i, sum = 0;
  for(i = 0; i < 4; i++) {
    const ITEM* it = &items[equip[i]];
    if(equip[i] && it->bonus && it->stat == stat)
      sum = (uint8_t)(sum + it->bonus);
  }
  return sum;
}
#define defence() (uint8_t)(equip_bonus(S_ARMOR) + buff[S_ARMOR])

// Armour is the only source of defence: no stat feeds it, so plate matters and no
// single stat becomes the obvious dump for everyone.
// What a stat is really worth once gear is counted. Every formula goes through this,
// otherwise the character screen shows bonuses that do nothing.
__attribute__((noinline)) static uint8_t stat_of(uint8_t s) {
  return (uint8_t)(st[s] + equip_bonus(s) + buff[s]);
}


// Which stat the equipped weapon strikes with, and how hard
static uint8_t attack_power(void) {
  uint8_t w = equip[SLOT_WEAP];
  if(!w) return 0;
  return stat_of(items[w].stat);         // equip_bonus already counts the weapon
}

// Gear can add vitality now, and stats have no ceiling, so this has to be clamped:
// at VIT 62 the old byte arithmetic wrapped and left the hero on almost no health.
static uint8_t maxhp(void) {
  uint16_t h = (uint16_t)(8 + ((uint16_t)stat_of(S_VIT) << 2));
  return (uint8_t)(h > 250 ? 250 : h);
}

static void zone_enter(uint8_t id, uint8_t from);   // defined with the other zone work
static void boss_place(uint8_t id);
static void mons_clear(void);

static void new_game(uint8_t cls) {
  uint8_t i;
  pclass  = cls;
  plvl    = 1;                            // a run starts here, and xp_need() counts
  ppoints = 3;                            // down from plvl, so this must never be zero
  pxp     = 0;
  for(i = 0; i < 4; i++) st[i] = class_stat[cls][i];
  php = maxhp();
  equip[SLOT_HELM]  = IT_CAP;
  equip[SLOT_ARMOR] = IT_TUNIC;
  equip[SLOT_WEAP]  = class_weapon[cls];
  equip[SLOT_BOOT]  = IT_BOOTS;
  for(i = 0; i < BAG_N; i++) bag[i] = IT_NONE;
  bag[0] = IT_POTION;
  bag[1] = IT_POTION;
  boss_dead = 0; gold = 0;
  buff[S_AGI] = 0; buff[S_ARMOR] = 0;     // no scroll survives its owner
//  plvl = 100; ppoints = 102; gold = 2000;  /* TEMP: playtest kit, overrides the above */
//  for(i = 0; i < 4; i++) equip[i] = IT_NONE;        /* TEMP: naked, buy the lot at the */
//  bag[0] = IT_NONE; bag[1] = IT_NONE;               /* TEMP: counter, potions and all */
//  { uint8_t k; for(k = 0; k < 4; k++) st[k] = 20; }  /* TEMP: or dial the body here */
//  bag[2] = IT_AEGIS; bag[3] = IT_AEGIS;                                     /* TEMP */
//  bag[4] = IT_BKB; bag[5] = IT_RAPIRA;                     /* TEMP: both, pick one */
  zone_enter(ZONE_START, 4);              // placed at the spawn, the way a stair does it
  facing = FACE_DOWN;
  frame = 0; anim = 0;
}

// ===================================================================================
// Save slot: the last 64 byte page of flash, written only from SAVE AND EXIT
// ===================================================================================
// The linker maps flash at 0 because that is the boot alias the core fetches from,
// but the flash controller addresses the real bank at 0x08000000. Programming through
// the alias silently does nothing, which is why the first version never saved.
#ifndef SAVE_ADDR                         // the host test harness points both at RAM
#define SAVE_PROG  0x08003FC0             // what FLASH->ADDR and the buffer want
#define SAVE_READ  0x00003FC0             // where the CPU reads it back
#else
#define SAVE_PROG  SAVE_ADDR
#define SAVE_READ  SAVE_ADDR
#endif
#define SAVE_MAGIC 0xA8           // bumped when the boss's health joined it

typedef struct {
  uint8_t magic;
  PLAY    p;
} SAVE;

// The struct only needs two byte alignment, so taking a uint32_t* to a plain SAVE on
// the stack can be misaligned - and RV32EC has no misaligned word access, so the words
// come out shifted. Going through this union forces four byte alignment and covers the
// whole 64 byte page, so nothing is read past the end either.
typedef union {
  SAVE     s;
  uint32_t w[16];
} SAVEPAGE;

static uint8_t save_exists(void) {
  return (uint8_t)(((const SAVE*)SAVE_READ)->magic == SAVE_MAGIC);
}

// Kept out of line: measured smaller as a call than inlined.
__attribute__((noinline)) static void flash_unlock(void) {
  FLASH->KEYR = 0x45670123; FLASH->KEYR = 0xCDEF89AB;
  FLASH->MODEKEYR = 0x45670123; FLASH->MODEKEYR = 0xCDEF89AB;
}

// Both the erase and the write end the same way: point at the page, pull the trigger,
// wait for the controller, drop the mode and lock up again.
static void flash_go(uint32_t mode) {
  FLASH->ADDR  = SAVE_PROG;
  FLASH->CTLR |= FLASH_CTLR_STRT;
  while(FLASH->STATR & FLASH_STATR_BSY);
  FLASH->CTLR &= (uint32_t)~mode;
  FLASH->CTLR |= FLASH_CTLR_LOCK;
}

static void save_erase(void) {
#ifdef SAVE_ADDR              // the host build has no flash controller, so mimic what it does
  { uint8_t i; for(i = 0; i < 64; i++) ((uint8_t*)SAVE_PROG)[i] = 0xFF; return; }
#endif
  flash_unlock();
  FLASH->CTLR |= FLASH_CTLR_PAGE_ER;
  flash_go(FLASH_CTLR_PAGE_ER);
}

static uint8_t save_write(void) {
  SAVEPAGE u;
  uint8_t i;
  for(i = 0; i < 16; i++) u.w[i] = 0xFFFFFFFF;
  u.s.magic = SAVE_MAGIC;
  P.bosshp  = mons[BOSS_SLOT].hp;         // a fight in progress is part of the run
  {                                       // the whole run in one copy, spelled out
    uint8_t* d = (uint8_t*)&u.s.p;        // because there is no library to call
    const uint8_t* a = (const uint8_t*)&P;
    for(i = 0; i < sizeof(PLAY); i++) d[i] = a[i];
  }

  save_erase();
  flash_unlock();
  FLASH->CTLR |= FLASH_CTLR_PAGE_PG;      // fill the write buffer, then commit it
  FLASH->CTLR |= FLASH_CTLR_BUF_RST;
  while(FLASH->STATR & FLASH_STATR_BSY);
  for(i = 0; i < 16; i++) {
    *(volatile uint32_t*)(SAVE_PROG + ((uint32_t)i << 2)) = u.w[i];
    FLASH->CTLR |= FLASH_CTLR_BUF_LOAD;
    while(FLASH->STATR & FLASH_STATR_BSY);
  }
  flash_go(FLASH_CTLR_PAGE_PG);

  {                                       // read the whole page back before believing it
    const uint8_t* a = (const uint8_t*)&u.s;
    const uint8_t* b = (const uint8_t*)SAVE_READ;
    for(i = 0; i < sizeof(SAVE); i++) if(a[i] != b[i]) return 0;
    return 1;
  }
}

// Every field off the page is bounded before it is believed, and the bounding comes in
// only two shapes: below a limit, or between one and a maximum. Naming them keeps a
// corrupt page as harmless as it was, for a fraction of the code it used to take.
static uint8_t sv_lim(uint8_t v, uint8_t lim, uint8_t def) { return v < lim ? v : def; }
static uint8_t sv_pos(uint8_t v, uint8_t hi, uint8_t def) {
  return (v && v <= hi) ? v : def;
}

static void save_read(void) {
  const PLAY* s = &((const SAVE*)SAVE_READ)->p;
  uint8_t i, m;
  pclass  = sv_lim(s->cls, CLS_N, CLS_WARRIOR);
  plvl    = sv_pos(s->lvl, LVL_CAP, 1);
  ppoints = sv_lim(s->points, 110, 0);   // a hero at the cap has earned 102
  for(i = 0; i < 4; i++) {
    st[i]    = sv_pos(s->stat[i], 99, 1);
    equip[i] = sv_lim(s->worn[i], IT_COUNT, IT_NONE);
    // A page that says a breastplate is a sword would send attack_power looking up a
    // stat that does not exist, so a worn thing has to belong in the slot it sits in.
    if(items[equip[i]].slot != i) equip[i] = IT_NONE;
  }
  for(i = 0; i < BAG_N; i++) bag[i] = sv_lim(s->sack[i], IT_COUNT, IT_NONE);
  m = maxhp();
  php = sv_pos(s->hp, m, m);
  pxp   = s->xp;
  gold  = s->coin;
  boss_dead = s->bosses;
  zone_set((uint8_t)(s->zone < ZONE_N ? s->zone : ZONE_START));
  hx = (s->px >= 0 && s->px < WORLD_PX_W - 8) ? s->px
                                              : (int16_t)((uint16_t)Z->spawnx << 3);
  hy = (s->py >= 0 && s->py < WORLD_PX_H - 8) ? s->py
                                              : (int16_t)((uint16_t)Z->spawny << 3);
  facing = FACE_DOWN; frame = 0; anim = 0;
  mons_clear();
  boss_place(cur_zone);                   // the arena you saved in still has its boss
  // and he is as hurt as you left him. Anything a page claims that is not less than
  // what he starts with is ignored, which also covers his being absent or already down.
  // A wound is also proof the fight had started, so he wakes up still angry: boss_place
  // leaves him idle, and the sand lord neither hunts nor spits, so nothing would ever
  // have woken him again.
  if(s->bosshp && s->bosshp < mons[BOSS_SLOT].hp) {
    mons[BOSS_SLOT].hp = s->bosshp;
    mons[BOSS_SLOT].aggro = 1;
  }
}

// ===================================================================================
// Levelling. The save is no longer written here: SAVE AND EXIT is the only writer.
// ===================================================================================
// What the next level costs. The thresholds were a geometric run all along, so they
// are worked out rather than stored: a hundred bytes of table for a line of arithmetic,
// and the curve lands within a couple of percent of the one it replaces.
static uint16_t xp_need(uint8_t lv) {
  uint16_t n = 33;
  // The run climbs 13% a level, and one clamp on the level stops the climb without a
  // test on every pass: everything from XP_LAST up costs what XP_LAST costs. The clamp
  // has to sit at 62 or below whatever else is wanted of it, because the raw run passes
  // sixty five thousand at 63, wraps, and would make a level cheaper than the one before.
  // At 50 it holds the second half of the run to a flat 12388.
  if(lv > XP_LAST) lv = XP_LAST;
  while(--lv) n = (uint16_t)(n + (n >> 3) + (n >> 7));
  return n;
}

// Phrases are pitch and hold in pairs, ending on a silent one. Written out as calls
// they cost ten bytes a note; here a note is two.
const uint8_t tune_level[]   = {  65,145,  96,173, 128,217, 160,255, 0,0 };
const uint8_t tune_fanfare[] = {  40,150,  65,150,  96,160, 128,190,
                                 160,255, 160,255, 128,120, 160,255, 0,0 };
const uint8_t tune_death[]   = {  96,190,  65,160,  40,160,  20,170,
                                   0,255,   0,255, 0,0 };

static void play(const uint8_t* p) {
  while(p[1]) { JOY_sound(p[0], p[1]); p += 2; }
}

// The two endings of a run: a line, a phrase and long enough to read it.
static void announce(uint8_t x, const char* t, const uint8_t* tune) {
  msg_screen(x, t);
  play(tune);
  JOY_DLY_ms(2500);
}

static void level_up(void) {
  plvl++;
  ppoints = (uint8_t)(ppoints + PTS_PER_LEVEL);
  php = maxhp();                          // a level always puts you back on your feet
  play(tune_level);                       // rising, because a level is good news
}

// Called by combat when a monster dies
void gain_xp(uint16_t v) {
  pxp += v;
  while(plvl < LVL_CAP) {
    uint16_t need = xp_need(plvl);
    if(pxp < need) break;
    pxp = (uint16_t)(pxp - need);
    level_up();
  }
  if(plvl >= LVL_CAP) pxp = 0;
}

// ===================================================================================
// Input
// ===================================================================================
#define NEAR(v,c) (((v) > (c) - JOY_DEV) && ((v) < (c) + JOY_DEV))

// Eight rungs of the resistor ladder, each with the step it means. A table beats
// eight branches that all ask the same question of a different constant.
// The first four rungs are listed in FACE order on purpose: a facing and a straight
// push are the same four vectors, so which way the hero looks reads out of this table
// too and does not need a pair of functions of its own.
static const uint16_t joy_rung[8] = { JOY_S, JOY_N, JOY_W, JOY_E,
                                      JOY_NE, JOY_SE, JOY_SW, JOY_NW };
static const int8_t joy_step[8][2] = { { 0, 1}, { 0,-1}, {-1, 0}, { 1, 0},
                                       { 1,-1}, { 1, 1}, {-1, 1}, {-1,-1} };
#define face_dx(f) (joy_step[f][0])
#define face_dy(f) (joy_step[f][1])

static uint8_t joy_delta(uint16_t v, int8_t* dx, int8_t* dy) {
  uint8_t i;
  *dx = 0; *dy = 0;
  if(v < 10) return 0;
  for(i = 0; i < 8; i++)
    if(NEAR(v, joy_rung[i])) {
      *dx = joy_step[i][0];
      *dy = joy_step[i][1];
      return 1;
    }
  return 0;
}

static uint8_t input_now(void) {
  int8_t dx, dy;
  if(JOY_act_pressed()) return IN_ACT;
  if(joy_delta(ADC_read(), &dx, &dy)) {
    if(dy < 0) return IN_UP;
    if(dy > 0) return IN_DOWN;
    if(dx < 0) return IN_LEFT;
    return IN_RIGHT;
  }
  return 0;
}

// Wait for a settled release, then for a press that reads the same DEB_N times.
// Both halves have to be stable or the ladder's transients count as extra presses.
static uint8_t ui_input(void) {
  uint8_t n = 0, cand = 0;
  while(n < DEB_N) {
    if(input_now()) n = 0; else n++;
    entropy++;
    JOY_DLY_ms(DEB_MS);
  }
  n = 0;
  while(1) {
    uint8_t k = input_now();
    entropy++;
    if(k && k == cand) { if(++n >= DEB_N) return k; }
    else { cand = k; n = 0; }
    JOY_DLY_ms(DEB_MS);
  }
}

// ===================================================================================
// World rendering
// ===================================================================================
// The map is PackBits compressed one row at a time. Rendering walks a row left to
// right, so it decodes the whole row once; collision wants single tiles, so it walks
// the row only as far as the column it needs and stops.
uint8_t rowbuf[2][ZONE_MAXW];

// The inner decoder lives in src/rle.S on the device and here on the host, so the
// simulator can still check it. Both take the same pointers and must agree byte for
// byte; nothing about the zone layout leaks into the assembly.
#ifdef SIM_HOST
static void map_row_rle(const uint8_t* p, const uint8_t* end, uint8_t* out, uint8_t w) {
  uint8_t* o = out;
  uint8_t* oe = out + w;
  while(o < oe && p < end) {
    uint8_t t = *p++;
    if(t & 0x80) {
      uint8_t n = (uint8_t)(t & 0x7F), v = *p++;
      while(n-- && o < oe) *o++ = v;
    }
    else {
      while(t-- && o < oe) *o++ = *p++;
    }
  }
  while(o < oe) *o++ = 0;
}
#else
void map_row_rle(const uint8_t* p, const uint8_t* end, uint8_t* out, uint8_t w);
#endif

static void map_row(uint8_t ty, uint8_t* out) {
  uint8_t x = 0;
  if(ty >= Z->h) { while(x < Z->w) out[x++] = 0; return; }
  map_row_rle(row_ptr[ty], row_ptr[ty] + row_len[ty], out, Z->w);
}

static uint8_t mget(uint8_t tx, uint8_t ty) {
  const uint8_t* p;
  uint8_t x = 0;
  if(tx >= Z->w || ty >= Z->h) return 0;
  p = row_ptr[ty];
  while(1) {
    uint8_t t = *p++;
    if(t & 0x80) {
      uint8_t n = (uint8_t)(t & 0x7F);
      if(tx < (uint8_t)(x + n)) return *p;
      x = (uint8_t)(x + n);
      p++;
    }
    else {
      if(tx < (uint8_t)(x + t)) return p[tx - x];
      x = (uint8_t)(x + t);
      p += t;
    }
  }
}

// Plain ground is one character in world.txt; spread it over three interchangeable
// pictures so a field is not visibly tiled. xor and shift alone are linear over GF(2)
// and give a periodic pattern, which is what the additions are there to break up.
static uint8_t gvar(uint8_t x, uint8_t y) {
  static const uint8_t pick[4] = { 0, 1, 2, 1 };
  uint8_t h = (uint8_t)(x + (uint8_t)(y << 3));
  h ^= (uint8_t)(h >> 3);
  h  = (uint8_t)(h + (uint8_t)(h << 2));
  h ^= (uint8_t)(h >> 5);
  h  = (uint8_t)(h + 0x37);
  h ^= (uint8_t)(h << 1);
  return pick[h & 3];
}

// A zero in the map means "this biome's ground": the zone says which trio of ground
// pictures to use and the position picks one of the three.
static uint8_t tpic(uint8_t v, uint8_t tx, uint8_t ty) {
  return v ? v : (uint8_t)(Z->ground + gvar(tx, ty));
}


// Composite one 8x8 sprite into a page row at an arbitrary vertical offset
static void put_spr_w(uint8_t* row, const uint8_t* g, int16_t sx, int16_t dy, uint8_t w) {
  uint8_t c;
  if(dy <= -8 || dy >= 8) return;
  for(c = 0; c < w; c++) {
    int16_t ox = sx + c;
    if(ox < 0 || ox >= SCR_W) continue;
    if(dy >= 0) {
      row[ox] &= (uint8_t)~(uint8_t)(0xFF << dy);
      row[ox] |= (uint8_t)(g[c] << dy);
    }
    else {
      row[ox] &= (uint8_t)~(uint8_t)(0xFF >> (-dy));
      row[ox] |= (uint8_t)(g[c] >> (-dy));
    }
  }
}

static void put_spr(uint8_t* row, const uint8_t* g, int16_t sx, int16_t dy) {
  put_spr_w(row, g, sx, dy, 8);
}

// Scale val/max onto w pixels without dividing: Bresenham over w steps
static uint8_t bar_fill(uint16_t val, uint16_t max, uint8_t w) {
  uint32_t acc = 0;                       // max + val, and a boss has fifty thousand
  uint8_t i, n = 0;
  if(!max) return 0;
  if(val >= max) return w;
  for(i = 0; i < w; i++) {
    acc += val;
    if(acc >= max) { acc -= max; n++; }
  }
  return n;
}

// Health and experience, tucked into the top left corner so they clear the doorways
#define HUD_LX 1
#define HUD_BX 7
#define HUD_BW 38
#define HUD_IW (HUD_BW - 2)
#define HUD_BH 5                          // two pixels thinner than it used to be

// A bar is five rows tall and sits inside one page whichever of the two it is, so the
// three rows of fill share a mask and go in one write a column.
// Kept out of line: measured smaller as a call than inlined.
__attribute__((noinline)) static void draw_bar(uint8_t y, uint8_t fill) {
  uint8_t i;
  uint8_t m = (uint8_t)(0x07u << ((y & 7) + 1));
  uint8_t* p = fb + ((uint16_t)(y >> 3) << 7) + HUD_BX + 1;
  fb_box(HUD_BX, y, HUD_BW, HUD_BH);
  for(i = 0; i < fill; i++) p[i] |= m;
}

#define HUD_MX (HUD_BX + HUD_BW + 2)      // where a scroll's mark stands
// The font is four by seven, which is too tall to frame twice inside the plate, so the
// marks carry their own three by five letters - frame and all, one byte a column, and
// no box to draw around them.
const uint8_t mark_stat[3] = { S_AGI, S_ARMOR, S_INT };
const uint8_t mark_glyph[21] = {
  0x7F, 0x41, 0x7D, 0x4B, 0x7D, 0x41, 0x7F,   // A
  0x7F, 0x41, 0x7F, 0x63, 0x5D, 0x41, 0x7F,   // D
  0x7F, 0x41, 0x63, 0x7F, 0x63, 0x41, 0x7F,   // I
};

// Kept out of line: measured smaller as a call than inlined.
__attribute__((noinline)) static void draw_hud(void) {
  uint8_t x;
  // Dark plate so the bars stay readable. Fifteen rows is one whole page and seven bits
  // of the next, so it goes down by the column rather than by the pixel. It reaches out
  // to the marks only on the rows they are on, or their column would stand on a black
  // strip all the way to the bars.
  uint8_t wide = (uint8_t)((buff[S_AGI] || buff[S_ARMOR] || buff[S_INT]) ? HUD_MX + 23 : HUD_BX + HUD_BW);
  for(x = 0; x <= HUD_BX + HUD_BW; x++) { fb[x] = 0; fb[SCR_W + x] &= 0x80; }
  for(; x <= wide; x++) fb[x] &= 0x80;    // beside the marks, row seven stays as it was
  fb_text(HUD_LX, 0, "H");
  draw_bar(1, bar_fill(php, maxhp(), HUD_IW));
  // With a boss in sight the lower bar shows his life instead of your progress: in
  // that fight the number that matters is his, and the plate is already drawn. Out of
  // sight it goes back, or the whole valhalla grind would run with no bar at all.
  { const MONST* b = &mons[BOSS_SLOT];
    uint8_t near = (uint8_t)(b->hp && abs((int16_t)(b->x - hx)) < 96);
    fb_text(HUD_LX, 8, near ? "B" : "E");
    draw_bar(9, near ? bar_fill(b->hp, mon_hp0[b->type], HUD_IW)
                     : bar_fill(pxp, xp_need(plvl), HUD_IW)); }
  // A boxed letter beside each bar for as long as its scroll lasts
  // Side by side against the top edge, each keeping its place whether the other is lit
  { uint8_t k;                            // each keeps its own place, lit or not
    for(k = 0; k < 3; k++)
      if(buff[mark_stat[k]])
        fb_blit(mark_glyph + k * 7, 7, 1, (uint8_t)(HUD_MX + k * 8), 0); }
}

#define SNOW_GROUND TW_SNOW_A             // the ground the snow field and the cave share
// Weather belongs to a zone, not to what it is floored with: the ice cave is laid on the
// same snow tiles, so snow used to fall indoors. The cave is the one snow-floored zone
// with a boss in it, which is the cheapest thing that tells the two apart - the zone
// number costs twelve bytes more, because Z is already in hand here and cur_zone is not.
// Only the drawing asks. The drift below keeps running under the roof, for nothing and
// for free: the cave already redraws on the monster tick, on the same period.
#define SNOW_COUNT  14                    // flakes in the air, not a tile index

// Flakes are drawn straight onto the finished frame: no tiles, no extra layer
static void draw_snow(void) {
  uint8_t i;
  for(i = 0; i < SNOW_COUNT; i++) {
    uint8_t x = (uint8_t)((i * 37 + (snow_t >> 2)) & 127);
    uint8_t y = (uint8_t)((i * 23 + snow_t + (i << 3)) & 63);
    fb_px(x, y);
  }
}

// The same lookup four times over: which picture sits at this tile.
static const uint8_t* tile_pic(uint8_t r, uint8_t tx, uint8_t ty) {
  return world_tiles + ((uint16_t)tpic(rowbuf[r][tx], tx, ty) << 3);
}

// Four things are drawn from world coordinates; only this knows about the camera.
static void put_world(uint8_t* row, const uint8_t* g, int16_t wx, int16_t wy, int16_t base) {
  put_spr(row, g, (int16_t)(wx - cam_x), (int16_t)(wy - cam_y - base));
}

static void draw_world(void) {
  uint8_t page;
  uint8_t tx0 = (uint8_t)(cam_x >> 3), sx = (uint8_t)(cam_x & 7);
  uint8_t ty0 = (uint8_t)(cam_y >> 3), sy = (uint8_t)(cam_y & 7);

  for(page = 0; page < 8; page++) {
    uint8_t ty = (uint8_t)(ty0 + page);
    uint8_t tx = tx0, col = sx, i = 0;
    uint8_t* row = fb + ((uint16_t)page << 7);
    const uint8_t* ga;
    const uint8_t* gb;
    uint8_t twoRows = (uint8_t)(sy && (uint8_t)(ty + 1) < Z->h);
    map_row(ty, rowbuf[0]);
    if(twoRows) map_row((uint8_t)(ty + 1), rowbuf[1]);
    ga = tile_pic(0, tx, ty);
    gb = twoRows ? tile_pic(1, tx, (uint8_t)(ty + 1)) : 0;
    while(i < SCR_W) {
      uint8_t v = (uint8_t)(ga[col] >> sy);
      if(gb) v |= (uint8_t)(gb[col] << (8 - sy));
      row[i++] = v;
      if(++col == 8) {
        col = 0;
        if(++tx < Z->w) {
          ga = tile_pic(0, tx, ty);
          if(gb) gb = tile_pic(1, tx, (uint8_t)(ty + 1));
        }
      }
    }
    // Hero and any attack effect, each clearing the box under it so it stays
    // readable against bright ground
    {
      int16_t base = (int16_t)((uint16_t)page << 3);
      uint8_t k;
      for(k = 0; k < MON_N; k++) {
        int16_t mx, my;
        if(!mons[k].hp) continue;
        mx = (int16_t)(mons[k].x - cam_x); my = (int16_t)(mons[k].y - cam_y - base);
        if(IS_BOSS(mons[k].type)) {       // twice the size, so two stacked halves
          const uint8_t* g = boss_spr
              + ((uint16_t)(mons[k].type - M_MALENIA) * BOSS_W * BOSS_PAGES);
          put_spr_w(row, g, mx, my, BOSS_W);
          put_spr_w(row, g + BOSS_W, mx, (int16_t)(my + 8), BOSS_W);
        }
        else put_spr(row, mon_spr + ((uint16_t)((mons[k].type << 1) + mons[k].frame) << 3),
                     mx, my);
      }
      put_world(row, hero_spr + ((uint16_t)((facing << 1) + frame) << 3), hx, hy, base);
      if(atk_timer)
        put_world(row, fx_spr + ((uint16_t)(FX_SLASH + facing) << 3),
                  (int16_t)(hx + face_dx(facing) * 8),
                  (int16_t)(hy + face_dy(facing) * 8), base);
      { uint8_t sh;
        for(sh = 0; sh < SHOT_N; sh++)
          if(shot_life[sh])
            put_world(row, fx_spr + ((uint16_t)(shot_pow[sh] ? FX_BOLT : FX_FIRE) << 3),
                      shot_x[sh], shot_y[sh], base); }
    }
  }
  if(Z->ground == SNOW_GROUND && !Z->boss) draw_snow();
  draw_hud();
  fb_flush();
}

// ===================================================================================
// Movement
// ===================================================================================
static uint8_t solid_px(int16_t px, int16_t py) {
  return (uint8_t)(mget((uint8_t)(px >> 3), (uint8_t)(py >> 3)) >= SOLID_FROM);
}

// Which edge a move would cross, 1..4 for north, south, west, east, else 0
static uint8_t off_edge(int16_t nx, int16_t ny) {
  if(nx + FEET_X0 < 0) return 3;
  if(nx + FEET_X1 >= WORLD_PX_W) return 4;
  if(ny + FEET_Y0 < 0) return 1;
  if(ny + FEET_Y1 >= WORLD_PX_H) return 2;
  return 0;
}

// Hero and monster are the same size and collide by the same box around their feet,
// so one test serves both.
static uint8_t blocked(int16_t nx, int16_t ny) {
  int16_t ax = (int16_t)(nx + FEET_X0), bx = (int16_t)(nx + FEET_X1);
  int16_t ay = (int16_t)(ny + FEET_Y0), by = (int16_t)(ny + FEET_Y1);
  uint8_t k;
  if(off_edge(nx, ny)) return 1;
  for(k = 0; k < 4; k++)                  // the four corners of the feet, in turn
    if(solid_px((k & 1) ? bx : ax, (k & 2) ? by : ay)) return 1;
  return 0;
}

// Entering a zone: place the hero, wipe the monsters and let them come back fresh
// floor(v * to / from) without dividing
__attribute__((noinline)) static uint16_t scale_span(uint16_t v, uint16_t from, uint16_t to) {
  uint16_t acc = 0, i, n = 0;
  if(!from || v >= from) return to;
  for(i = 0; i < to; i++) {
    acc = (uint16_t)(acc + v);
    if(acc >= from) { acc = (uint16_t)(acc - from); n++; }
  }
  return n;
}

// Hold a coordinate between two fences. Four places wanted this, two of them for a
// pair of axes that differ only in their names.
// Kept out of line on purpose: measured smaller as a call than inlined.
__attribute__((noinline)) static int16_t clamp(int16_t v, int16_t lo, int16_t hi) {
  if(hi < lo) hi = lo;                    // a zone narrower than the screen
  return (v < lo) ? lo : ((v > hi) ? hi : v);
}

// Is the border tile at this pixel walkable? Both ends of the search ask the same
// question, only the axis differs.
__attribute__((noinline)) static uint8_t edge_open(uint8_t vert, uint8_t edge, int16_t px) {
  uint8_t t = (uint8_t)(px >> 3);
  return (uint8_t)((vert ? mget(edge, t) : mget(t, edge)) < SOLID_FROM);
}

// Nothing alive, nothing in the air, nothing swinging, and standing on a door counts
// for nothing until you step off it. Every way into a zone wants exactly this.
static void mons_clear(void) {
  uint8_t i;
  for(i = 0; i < MON_N; i++) { mons[i].hp = 0; mons[i].cd = (uint8_t)(i * 6); }
  for(i = 0; i < SHOT_N; i++) shot_life[i] = 0;
  atk_timer = 0;
  door_lock = 1;
}

// A boss belongs to his zone, not to the way you arrived: a stair and a saved game
// both have to find him standing there. Loading inside an arena used to leave it
// empty until you walked out and back in.
static void boss_place(uint8_t id) {
  MONST* b = &mons[BOSS_SLOT];            // a boss is a monster in the last slot
  if(!Z->boss || (boss_dead & (uint8_t)(1 << id))) return;
  b->type = Z->boss;
  b->x    = (int16_t)((uint16_t)Z->bossx << 3);
  b->y    = (int16_t)((uint16_t)Z->bossy << 3);
  b->hp   = mon_hp0[Z->boss];
  // The four state bytes sit together and end the struct, so naming the fourth as well
  // lets one word store stand for all of them - and a sidestep held over from the last
  // boss was being carried into this one.
  b->aggro = 0; b->cd = 0; b->frame = 0; b->slide = 0;
}

static void zone_enter(uint8_t id, uint8_t from) {
  uint8_t i;
  uint16_t ow = (uint16_t)(Z ? (Z->w << 3) : 0), oh = (uint16_t)(Z ? (Z->h << 3) : 0);
  zone_set(id);
  // 0..3 came in through the N, S, W or E edge. Zones differ in size, so the point
  // where the edge was crossed is carried over proportionally. The two axes differ
  // only in which coordinate is pinned, so they share one branch.
  if(from < 4) {
    if(from >= 2) { hx = (int16_t)(from == 2 ? WORLD_PX_W - 16 : 8);
                    hy = (int16_t)scale_span((uint16_t)hy, oh, (uint16_t)WORLD_PX_H); }
    else          { hy = (int16_t)(from == 0 ? WORLD_PX_H - 16 : 8);
                    hx = (int16_t)scale_span((uint16_t)hx, ow, (uint16_t)WORLD_PX_W); }
  }
  else if(from == 5) {                    // stepped through a doorway
    // The door tile itself is solid, so come out on the open tile beside the door
    // that leads back where we came from.
    uint8_t k;
    hx = (int16_t)((uint16_t)Z->doorx << 3);
    hy = (int16_t)((uint16_t)Z->doory << 3);
    for(k = 0; k < 4; k++) {
      int16_t tx = (int16_t)(Z->doorx + face_dx(k)), ty = (int16_t)(Z->doory + face_dy(k));
      if(tx >= 0 && ty >= 0 && tx < (int16_t)Z->w && ty < (int16_t)Z->h &&
         mget((uint8_t)tx, (uint8_t)ty) < SOLID_FROM) {
        hx = (int16_t)(tx << 3); hy = (int16_t)(ty << 3);
        break;
      }
    }
  }
  else {
    hx = (int16_t)((uint16_t)Z->spawnx << 3);
    hy = (int16_t)((uint16_t)Z->spawny << 3);
  }
  hx = clamp(hx, 8, (int16_t)(WORLD_PX_W - 16));
  hy = clamp(hy, 8, (int16_t)(WORLD_PX_H - 16));

  // You must come in through an actual gap in the wall you crossed, not through the
  // wall itself, so slide along that edge until the border tile is open.
  if(from < 4) {
    uint8_t vert = (uint8_t)(from >= 2);       // came through a side wall
    uint8_t edge = (uint8_t)(from == 0 ? (Z->h - 1) : from == 1 ? 0
                           : from == 2 ? (Z->w - 1) : 0);
    int16_t pos = vert ? hy : hx;
    int16_t lim = vert ? WORLD_PX_H : WORLD_PX_W;
    // Out from where the edge was crossed, a tile back then a tile on, until the wall
    // opens. Both directions ask the same question, so they are written once.
    for(i = 0; i < 256; i++) {
      int16_t p = (i & 1) ? (int16_t)(pos + ((i >> 1) << 3))
                          : (int16_t)(pos - ((i >> 1) << 3));
      if(p < 8 || p > lim - 16) continue;
      if(edge_open(vert, edge, p)) { if(vert) hy = p; else hx = p; break; }
    }
  }
  mons_clear();
  boss_place(id);
}

// One axis of a step. The axis that is not moving is handed in unchanged, so both
// directions share the walk-or-cross-the-border decision instead of spelling it twice.
// Returns 1 when the zone changed, which ends the stepping for this frame.
static uint8_t step_axis(int16_t nx, int16_t ny) {
  uint8_t e;
  if(!blocked(nx, ny)) { hx = nx; hy = ny; return 0; }
  e = off_edge(nx, ny);
  if(e && zones[cur_zone].link[e - 1] >= 0) {
    zone_enter((uint8_t)zones[cur_zone].link[e - 1], (uint8_t)(e - 1));
    return 1;
  }
  return 0;
}

// Only two stats can ever be under a scroll, so this takes the pair by name rather
// than walking the whole array.
// Kept out of line: measured smaller as a call than inlined.
__attribute__((noinline)) static void buff_spend(uint8_t k, uint32_t d) {
  if(!buff[k]) return;
  if(buff_left[k] <= d) buff[k] = 0;
  else                  buff_left[k] = buff_left[k] - d;
}

static void cam_update(void) {
  cam_x = clamp((int16_t)(hx - 60), 0, (int16_t)(WORLD_PX_W - SCR_W));
  cam_y = clamp((int16_t)(hy - 28), 0, (int16_t)(WORLD_PX_H - SCR_H));
}

// Kept out of line on purpose: measured smaller as a call than inlined.
__attribute__((noinline)) static uint8_t dmg_roll(uint8_t att, uint8_t def) {
  uint8_t a = (uint8_t)(att + (JOY_random() & 3));
  uint8_t r = (uint8_t)(def >> 1);
  return (uint8_t)((a > r) ? (a - r) : 1);
}

// Coins go straight to the purse; anything else wants a slot, and a full bag loses
// it. Answers whether the drop landed, which is what buying needs to know.
static uint8_t bag_add(uint8_t id) {
  uint8_t i;
  if(id == IT_COIN) { if(gold != COIN_MAX) gold++; return 1; }
  for(i = 0; i < BAG_N; i++) if(!bag[i]) { bag[i] = id; return 1; }
  return 0;
}

// Who stands in this box? Pass MON_N to ask about all of them, or an index to let a
// creature ignore itself while it looks for room to step into.
static uint8_t mon_at_box(int16_t bx, int16_t by, uint8_t skip) {
  uint8_t i;
  for(i = 0; i < MON_N; i++) {
    if(i == skip || !mons[i].hp) continue;
    { int16_t sz = IS_BOSS(mons[i].type) ? 15 : 7;
      if(bx + 7 >= mons[i].x && bx <= mons[i].x + sz &&
         by + 7 >= mons[i].y && by <= mons[i].y + sz) return (uint8_t)(i + 1); }
  }
  return 0;
}

// Three places wanted to step a creature onto a square only if both the map and the
// other creatures allow it, and each spelled the pair of checks out again.
static uint8_t mon_try(MONST* m, uint8_t i, int16_t nx, int16_t ny) {
  // A boss stands two bodies wide and two tall, and blocked() only ever measures one.
  // Without asking about his other three quarters he walks through pillars; corners
  // alone would not do either, since a whole tile fits between them at that size.
  uint8_t n = (uint8_t)(IS_BOSS(m->type) ? 4 : 1), k;
  for(k = 0; k < n; k++)
    if(blocked((int16_t)(nx + ((k & 1) << 3)), (int16_t)(ny + ((k & 2) << 2)))) return 0;
  if(mon_at_box(nx, ny, i)) return 0;
  m->x = nx; m->y = ny;
  return 1;
}

// Put a monster somewhere walkable outside town and away from the player
static void mon_spawn(uint8_t i) {
  uint8_t t;
  for(t = 0; t < 24; t++) {
    // One draw carries all three choices in separate bit ranges. Consecutive draws
    // are shifted copies of each other, so taking them one at a time pinned ty to tx
    // and hatched every creature along two diagonal stripes. Masking rather than
    // taking a remainder keeps libgcc out of it.
    uint16_t rv = JOY_random();
    uint8_t tx = (uint8_t)(rv & ZONE_MASKW);
    uint8_t ty = (uint8_t)((rv >> 8) & ZONE_MASKH);
    int16_t px, py;
    if(!Z->nmob) return;                  // a safe zone has nothing to spawn
    if(tx >= Z->w || ty >= Z->h) continue;
    if(tx >= Z->sx0 && tx <= Z->sx1 && ty >= Z->sy0 && ty <= Z->sy1) continue;
    if(mget(tx, ty) >= SOLID_FROM) continue;
    px = (int16_t)((uint16_t)tx << 3);
    py = (int16_t)((uint16_t)ty << 3);
    if(abs((int16_t)(px - hx)) < 48 && abs((int16_t)(py - hy)) < 40) continue;
    if(mon_at_box(px, py, i)) continue;   // never hatch on top of another one
    {
      uint8_t k = (uint8_t)((rv >> 13) & 3);
      if(k >= Z->nmob) k = 0;
      k = Z->mob[k];
      mons[i].type = k;
      mons[i].hp   = mon_hp0[k];
    }
    mons[i].x = px; mons[i].y = py;
    mons[i].aggro = 0; mons[i].cd = 0; mons[i].frame = 0;
    return;
  }
  mons[i].cd = 20;                        // nowhere good right now, try again later
}

// Rolls a creature's whole loot list. Bosses hand out their fixed prize on top.
static void roll_drops(uint8_t t) {
  uint8_t k;
  for(k = drop_ofs[t]; k < drop_ofs[t + 1]; k = (uint8_t)(k + 2))
    if((uint8_t)(JOY_random() & 0xFF) <= drop_tab[k + 1]) bag_add(drop_tab[k]);
}

static void mon_kill(uint8_t i) {
  uint8_t t = mons[i].type;
  uint8_t boss = (uint8_t)IS_BOSS(t);
  mons[i].hp = 0; mons[i].aggro = 0; mons[i].cd = MON_RESPAWN;
  if(boss) {
    boss_dead = (uint8_t)(boss_dead | (uint8_t)(1 << cur_zone));
    // Only the weapon depends on the class, the rest lives in the drop table.
    if(t == M_FROSTKING) bag_add((uint8_t)(pclass == CLS_MAGE ? IT_BKB : IT_RAPIRA));
  }
  roll_drops(t);
  if(boss) play(tune_fanfare);
  else { JOY_sound(150, 10); JOY_sound(95, 16); }
  gain_xp(mon_xp0[t]);
  if(t == M_MALENIA) {
    announce(44, "YOU WIN", tune_fanfare);
  }
}

// The hero taking a blow. A bite and a thrown thing differ only in what they sound
// like; the roll, the guard it goes against and the floor at zero are one and the same.
static void hero_hurt(uint8_t power, uint8_t pitch, uint8_t dur) {
  uint8_t d = dmg_roll(power, defence());
  JOY_sound(pitch, dur);
  php = (uint8_t)((d >= php) ? 0 : (php - d));
}

static void hit_mon(uint8_t i, uint8_t power) {
  uint16_t d = dmg_roll(power, mon_def0[mons[i].type]);
  mons[i].aggro = 1;                      // they only fight once provoked
  if(d < mons[i].hp) { mons[i].hp = (uint16_t)(mons[i].hp - d); JOY_sound(70, 8); }
  else mon_kill(i);
}

// Bosses do not respawn and always leave something behind
static void mon_act(uint8_t i) {
  MONST* m = &mons[i];
  int16_t dx, dy;
  if(!m->hp) {
    if(i == BOSS_SLOT) return;            // a fallen boss stays fallen
    if(m->cd) m->cd--;
    else mon_spawn(i);
    return;
  }
  m->frame ^= 1;
  dx = (int16_t)(hx - m->x);
  dy = (int16_t)(hy - m->y);

  if(!m->aggro && (mon_flag[m->type] & M_HUNTS) &&
     abs(dx) < (HUNT_RANGE << 3) && abs(dy) < (HUNT_RANGE << 3))
    m->aggro = 1;                         // it spotted you

  if(!m->aggro) {                         // idle drift
    // One draw decides both. Two would not: once the low bits pass the gate they are
    // zero, and the next shift carries that zero into the direction, which left the
    // creatures walking only up and left, forever, into the nearest corner.
    uint16_t rv = JOY_random();
    if(!(rv & 3)) {
      uint8_t d = (uint8_t)((rv >> 8) & 3);
      int16_t nx = (int16_t)(m->x + ((d == 3) ? 4 : ((d == 2) ? -4 : 0)));
      int16_t ny = (int16_t)(m->y + ((d == 1) ? 4 : ((d == 0) ? -4 : 0)));
      mon_try(m, i, nx, ny);
    }
    return;
  }

  if(m->cd) m->cd--;                      // one timer serves both the bite and the spit

  if((mon_flag[m->type] & M_SHOOTS) && !m->cd &&
     abs(dx) < SHOOT_RANGE && abs(dy) < SHOOT_RANGE) {
    // it spits along whichever axis you are on
    uint8_t d = (uint8_t)(abs(dx) > abs(dy) ? (dx > 0 ? FACE_RIGHT : FACE_LEFT)
                                            : (dy > 0 ? FACE_DOWN  : FACE_UP));
    if(shot_fire(1, m->x, m->y, d, mon_str0[m->type])) {
      m->cd = MON_BITE;
      JOY_sound(90, 8);
      return;
    }
  }

  { int16_t reach = IS_BOSS(m->type) ? 14 : 8;   // a boss is twice the size
  if(abs(dx) <= reach && abs(dy) <= reach) {
    if(m->cd) return;
    m->cd = MON_BITE;
    hero_hurt(mon_str0[m->type], 35, 20);
    return;
  } }

  {                                       // walk toward the player
    // A hunter outpaces a hero below 16 agility; the placid ones only amble.
    int16_t v  = (mon_flag[m->type] & M_HUNTS) ? MON_FAST : MON_SLOW;
    int16_t sx = (dx > 0) ? v : -v, sy = (dy > 0) ? v : -v;
    // The far axis first. When it is shut, edging along the near axis toward the hero
    // only rocks the creature about the very spot it cannot pass: it is two tiles wide,
    // so rounding a pillar means first going the other way. So the sidestep picks a
    // direction once and holds it until the far axis opens, and turns back only when
    // that way is shut too.
    uint8_t maj = (uint8_t)(abs(dx) > abs(dy));
    if(mon_try(m, i, (int16_t)(m->x + (maj ? sx : 0)),
                     (int16_t)(m->y + (maj ? 0 : sy)))) { m->slide = 0; return; }
    if(!m->slide) m->slide = (int8_t)(maj ? sy : sx);
    if(!mon_try(m, i, (int16_t)(m->x + (maj ? 0 : m->slide)),
                      (int16_t)(m->y + (maj ? m->slide : 0))))
      m->slide = (int8_t)-m->slide;        // that way is shut as well, turn around
  }
}

// Agility trims the warrior's swing. A mage is held back by having one bolt in the
// air at a time, so a shorter cooldown would buy nothing without several in flight.
// What one frame adds to the swing. The line is drawn to end at a hundred agility, and
// nothing clamps it there: a stat only reaches past a hundred under a scroll, and even
// then the swing lands a little under the slash arc rather than anywhere strange.
static uint16_t atk_gain(void) {
  uint16_t a = (pclass == CLS_WARRIOR) ? stat_of(S_AGI) : 0;
  return (uint16_t)(ATK_STEP + a * ATK_RISE);
}

// Whatever is standing on that square takes the hero's blow. Says whether it landed,
// which is all a bolt needs to know to spend itself.
static uint8_t strike(int16_t x, int16_t y) {
  uint8_t m = mon_at_box(x, y, MON_N);
  if(!m) return 0;
  hit_mon((uint8_t)(m - 1), attack_power());
  return 1;
}

static void do_attack(void) {
  uint8_t w = equip[SLOT_WEAP];
  if(!w) { JOY_sound(45, 22); return; }   // bare hands are not a weapon
  if(atk_acc < ATK_FULL) return;
  atk_acc = (uint16_t)(atk_acc - ATK_FULL);   // the overflow is carried, not thrown away
  if(items[w].slot == SLOT_WEAP && items[w].stat == S_INT) {
    if(!shot_fire(0, hx, hy, facing, 0)) return;   // the air is full, wait a beat
    JOY_sound(190, 8);
  }
  else {
    atk_timer = SLASH_FRAMES;
    JOY_sound(90, 10);
    strike((int16_t)(hx + face_dx(facing) * 8), (int16_t)(hy + face_dy(facing) * 8));
  }
}

// Both bolts fly and die the same way; only what they are looking for differs.
static void shot_step(uint8_t s) {
  int16_t x = (int16_t)(shot_x[s] + face_dx(shot_dir[s]) * ARROW_SPD);
  int16_t y = (int16_t)(shot_y[s] + face_dy(shot_dir[s]) * ARROW_SPD);
  shot_x[s] = x; shot_y[s] = y;
  shot_life[s]--;
  if(x < 0 || y < 0 || x >= WORLD_PX_W - 8 || y >= WORLD_PX_H - 8 ||
     solid_px((int16_t)(x + 4), (int16_t)(y + 4))) { shot_life[s] = 0; return; }
  if(shot_pow[s]) {                       // a monster threw it, so it hunts the hero
    if(abs((int16_t)(x - hx)) < 8 && abs((int16_t)(y - hy)) < 8) {
      hero_hurt(shot_pow[s], 30, 18);
      shot_life[s] = 0;
    }
    return;
  }
  // Spend this bolt, not slot zero: the hero's bolt lives there today only because his
  // cooldown outlasts a flight, and that is not a thing to lean on.
  if(strike(x, y)) shot_life[s] = 0;
}

// Death is final: the save page is wiped, so there is nothing to come back to.
// Returns 1 when the run is really over. An aegis in the bag spends itself first.
static uint8_t player_died(void) {
  uint8_t i;
  for(i = 0; i < BAG_N; i++) {
    if(bag[i] != IT_AEGIS) continue;
    bag[i] = IT_NONE;
    php = maxhp();
    for(i = 0; i < MON_N; i++) mons[i].cd = MON_BITE;   // one beat, not a coma
    // The same fourteen letters as before, so the line still starts where it did
    msg_screen(24, "THE AEGIS HELD");
    JOY_sound(200, 40); JOY_sound(255, 60);
    JOY_DLY_ms(1600);
    return 0;
  }
  save_erase();
  announce(38, "YOU DIED", tune_death);   // falling to the lowest note there is
  return 1;
}

// ===================================================================================
// Inventory screen
// ===================================================================================
// Left: the hero beside a 2x2 paperdoll, with its own EXIT button underneath.
// Right: the bag, four across and three down so the icons get as much room as possible.
#define GEAR_X0 20
#define GEAR_Y0 12
#define GEAR_S  16                        // step between slot boxes
#define GEAR_W  14
#define EXIT_X  20
#define EXIT_Y  45
#define EXIT_W  30
#define EXIT_H  10
#define BAG_X0  54
#define BAG_Y0  11
#define BAG_CW  18
#define BAG_CH  14
#define BAG_BW  16


// ===================================================================================
// Shared menu furniture. Every screen draws the same things: a grid of item cells, a
// cursor over one of them, a framed button and a description strip along the bottom.
// ===================================================================================
typedef struct {
  uint8_t x0, y0;                         // top left of cell zero
  uint8_t px, py;                         // pitch between cells
  uint8_t bw, bh;                         // box size
  uint8_t ix, iy;                         // where the icon sits inside the box
  uint8_t cols;
} GRID;

const GRID g_gear = { GEAR_X0, GEAR_Y0, GEAR_S, GEAR_S, GEAR_W, GEAR_W, 1, 1, 2 };
const GRID g_bag  = { BAG_X0,  BAG_Y0,  BAG_CW, BAG_CH, BAG_BW, BAG_CH, 2, 1, BAG_COLS };
// The counter is laid out like the bag screen, down to the grid: the trader stands
// where the hero stands, the two buttons sit under him where EXIT sits, and the cells
// are the same twelve in the same place.
// Centred in what is left of the screen once the grid has its half: the trader is
// sixteen across and the buttons thirty-two, in a strip that ends where BAG_X0 starts.
#define SHOP_FX  ((BAG_X0 - HERO_W) / 2)
#define SHOP_BX  ((BAG_X0 - SHOP_BW) / 2)
#define SHOP_BW  32
#define SHOP_BH  9
#define SHOP_BY0 37

// No multiply: this core would call into libgcc for a variable one
static void grid_xy(const GRID* g, uint8_t idx, uint8_t* ox, uint8_t* oy) {
  uint8_t c = idx, x = g->x0, y = g->y0;
  while(c >= g->cols) { c = (uint8_t)(c - g->cols); y = (uint8_t)(y + g->py); }
  while(c--) x = (uint8_t)(x + g->px);
  *ox = x; *oy = y;
}

static void grid_draw(const GRID* g, const uint8_t* list, uint8_t n) {
  uint8_t i, x, y;
  for(i = 0; i < n; i++) {
    grid_xy(g, i, &x, &y);
    fb_box(x, y, g->bw, g->bh);
    if(list[i]) fb_icon(list[i], (uint8_t)(x + g->ix), (uint8_t)(y + g->iy));
  }
}

static void grid_cursor(const GRID* g, uint8_t idx) {
  uint8_t x, y;
  grid_xy(g, idx, &x, &y);
  fb_invert(x, y, g->bw, g->bh);
}

static void ui_button(uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                      const char* label, uint8_t on) {
  fb_box(x, y, w, h);
  fb_text((uint8_t)(x + 5), (uint8_t)(y + 2), label);
  if(on) fb_invert(x, y, w, h);
}

// Rule plus one line at the bottom. Returns where a second field may start, or 0
// when the slot was empty and nothing more should be written.

// The fixed labels of every screen, drawn from a table so each one costs three bytes
// and an index instead of a call with three arguments.
typedef struct { uint8_t x, y, ofs; } LABEL;
const char label_txt[] =
  "BAG\0"
  "USE\0"
  "DROP\0"
  "LV\0"
  "HP\0"
  "DEF\0"
  "INVENTORY\0"
  "SAVE AND EXIT\0"
  "CHOOSE A CLASS\0"
  "NEW GAME\0"
  "CONTINUE\0"
  "SERGEY S. + CLAUDE OPUS 5\0"
  ;
const LABEL labels[] = {
  {   2,  1,   0 },
  {   6, 57,   4 },
  {  40, 57,   8 },
  {  42,  1,  13 },
  {   2, 40,  16 },
  {   2, 48,  19 },
  {  50, 44,  23 },
  {  50, 52,  33 },
  {  28,  1,  47 },
  {  45, 32,  62 },
  {  45, 42,  71 },
  {   2, 57,  80 },
};

static void labels_draw(uint8_t from, uint8_t to) {
  for(; from < to; from++)
    fb_text(labels[from].x, labels[from].y, label_txt + labels[from].ofs);
}

static uint8_t ui_name(uint8_t y, uint8_t id) {
  fb_hline(0, (uint8_t)(y - 2), SCR_W);
  if(!id) { fb_text(2, y, "EMPTY"); return 0; }
  // artifact names run past any fixed column, so the strip starts where the name ends
  return (uint8_t)(fb_text(2, y, item_names + items[id].nameofs) + FONT_ADV);
}

static void inv_info(uint8_t id) {
  uint8_t x = ui_name(57, id);
  if(!x || id >= IT_SCROLL) return;       // empty, or a scroll whose name says it all
  if(items[id].slot == SLOT_NONE) {
    if(items[id].bonus) {                 // a potion, where the bonus is what it heals
      x = fb_text(x, 57, "HEALS");
      fb_num((uint8_t)(x + FONT_ADV), 57, items[id].bonus);
    }
    else if(id == IT_AEGIS) fb_text(x, 57, "REVIVES");
    // the souvenir says nothing: its name already is the whole joke
  }
  else if(items[id].bonus) {              // armour guards, it has no stat of its own
    x = fb_text(x, 57, items[id].stat == S_ARMOR ? "DEF" : STAT_NAME(items[id].stat));
    x = fb_text((uint8_t)(x + FONT_ADV), 57, "+");
    x = fb_num(x, 57, items[id].bonus);
    // Which part of you it goes on. One name is long enough that the strip runs out
    // before this fits; better to leave it off than to print half a word.
    x = (uint8_t)(x + FONT_ADV);
    if(x + SLOT_NAME_W <= SCR_W) fb_text(x, 57, SLOT_NAME(items[id].slot));
  }
}

// Both screens with a purse open the same way: the money top right under its caption,
// a rule beneath, and a figure standing on the left. Only the word top left and which
// figure it is differ, and those the caller has already drawn or chosen.
static void ui_head(uint8_t fig, uint8_t fx, uint8_t fy) {
  fb_text((uint8_t)(fb_num(NUM_RIGHT | 127, 1, gold) - 6 * FONT_ADV), 1, "COINS");
  fb_hline(0, 9, SCR_W);
  fb_blit(ui_hero + (uint16_t)fig * HERO_BYTES, HERO_W, HERO_PAGES, fx, fy);
}

static void bag_equip(uint8_t bi) {
  uint8_t id = bag[bi];
  const ITEM* it = &items[id];
  if(!id || it->slot == SLOT_NONE) return;
  if(it->slot == SLOT_WEAP &&               // a mage cannot swing a sword
     it->stat != (uint8_t)(pclass == CLS_MAGE ? S_INT : S_STR)) {
    JOY_sound(40, 30);
    return;
  }
  bag[bi] = equip[it->slot];
  equip[it->slot] = id;
  JOY_sound(200, 15);
}

static void slot_unequip(uint8_t sl) {
  uint8_t i;
  if(!equip[sl]) return;
  for(i = 0; i < BAG_N; i++) {
    if(!bag[i]) {
      bag[i] = equip[sl];
      equip[sl] = IT_NONE;
      JOY_sound(120, 15);
      return;
    }
  }
  JOY_sound(40, 30);                      // no room in the bag
}

// What the counter pays: a quarter of what a thing is worth, so nothing can be
// bought and sold at a profit. A souvenir and an aegis are worth nothing, which is
// the point of both.
static uint16_t sell_price(uint8_t id) {
  return (uint16_t)(item_price[id] >> 2);
}

// The counter in town. One grid of twelve cells serves both halves of the trade: it
// shows the eleven things on offer, or your own twelve slots, and the button in the
// header says which. Buying and selling are then the same walk over the same cells.
static void screen_shop(void) {
  uint8_t sel = 0, sell = 0;
  while(1) {
    const uint8_t* list = sell ? bag : shop_id;
    uint8_t n    = (uint8_t)(sell ? BAG_N : SHOP_N);
    uint8_t id   = (uint8_t)(sel < n ? list[sel] : IT_NONE);

    fb_clear();
    // Both buttons are places the cursor can stand, not decoration: EXIT is the only
    // way out, so up from the top row lands on it rather than leaving.
    fb_text(2, 1, "SHOP");
    ui_head(HERO_SHOP, SHOP_FX, 12);
    ui_button(SHOP_BX, SHOP_BY0, SHOP_BW, SHOP_BH, "EXIT", (uint8_t)(sel == n));
    ui_button(SHOP_BX, (uint8_t)(SHOP_BY0 + SHOP_BH), SHOP_BW, SHOP_BH,
              sell ? "SELL" : "BUY", (uint8_t)(sel == n + 1));
    grid_draw(&g_bag, list, n);
    if(sel < n) {
      grid_cursor(&g_bag, sel);
      inv_info(id);                       // name and what it grants, as in the bag
      if(id) fb_num(NUM_RIGHT | 127, 57, sell ? sell_price(id) : BUY(id));
    }
    else fb_hline(0, 55, SCR_W);          // the strip keeps its rule on the buttons
    fb_flush();

    switch(ui_input()) {
      // Rows do not run into one another: off the left edge of any of them lies the
      // pair of buttons, and the right edge is simply the end of that row.
      case IN_LEFT:
        if(sel >= n)                        break;
        if(sel & (BAG_COLS - 1))            sel--;
        else                                sel = n;
        break;
      case IN_RIGHT:
        if(sel >= n)                             sel = 0;
        else if((sel & (BAG_COLS - 1)) != BAG_COLS - 1) sel++;
        break;
      case IN_UP:
        if(sel == n + 1)             sel = n;          // the buttons are stacked
        else if(sel == n)            sel = (uint8_t)(n + 1);
        else if(sel >= BAG_COLS)     sel = (uint8_t)(sel - BAG_COLS);
        else                         sel = (uint8_t)(n - BAG_COLS + (sel & (BAG_COLS - 1)));
        break;
      case IN_DOWN:
        if(sel == n)                 sel = (uint8_t)(n + 1);
        else if(sel == n + 1)        sel = n;
        else if(sel + BAG_COLS < n)  sel = (uint8_t)(sel + BAG_COLS);
        else                         sel = (uint8_t)(sel & (BAG_COLS - 1));
        break;
      case IN_ACT:
        if(sel == n) { JOY_sound(180, 20); return; }
        if(sel == n + 1) { sell ^= 1; sel = 0; JOY_sound(160, 15); break; }
        if(sell) {
          if(id) {
            uint16_t g = (uint16_t)(gold + sell_price(id));
            gold = (g < gold) ? COIN_MAX : g;   // a purse that wrapped would read as empty
            bag[sel] = IT_NONE;
            JOY_sound(180, 10); JOY_sound(240, 16);   // coming in, so it rises
            break;
          }
        }
        else if(id && gold >= BUY(id) && bag_add(id)) {
          gold = (uint16_t)(gold - BUY(id));
          JOY_sound(240, 10); JOY_sound(180, 16);   // going out, so it falls
          break;
        }
        JOY_sound(40, 25);
        break;
      default: break;
    }
  }
}

static void screen_inventory(void) {
  uint8_t panel = 1, row = 0, col = 0;    // panel 0 = paperdoll and exit, 1 = bag
  uint8_t prompt = 0;                     // 0 off, 1 asking with USE lit, 2 with DROP
  while(1) {
    uint8_t sel_id;
    fb_clear();

    labels_draw(0, 1);
    ui_head(pclass, 2, 20);
    grid_draw(&g_gear, equip, 4);
    grid_draw(&g_bag, bag, BAG_N);
    ui_button(EXIT_X, EXIT_Y, EXIT_W, EXIT_H, "EXIT",
              (uint8_t)(panel == 0 && row == 2));

    if(panel == 0 && row == 2) { fb_hline(0, 55, SCR_W); sel_id = IT_NONE; }
    else {
      uint8_t idx;
      if(panel == 0) { idx = (uint8_t)((row << 1) + col); sel_id = equip[idx];
                       grid_cursor(&g_gear, idx); }
      else           { idx = (uint8_t)(row * BAG_COLS + col); sel_id = bag[idx];
                       grid_cursor(&g_bag, idx); }
      if(prompt) {                        // the bag is finite, so anything can go
        fb_hline(0, 55, SCR_W);
        labels_draw(1, 3);
        if(prompt == 1) fb_invert(4, 56, 22, 9);
        else            fb_invert(38, 56, 27, 9);
      }
      else inv_info(sel_id);
    }
    fb_flush();

    if(prompt) {                          // answering the use-or-drop question
      uint8_t bi = (uint8_t)(row * BAG_COLS + col);
      switch(ui_input()) {
        case IN_LEFT:  prompt = 1; break;
        case IN_RIGHT: prompt = 2; break;
        case IN_ACT:
          if(prompt == 2) { bag[bi] = IT_NONE; JOY_sound(60, 25); }
          else if(bag[bi] && items[bag[bi]].slot != SLOT_NONE) bag_equip(bi);
          else if(bag[bi] >= IT_SCROLL) { // every scroll is the same promise
            uint8_t k = items[bag[bi]].stat;
            buff[k]      = items[bag[bi]].bonus;
            buff_left[k] = SCROLL_TICKS;
            bag[bi] = IT_NONE;
            JOY_sound(230, 25);
          }
          else if(items[bag[bi]].slot == SLOT_NONE && items[bag[bi]].bonus) {
            uint8_t m = maxhp(), h = items[bag[bi]].bonus;
            php = (uint8_t)((php + h > m) ? m : php + h);
            bag[bi] = IT_NONE;
            JOY_sound(220, 20);
          }
          prompt = 0;
          break;
        default: prompt = 0; break;       // up or down backs out
      }
      continue;
    }

    switch(ui_input()) {
      case IN_UP:
        row = (uint8_t)(row ? row - 1 : (panel == 0 ? 2 : BAG_ROWS - 1));
        if(panel == 0 && row == 2) col = 0;
        break;
      case IN_DOWN:
        row++;
        if(row > (panel == 0 ? 2 : BAG_ROWS - 1)) row = 0;
        if(panel == 0 && row == 2) col = 0;
        break;
      case IN_LEFT:
        if(panel == 1) {
          if(col) col--;
          else { panel = 0; col = 1; if(row > 2) row = 2; }
        }
        else if(row < 2 && col) col--;    // never leaves the screen any more
        break;
      case IN_RIGHT:
        if(panel == 0) {
          if(row < 2 && col == 0) col = 1;
          else { panel = 1; col = 0; if(row > BAG_ROWS - 1) row = BAG_ROWS - 1; }
        }
        else if(col < BAG_COLS - 1) col++;
        break;
      default:
        if(panel == 0) {
          if(row == 2) return;            // EXIT
          slot_unequip((uint8_t)((row << 1) + col));
        }
        else if(bag[(uint8_t)(row * BAG_COLS + col)]) prompt = 1;
        break;
    }
  }
}

// ===================================================================================
// Character screen
// ===================================================================================
// Left and right move points in and out of a stat, up and down change rows, and
// SAVE AND EXIT is what commits the lot and writes flash.
#define CH_ROWS 6

static void screen_character(void) {
  uint8_t sel = 4, i;     // opens on INVENTORY, which is what it is opened for
  uint8_t base[4];
  for(i = 0; i < 4; i++) base[i] = st[i];  // points can only be taken back to here

  while(1) {
    uint8_t x, k;
    fb_clear();

    fb_text(2, 1, CLASS_NAME(pclass));
    labels_draw(3, 4);
    x = fb_num(54, 1, plvl);
    fb_num(fb_text((uint8_t)(x + 6), 1, "PTS"), 1, ppoints);
    fb_hline(0, 9, SCR_W);

    fb_blit(ui_hero + (uint16_t)pclass * HERO_BYTES, HERO_W, HERO_PAGES, 8, 14);
    labels_draw(4, 5);
    fb_num(fb_text(fb_num(14, 40, php), 40, "/"), 40, maxhp());
    labels_draw(5, 6);
    fb_num(22, 48, defence());

    for(i = 0; i < 4; i++) {
      uint8_t y = (uint8_t)(12 + (i << 3));
      uint8_t b = (uint8_t)(equip_bonus(i) + buff[i]);   // a scroll reads as gear
      fb_text(50, y, STAT_NAME(i));
      x = fb_num(72, y, st[i]);
      if(b) fb_num(fb_text(x, y, "+"), y, b);
      if(sel == i) fb_text(104, y, "-+");  // reminder that left and right move points
    }
    labels_draw(6, 8);

    fb_invert(48, (uint8_t)(11 + (sel << 3)), 70, 8);
    fb_flush();

    k = ui_input();
    switch(k) {
      case IN_UP:   sel = (uint8_t)(sel ? sel - 1 : CH_ROWS - 1); break;
      case IN_DOWN: sel = (uint8_t)((sel < CH_ROWS - 1) ? sel + 1 : 0); break;
      // Spending a point and taking it back are the same move in two directions, so
      // they are written once. Health follows vitality either way, and the ceiling it
      // is held under has already moved by the time it is checked.
      case IN_RIGHT:
      case IN_LEFT: {
        uint8_t up = (uint8_t)(k == IN_RIGHT);
        if(sel < 4 && (up ? (ppoints != 0) : (st[sel] > base[sel]))) {
          if(up) { ppoints--; st[sel]++; }
          else   { st[sel]--; ppoints++; }
          if(sel == S_VIT) {
            uint8_t m = maxhp();
            php = up ? (uint8_t)(php + 4) : (uint8_t)((php > 4) ? php - 4 : 1);
            if(php > m) php = m;
          }
          JOY_sound(up ? 200 : 120, 15);
        }
        else JOY_sound(40, 20);
        break; }
      default:
        if(sel == 4) screen_inventory();
        else if(sel == 5) {
          uint8_t ok = save_write();
          msg_screen(34, ok ? "GAME SAVED" : "SAVE FAILED");
          JOY_sound(ok ? 220 : 40, 25);
          JOY_DLY_ms(800);
          return;
        }
        else JOY_sound(40, 20);           // ACT on a stat does nothing, use left/right
        break;
    }
  }
}

// ===================================================================================
// Class choice, shown when starting a new game
// ===================================================================================
static uint8_t screen_class(void) {
  uint8_t sel = 0;
  while(1) {
    uint8_t c, i;
    fb_clear();
    labels_draw(8, 9);
    fb_hline(0, 9, SCR_W);
    for(i = 0; i < 4; i++) fb_text((uint8_t)(56 + i * 18), 12, STAT_NAME(i));

    for(c = 0; c < CLS_N; c++) {
      uint8_t y = (uint8_t)(22 + c * 18);
      fb_icon(class_weapon[c], 4, y);
      fb_text(20, (uint8_t)(y + 3), CLASS_NAME(c));
      for(i = 0; i < 4; i++)
        fb_num((uint8_t)(60 + i * 18), (uint8_t)(y + 3), class_stat[c][i]);
    }
    fb_invert(2, (uint8_t)(20 + sel * 18), 124, 16);
    inv_info(class_weapon[sel]);         // read the weapon rather than describe it twice
    fb_flush();

    switch(ui_input()) {
      case IN_UP:
      case IN_DOWN:  sel ^= 1; break;
      case IN_ACT:   JOY_sound(180, 20); return sel;
      default:       break;
    }
  }
}

// ===================================================================================
// Title screen
// ===================================================================================
static uint8_t screen_title(void) {
  uint8_t has_save = save_exists();
  uint8_t sel = has_save;                 // land on Continue when there is one
  while(1) {
    fb_clear();
    fb_text3(4, 1, "TINY RPG");
    fb_blit(ui_hero, HERO_W, HERO_PAGES, 4, 28);              // warrior on the left
    fb_blit(ui_hero + HERO_BYTES, HERO_W, HERO_PAGES, 108, 28);   // mage on the right
    labels_draw(9, (uint8_t)(has_save ? 11 : 10));
    fb_invert(42, (uint8_t)(31 + (sel * 10)), 46, 9);
    labels_draw(11, 12);
    fb_flush();

    switch(ui_input()) {
      case IN_UP:
      case IN_DOWN:
        if(has_save) sel ^= 1;
        break;
      case IN_ACT:
        JOY_sound(180, 20);
        return sel;
      default:
        break;
    }
  }
}

// ===================================================================================
// Main
// ===================================================================================
int main(void) {
  JOY_init();

  while(1) {
    uint8_t act_hold = 0, act_used = 0;

    if(screen_title()) save_read();
    else               new_game(screen_class());

    rnval = (uint16_t)(entropy | 1);      // the wait on the title screen is our seed
    mon_tick = 0;

    act_used = 1;                        // the button that started us is not a swing
    cam_update();
    draw_world();

    while(1) {
      int8_t dx, dy;
      uint8_t moved = joy_delta(ADC_read(), &dx, &dy);
      uint8_t redraw = 0;

      // A tap attacks on release, a hold opens the menu and eats that release
      if(JOY_act_pressed()) {
        if(act_hold < 255) act_hold++;
        if(act_hold >= HOLD_FRAMES && !act_used) {
          act_used = 1;
          screen_character();
          cam_update();
          redraw = 1;
        }
      }
      else {
        if(act_hold && !act_used) { do_attack(); redraw = 1; }
        act_hold = 0; act_used = 0;
      }

      // Scrolls are spent in real time, but a menu stops the world: a pass that took
      // longer than any frame ever could was a menu, and is not charged for.
      { uint32_t n = NOW(), d = (uint32_t)(n - clk);
        clk = n;
        if(d > MENU_GAP) d = 0;
        { uint8_t k; for(k = 0; k < 3; k++) buff_spend(mark_stat[k], d); } }
      if(atk_timer) { atk_timer--; redraw = 1; }
      if(atk_acc < ATK_FULL) atk_acc = (uint16_t)(atk_acc + atk_gain());
      if(Z->ground == SNOW_GROUND) {      // snow keeps falling whether you move or not
        snow_t++;
        if(!(snow_t & 3)) redraw = 1;
      }
      { uint8_t sh;
        for(sh = 0; sh < SHOT_N; sh++) if(shot_life[sh]) { shot_step(sh); redraw = 1; } }

      {                                   // a door leads somewhere: step into it
        int16_t fx = (int16_t)(hx + 3 + face_dx(facing) * 6);
        int16_t fy = (int16_t)(hy + 6 + face_dy(facing) * 6);
        uint8_t tx = (uint8_t)(fx >> 3), ty = (uint8_t)(fy >> 3);
        if(fx >= 0 && fy >= 0 && mget(tx, ty) == TW_SHOP) {
          if(!door_lock) {                // the counter opens the same way a door does
            door_lock = 1;
            screen_shop();
            act_used = 1;               // the press that left the shop is not a swing
            cam_update();
            redraw = 1;
          }
        }
        else if(Z->door >= 0 && fx >= 0 && fy >= 0 &&
                tx == Z->doorx && ty == Z->doory) {
          if(!door_lock) {
            door_lock = 1;
            zone_enter((uint8_t)Z->door, 5);
            cam_update();
            redraw = 1;
          }
        }
        else door_lock = 0;
      }

      if(++mon_tick >= MON_PERIOD) {
        uint8_t k;
        mon_tick = 0;
        for(k = 0; k < MON_N; k++) mon_act(k);
        redraw = 1;
      }
      if(!php && player_died()) break;      // unless an aegis brings you back

      if(moved) {
        // Agility buys a fraction of a pixel a frame. Riding mon_tick gave only four
        // rungs, all of them below thirty agility; carrying the remainder in an
        // accumulator spends every point instead, the way the healing does. Thirty-two
        // to the step, so the mask and the shift stand in for a division.
        uint8_t g = stat_of(S_AGI);
        uint8_t s, steps;
        walk_acc = (uint8_t)(walk_acc + g);
        steps = (uint8_t)(SPEED_MIN + (walk_acc >> 5));
        walk_acc &= 31;
        for(s = 0; s < steps; s++) {
          if(dx && step_axis((int16_t)(hx + dx), hy)) break;
          if(dy && step_axis(hx, (int16_t)(hy + dy))) break;
        }
        facing = dx ? ((dx > 0) ? FACE_RIGHT : FACE_LEFT)
                    : ((dy > 0) ? FACE_DOWN  : FACE_UP);
        if(++anim >= ANIM_RATE) { anim = 0; frame ^= 1; }
        cam_update();
        redraw = 1;
      }

      if(redraw) {
        uint8_t m = maxhp();
        if(php && php < m) {              // the dead do not mend
          regen = (uint16_t)(regen + m);
          if(regen >= REGEN_FULL) { regen = (uint16_t)(regen - REGEN_FULL); php++; }
        }
        else regen = 0;
        draw_world();
      }
      JOY_DLY_ms(10);
    }
  }
}
