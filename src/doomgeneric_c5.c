// Creator 5 platform layer for doomgeneric: the framebuffer, and touch controls
// drawn over the game. Sound is in i_c5sound.c. See ../README.md.
#include "doomgeneric.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// after linux/input.h, which has its own KEY_ENTER and KEY_TAB
#include "doomkeys.h"

// Landscape coordinates. The game fills the height at 1.2x its 640x400 buffer.
#define SCREEN_W 800
#define SCREEN_H 480
#define GAME_W 768
#define GAME_X ((SCREEN_W - GAME_W) / 2)
#define PANEL_W 480
#define PANEL_H 800
#define MAX_CONTACTS 10
#define KEY_QUEUE 64

// The movement cross: a 3x3 grid, so its corners press two directions at once
#define CROSS_X 20
#define CROSS_Y 240
#define CROSS_CELL 70

typedef struct {
  int x, y, w, h;
  const char *label;
  unsigned char key;
  int held;
} button_t;

enum { UI_UP, UI_DOWN, UI_LEFT, UI_RIGHT,
       UI_MENU, UI_MAP, UI_YES, UI_OK, UI_WPN, UI_USE, UI_FIRE, UI_COUNT };

static button_t buttons[UI_COUNT] = {
  [UI_UP]    = {CROSS_X + CROSS_CELL,     CROSS_Y,                  CROSS_CELL, CROSS_CELL, NULL, KEY_UPARROW, 0},
  [UI_DOWN]  = {CROSS_X + CROSS_CELL,     CROSS_Y + 2 * CROSS_CELL, CROSS_CELL, CROSS_CELL, NULL, KEY_DOWNARROW, 0},
  [UI_LEFT]  = {CROSS_X,                  CROSS_Y + CROSS_CELL,     CROSS_CELL, CROSS_CELL, NULL, KEY_LEFTARROW, 0},
  [UI_RIGHT] = {CROSS_X + 2 * CROSS_CELL, CROSS_Y + CROSS_CELL,     CROSS_CELL, CROSS_CELL, NULL, KEY_RIGHTARROW, 0},
  [UI_MENU]  = {560, 236,  70,  70, "MENU", KEY_ESCAPE, 0},
  [UI_MAP]   = {634, 236,  70,  70, "MAP",  KEY_TAB, 0},
  [UI_YES]   = {708, 236,  70,  70, "YES",  'y', 0},
  [UI_OK]    = {560, 310,  70,  70, "OK",   KEY_ENTER, 0},
  [UI_WPN]   = {634, 310,  70,  70, "WPN",  '1', 0},
  [UI_USE]   = {708, 310,  70,  70, "USE",  KEY_USE, 0},
  [UI_FIRE]  = {560, 384, 218,  86, "FIRE", KEY_FIRE, 0},
};

// Frames are composed here and copied to the screen whole, so the game never
// shows without its buttons
static uint32_t back[PANEL_W * PANEL_H];
static uint32_t *screen;
static int stride_px = PANEL_W;
// 1 matches GrumpyScreen's display_rotate: 1. 3 is the other way up.
static int rotate = 1;

static int touch_fd = -1;
static FILE *touch_log;
// a contact is down while id >= 0
typedef struct { int id, x, y; } contact_t;
static contact_t contacts[MAX_CONTACTS];
static contact_t pending[MAX_CONTACTS];  // the older protocol sends them all per report
static int pending_count, older_protocol, slot, position_seen;
static int log_lines;

static struct { unsigned char key, pressed; } key_queue[KEY_QUEUE];
static unsigned key_head, key_tail;
static int weapon;

static int panel_offset(int lx, int ly) {
  const int px = rotate == 3 ? PANEL_W - 1 - ly : ly;
  const int py = rotate == 3 ? lx : PANEL_H - 1 - lx;
  return py * PANEL_W + px;
}

