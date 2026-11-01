// USB keyboards for c5doom: any input device with letter keys, plugged in
// before or during the game.
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// linux/input.h and doomkeys.h use some of the same names with different
// values, so the Linux codes are kept here before doomkeys.h replaces them.
enum {
  L_ESC = KEY_ESC, L_ENTER = KEY_ENTER, L_KPENTER = KEY_KPENTER, L_TAB = KEY_TAB,
  L_SPACE = KEY_SPACE, L_BACKSPACE = KEY_BACKSPACE, L_UP = KEY_UP, L_DOWN = KEY_DOWN,
  L_LEFT = KEY_LEFT, L_RIGHT = KEY_RIGHT, L_LCTRL = KEY_LEFTCTRL, L_RCTRL = KEY_RIGHTCTRL,
  L_LSHIFT = KEY_LEFTSHIFT, L_RSHIFT = KEY_RIGHTSHIFT, L_LALT = KEY_LEFTALT,
  L_RALT = KEY_RIGHTALT, L_MINUS = KEY_MINUS, L_EQUAL = KEY_EQUAL, L_COMMA = KEY_COMMA,
  L_DOT = KEY_DOT, L_SLASH = KEY_SLASH, L_SEMICOLON = KEY_SEMICOLON,
  L_APOSTROPHE = KEY_APOSTROPHE, L_LEFTBRACE = KEY_LEFTBRACE, L_RIGHTBRACE = KEY_RIGHTBRACE,
  L_BACKSLASH = KEY_BACKSLASH, L_GRAVE = KEY_GRAVE, L_PAUSE = KEY_PAUSE,
  L_F1 = KEY_F1, L_F10 = KEY_F10, L_F11 = KEY_F11, L_F12 = KEY_F12,
  L_1 = KEY_1, L_0 = KEY_0, L_A = KEY_A, L_SPACE_BIT = KEY_SPACE, L_MAX = KEY_MAX,
};

#include "doomkeys.h"

#define MAX_KEYBOARDS 4
#define MAX_EVENT_DEVICES 16
#define RESCAN_MS 1000

// the letter and symbol rows, by Linux key code
static const char row_chars[][2] = {
  {16, 'q'}, {17, 'w'}, {18, 'e'}, {19, 'r'}, {20, 't'}, {21, 'y'}, {22, 'u'}, {23, 'i'},
  {24, 'o'}, {25, 'p'}, {30, 'a'}, {31, 's'}, {32, 'd'}, {33, 'f'}, {34, 'g'}, {35, 'h'},
  {36, 'j'}, {37, 'k'}, {38, 'l'}, {44, 'z'}, {45, 'x'}, {46, 'c'}, {47, 'v'}, {48, 'b'},
  {49, 'n'}, {50, 'm'},
};

static int to_doom_key(int code) {
  switch (code) {
    case L_ESC: return KEY_ESCAPE;
    case L_ENTER: case L_KPENTER: return KEY_ENTER;
    case L_TAB: return KEY_TAB;
    case L_SPACE: return KEY_USE;
    case L_BACKSPACE: return KEY_BACKSPACE;
    case L_UP: return KEY_UPARROW;
    case L_DOWN: return KEY_DOWNARROW;
    case L_LEFT: return KEY_LEFTARROW;
    case L_RIGHT: return KEY_RIGHTARROW;
    case L_LCTRL: case L_RCTRL: return KEY_FIRE;
    case L_LSHIFT: case L_RSHIFT: return KEY_RSHIFT;
    case L_LALT: case L_RALT: return KEY_RALT;
    case L_MINUS: return KEY_MINUS;
    case L_EQUAL: return KEY_EQUALS;
    case L_COMMA: return KEY_STRAFE_L;
    case L_DOT: return KEY_STRAFE_R;
    case L_SLASH: return '/';
    case L_SEMICOLON: return ';';
    case L_APOSTROPHE: return '\'';
    case L_LEFTBRACE: return '[';
    case L_RIGHTBRACE: return ']';
    case L_BACKSLASH: return '\\';
    case L_GRAVE: return '`';
    case L_PAUSE: return KEY_PAUSE;
    case L_F11: return KEY_F11;
    case L_F12: return KEY_F12;
    case L_0: return '0';
  }
  if (code >= L_1 && code < L_0) return '1' + (code - L_1);
  if (code >= L_F1 && code <= L_F10) return KEY_F1 + (code - L_F1);
  for (size_t i = 0; i < sizeof(row_chars) / sizeof(row_chars[0]); i++) {
    if (row_chars[i][0] == code) return row_chars[i][1];
  }
  return 0;
}

static struct { int fd, number; } keyboards[MAX_KEYBOARDS];
static int started;
static long last_scan = -RESCAN_MS;
static unsigned checked;
static int scanned;
static struct timespec last_change;

static long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int has_letter_keys(int fd) {
  unsigned char bits[L_MAX / 8 + 1] = {0};
  if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(bits)), bits) < 0) return 0;
  return (bits[L_A / 8] >> (L_A % 8) & 1) && (bits[L_SPACE_BIT / 8] >> (L_SPACE_BIT % 8) & 1);
}

static void add_keyboard(int fd, int number) {
  for (int i = 0; i < MAX_KEYBOARDS; i++) {
    if (keyboards[i].fd >= 0) continue;
    keyboards[i].fd = fd;
    keyboards[i].number = number;
    return;
  }
  close(fd);
}

// Closing an input device takes this kernel about 100 ms, so each device is
// opened only once, when it first appears.
static void scan_devices(void) {
  for (int n = 0; n < MAX_EVENT_DEVICES; n++) {
    char path[32];
    snprintf(path, sizeof(path), "/dev/input/event%d", n);
    if (access(path, F_OK) != 0) {
      checked &= ~(1u << n);
      continue;
    }
    if (checked & (1u << n)) continue;
    checked |= 1u << n;
    const int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) continue;
    if (has_letter_keys(fd)) add_keyboard(fd, n);
    else close(fd);
  }
}

// /dev/input's time changes when a device comes or goes
static void scan_if_changed(void) {
  struct stat st;
  if (stat("/dev/input", &st) != 0) return;
  if (scanned && st.st_mtim.tv_sec == last_change.tv_sec &&
      st.st_mtim.tv_nsec == last_change.tv_nsec) return;
  scanned = 1;
  last_change = st.st_mtim;
  scan_devices();
}

void c5_keyboard_poll(void (*emit)(unsigned char key, int pressed)) {
  if (!started) {
    started = 1;
    for (int i = 0; i < MAX_KEYBOARDS; i++) keyboards[i].fd = -1;
    // for testing: a file or pipe of events instead of a real device
    const char *test = getenv("DOOM_KEYBOARD");
    if (test) { keyboards[0].fd = open(test, O_RDONLY | O_NONBLOCK); keyboards[0].number = -1; }
  }
  const long now = now_ms();
  if (now - last_scan >= RESCAN_MS) {
    last_scan = now;
    scan_if_changed();
  }
  for (int i = 0; i < MAX_KEYBOARDS; i++) {
    if (keyboards[i].fd < 0) continue;
    struct input_event ev;
    ssize_t got;
    while ((got = read(keyboards[i].fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
      if (ev.type != EV_KEY || ev.value == 2) continue;  // Doom has its own key repeat
      const int key = to_doom_key(ev.code);
      if (key) emit((unsigned char)key, ev.value);
    }
    if (got < 0 && errno == ENODEV) {  // unplugged
      close(keyboards[i].fd);
      keyboards[i].fd = -1;
      if (keyboards[i].number >= 0) checked &= ~(1u << keyboards[i].number);
    }
  }
}