// 5x7 letters for the button labels: one byte per row, low 5 bits
static const struct { char c; unsigned char rows[7]; } font[] = {
  {'A', {14, 17, 17, 31, 17, 17, 17}}, {'E', {31, 16, 16, 30, 16, 16, 31}},
  {'F', {31, 16, 16, 30, 16, 16, 16}}, {'I', {14, 4, 4, 4, 4, 4, 14}},
  {'K', {17, 18, 20, 24, 20, 18, 17}}, {'M', {17, 27, 21, 21, 17, 17, 17}},
  {'N', {17, 25, 21, 19, 17, 17, 17}}, {'O', {14, 17, 17, 17, 17, 17, 14}},
  {'P', {30, 17, 17, 30, 16, 16, 16}}, {'R', {30, 17, 17, 30, 20, 18, 17}},
  {'S', {15, 16, 16, 14, 1, 1, 30}},   {'U', {17, 17, 17, 17, 17, 17, 14}},
  {'W', {17, 17, 17, 21, 21, 27, 17}}, {'Y', {17, 17, 10, 4, 4, 4, 4}},
};

static void draw_char(char c, int x, int y, int scale) {
  for (size_t f = 0; f < sizeof(font) / sizeof(font[0]); f++) {
    if (font[f].c != c) continue;
    for (int row = 0; row < 7; row++) {
      for (int col = 0; col < 5; col++) {
        if (!(font[f].rows[row] & (16 >> col))) continue;
        for (int sy = 0; sy < scale; sy++)
          for (int sx = 0; sx < scale; sx++)
            back[panel_offset(x + col * scale + sx, y + row * scale + sy)] = 0x00ffffff;
      }
    }
    return;
  }
}

static inline void blend(int lx, int ly, uint32_t color) {
  uint32_t *p = &back[panel_offset(lx, ly)];
  *p = ((*p >> 1) & 0x007f7f7f) + ((color >> 1) & 0x007f7f7f);
}

// An arrow pointing away from the middle of the cross
static void draw_arrow(int which, const button_t *b) {
  const int cx = b->x + b->w / 2, cy = b->y + b->h / 2;
  for (int along = -14; along <= 14; along++) {
    const int half = 14 - along > 0 ? (14 - along) * 3 / 4 : 0;
    for (int across = -half; across <= half; across++) {
      int x = cx, y = cy;
      switch (which) {
        case UI_UP:    x += across; y -= along; break;
        case UI_DOWN:  x += across; y += along; break;
        case UI_LEFT:  x -= along; y += across; break;
        default:       x += along; y += across; break;
      }
      back[panel_offset(x, y)] = 0x00ffffff;
    }
  }
}

// The buttons sit over the game, half see-through, and go on top of each frame
static void draw_button(int which) {
  const button_t *b = &buttons[which];
  const uint32_t fill = b->held ? 0x00e03020 : 0x00303030;
  for (int y = b->y; y < b->y + b->h; y++) {
    for (int x = b->x; x < b->x + b->w; x++) {
      const int edge = x < b->x + 2 || x >= b->x + b->w - 2 ||
                       y < b->y + 2 || y >= b->y + b->h - 2;
      blend(x, y, edge ? 0x00c0c0c0 : fill);
    }
  }
  if (b->label == NULL) {
    draw_arrow(which, b);
    return;
  }
  const int scale = 2;
  const int width = (int)strlen(b->label) * 6 * scale - scale;
  int x = b->x + (b->w - width) / 2;
  const int y = b->y + (b->h - 7 * scale) / 2;
  for (const char *c = b->label; *c; c++, x += 6 * scale) draw_char(*c, x, y, scale);
}

static void queue_key(unsigned char key, int pressed) {
  if (key_head - key_tail >= KEY_QUEUE) return;
  key_queue[key_head % KEY_QUEUE].key = key;
  key_queue[key_head % KEY_QUEUE].pressed = (unsigned char)pressed;
  key_head++;
}

static void press_at(int lx, int ly, int *held) {
  if (lx >= CROSS_X && lx < CROSS_X + 3 * CROSS_CELL &&
      ly >= CROSS_Y && ly < CROSS_Y + 3 * CROSS_CELL) {
    const int col = (lx - CROSS_X) / CROSS_CELL, row = (ly - CROSS_Y) / CROSS_CELL;
    if (row == 0) held[UI_UP] = 1;
    if (row == 2) held[UI_DOWN] = 1;
    if (col == 0) held[UI_LEFT] = 1;
    if (col == 2) held[UI_RIGHT] = 1;
    return;
  }
  for (int i = UI_MENU; i < UI_COUNT; i++) {
    const button_t *b = &buttons[i];
    if (lx >= b->x && lx < b->x + b->w && ly >= b->y && ly < b->y + b->h) held[i] = 1;
  }
}

static void update_buttons(void) {
  int held[UI_COUNT] = {0};
  for (int c = 0; c < MAX_CONTACTS; c++) {
    if (contacts[c].id < 0) continue;
    // The panel reports its own portrait coordinates, whatever range it declares
    const int px = contacts[c].x, py = contacts[c].y;
    const int lx = rotate == 3 ? py : PANEL_H - 1 - py;
    const int ly = rotate == 3 ? PANEL_W - 1 - px : px;
    if (touch_log) fprintf(touch_log, "contact %d raw %d,%d screen %d,%d\n", c, px, py, lx, ly);
    press_at(lx, ly, held);
  }
  for (int i = 0; i < UI_COUNT; i++) {
    button_t *b = &buttons[i];
    if (held[i] == b->held) continue;
    if (i == UI_WPN && held[i]) {
      // each press picks the next weapon slot, 1 to 7
      b->key = (unsigned char)('1' + weapon);
      weapon = (weapon + 1) % 7;
    }
    b->held = held[i];
    if (touch_log) fprintf(touch_log, "button %d %s\n", i, held[i] ? "down" : "up");
    queue_key(b->key, held[i]);
  }
}

static void release_all_contacts(void) {
  for (int c = 0; c < MAX_CONTACTS; c++) contacts[c].id = -1;
}

// Handles both multi-touch protocols, and a panel that only sends one contact
static void handle_touch_event(const struct input_event *ev) {
  if (touch_log && log_lines++ > 20000) { fclose(touch_log); touch_log = NULL; }
  if (touch_log) fprintf(touch_log, "ev %d %d %d\n", ev->type, ev->code, ev->value);
  if (ev->type == EV_ABS) {
    switch (ev->code) {
      case ABS_MT_SLOT:
        slot = ev->value >= 0 && ev->value < MAX_CONTACTS ? ev->value : 0;
        break;
      case ABS_MT_TRACKING_ID:
        contacts[slot].id = ev->value;
        break;
      case ABS_MT_POSITION_X:
        contacts[slot].x = ev->value;
        position_seen = 1;
        break;
      case ABS_MT_POSITION_Y:
        contacts[slot].y = ev->value;
        position_seen = 1;
        break;
    }
  } else if (ev->type == EV_KEY && ev->code == BTN_TOUCH) {
    if (!ev->value) {
      release_all_contacts();
      pending_count = 0;
      position_seen = 0;
    } else if (contacts[slot].id < 0) contacts[slot].id = 0;
  } else if (ev->type == EV_SYN && ev->code == SYN_MT_REPORT) {
    older_protocol = 1;
    if (position_seen && pending_count < MAX_CONTACTS) {
      pending[pending_count] = contacts[slot];
      pending[pending_count].id = pending_count;
      pending_count++;
    }
    position_seen = 0;
  } else if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
    if (older_protocol) {
      release_all_contacts();
      memcpy(contacts, pending, (size_t)pending_count * sizeof(contact_t));
      pending_count = 0;
    }
    update_buttons();
    if (touch_log) fflush(touch_log);
  }
}

void c5_keyboard_poll(void (*emit)(unsigned char key, int pressed));

static void poll_touch(void) {
  struct input_event ev;
  while (touch_fd >= 0 && read(touch_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
    handle_touch_event(&ev);
  }
}

static void open_touch(void) {
  const char *dev = getenv("DOOM_TOUCH");
  const char *log = getenv("DOOM_TOUCH_LOG");
  release_all_contacts();
  if (log) touch_log = fopen(log, "w");
  touch_fd = open(dev ? dev : "/dev/input/event2", O_RDONLY | O_NONBLOCK);
  if (touch_fd < 0) fprintf(stderr, "c5doom: no touch device: %s\n", strerror(errno));
}

static void open_screen(void) {
  const char *raw = getenv("DOOM_FB_RAW");  // a plain file, for testing off the printer
  const int fd = open(raw ? raw : "/dev/fb0", O_RDWR);
  if (fd < 0) { perror("c5doom: framebuffer"); exit(1); }
  size_t size = PANEL_W * PANEL_H * 4;
  if (!raw) {
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &var) || ioctl(fd, FBIOGET_FSCREENINFO, &fix)) {
      perror("c5doom: framebuffer info");
      exit(1);
    }
    if (var.xres != PANEL_W || var.yres != PANEL_H || var.bits_per_pixel != 32) {
      fprintf(stderr, "c5doom: expected a %dx%d 32-bit screen, found %ux%u %u-bit\n",
              PANEL_W, PANEL_H, var.xres, var.yres, var.bits_per_pixel);
      exit(1);
    }
    stride_px = (int)(fix.line_length / 4);
    size = fix.smem_len;
    // always draw to the first page, wherever the last program left the display
    var.xoffset = var.yoffset = 0;
    ioctl(fd, FBIOPAN_DISPLAY, &var);
  }
  screen = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (screen == MAP_FAILED) { perror("c5doom: mmap"); exit(1); }
  close(fd);
}

static void handle_signal(int sig) {
  (void)sig;
  exit(0);
}

void DG_Init(void) {
  const char *r = getenv("DOOM_ROTATE");
  if (r && atoi(r) == 3) rotate = 3;
  signal(SIGTERM, handle_signal);
  signal(SIGINT, handle_signal);
  open_screen();
  for (int y = 0; y < PANEL_H; y++) memset(screen + y * stride_px, 0, PANEL_W * 4);
  open_touch();
}

void DG_DrawFrame(void) {
  poll_touch();
  for (int lx = 0; lx < GAME_W; lx++) {
    const int sx = lx * DOOMGENERIC_RESX / GAME_W;
    for (int ly = 0; ly < SCREEN_H; ly++) {
      const int sy = ly * DOOMGENERIC_RESY / SCREEN_H;
      back[panel_offset(GAME_X + lx, ly)] = DG_ScreenBuffer[sy * DOOMGENERIC_RESX + sx];
    }
  }
  for (int i = 0; i < UI_COUNT; i++) draw_button(i);
  for (int y = 0; y < PANEL_H; y++) {
    memcpy(screen + y * stride_px, back + y * PANEL_W, PANEL_W * 4);
  }
}

void DG_SleepMs(uint32_t ms) {
  usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void keyboard_key(unsigned char key, int pressed) {
  queue_key(key, pressed);
}

int DG_GetKey(int *pressed, unsigned char *key) {
  poll_touch();
  c5_keyboard_poll(keyboard_key);
  if (key_tail == key_head) return 0;
  *key = key_queue[key_tail % KEY_QUEUE].key;
  *pressed = key_queue[key_tail % KEY_QUEUE].pressed;
  key_tail++;
  return 1;
}

void DG_SetWindowTitle(const char *title) {
  (void)title;
}

int main(int argc, char **argv) {
  doomgeneric_Create(argc, argv);
  for (;;) doomgeneric_Tick();
  return 0;
}
