// Doom sound on the Creator 5's single-voice buzzer. Effects are the WAD's PC
// speaker lumps (DP*). Music is reduced to its highest sounding note, and an
// effect takes over the buzzer while it plays. A thread sets the tone with
// cmd_pwm, one process at a time, so a change arriving during a call just
// replaces the one before it.
#include <math.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "doomtype.h"
#include "i_sound.h"
#include "sounds.h"
#include "w_wad.h"
#include "z_zone.h"

extern char **environ;

// config variables that normally belong to the SDL sound layer
int use_libsamplerate = 0;
float libsamplerate_scale = 0.65f;

#define SFX_RATE 140      // PC speaker lumps and MUS music both tick at 140 Hz
#define BASE_FREQ 175.0   // the tone of PC speaker value 1
#define PWM_CLOCK 50000000
#define MIN_NOTE_MS 20
#define OCTAVE_UP 2        // effects and music play an octave above their written pitch

typedef struct { uint32_t at_ms; uint16_t freq; } segment_t;
typedef struct { segment_t *segments; int count; uint32_t length_ms; } song_t;

static pthread_t thread;
static volatile int running;
static const char *pwm_cmd = "cmd_pwm";
static const char *pwm_channel = "pc12";
static int level = 60;

// shared with the thread; each is written whole
static const byte *volatile sfx_data;
static volatile int sfx_len;
static volatile uint32_t sfx_start;
static volatile int sfx_channel = -1;
static volatile int sfx_on = 1;

static song_t *volatile music;
static volatile int music_loop, music_paused, music_volume = 8;
static volatile uint32_t music_start, music_paused_at;

static uint32_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void pwm(const char *a, const char *b, const char *c, const char *d, const char *e) {
  char *argv[] = {(char *)pwm_cmd, (char *)a, (char *)pwm_channel, (char *)b, (char *)c, (char *)d, (char *)e, NULL};
  pid_t pid;
  if (posix_spawnp(&pid, pwm_cmd, NULL, NULL, argv, environ) == 0) {
    waitpid(pid, NULL, 0);
  }
}

static void set_tone(int freq, int duty_level) {
  char high[16], low[16];
  if (freq <= 0) {
    pwm("set_wc", "1", "0", NULL, NULL);
    return;
  }
  const int period = PWM_CLOCK / freq / OCTAVE_UP;
  const int h = period * duty_level / 200;
  snprintf(high, sizeof(high), "%d", h);
  snprintf(low, sizeof(low), "%d", period - h);
  pwm("set_wc", high, low, NULL, NULL);
}

// the tone music wants right now, with a binary search through the song
static int music_freq(uint32_t now) {
  song_t *song = music;
  if (song == NULL || song->count == 0 || music_volume == 0) return 0;
  uint32_t pos = (music_paused ? music_paused_at : now) - music_start;
  if (pos >= song->length_ms) {
    if (!music_loop || song->length_ms == 0) return 0;
    pos %= song->length_ms;
  }
  int lo = 0, hi = song->count - 1;
  while (lo < hi) {
    const int mid = (lo + hi + 1) / 2;
    if (song->segments[mid].at_ms <= pos) lo = mid; else hi = mid - 1;
  }
  return song->segments[lo].at_ms <= pos && !music_paused ? song->segments[lo].freq : 0;
}

static void *sound_thread(void *unused) {
  (void)unused;
  // same setup as Klipper's creator5_beeper, which leaves the channel like this too
  pwm("disable", NULL, NULL, NULL, NULL);
  pwm("config", "freq=50000000", "max_level=300", "active_level=1", "accuracy_priority=freq");
  pwm("set_level", "100", NULL, NULL, NULL);
  pwm("set_prescale", "6", NULL, NULL, NULL);
  set_tone(0, 0);
  pwm("enable_channels", NULL, NULL, NULL, NULL);

  int last = 0;
  while (running) {
    const uint32_t now = now_ms();
    int freq;
    const byte *data = sfx_data;
    const uint32_t tick = (now - sfx_start) * SFX_RATE / 1000;
    if (data != NULL && (int)tick < sfx_len) {
      const int tone = data[tick];
      freq = tone ? (int)(BASE_FREQ * pow(2.0, (tone - 1) / 24.0)) : 0;
    } else {
      sfx_data = NULL;
      freq = music_freq(now);
    }
    if (freq != last) {
      set_tone(freq, level);
      last = freq;
    }
    usleep(3000);
  }
  return NULL;
}

static void start_thread(void) {
  if (running) return;
  const char *env = getenv("DOOM_PWM");
  if (env) pwm_cmd = env;
  env = getenv("DOOM_BEEP_LEVEL");
  if (env) { const int l = atoi(env); if (l >= 0 && l <= 100) level = l; }
  running = 1;
  if (pthread_create(&thread, NULL, sound_thread, NULL) != 0) running = 0;
}

// ---- effects

static boolean Sfx_Init(boolean use_sfx_prefix) {
  (void)use_sfx_prefix;
  start_thread();
  return running ? true : false;
}

static void Sfx_Shutdown(void) {
  // the launcher silences the buzzer: the thread may be inside cmd_pwm
  running = 0;
}

static int Sfx_GetLumpNum(sfxinfo_t *sfx) {
  char name[16];
  snprintf(name, sizeof(name), "dp%s", sfx->link ? sfx->link->name : sfx->name);
  return W_CheckNumForName(name);
}

static void Sfx_Update(void) {}
static void Sfx_UpdateParams(int channel, int vol, int sep) { (void)channel; (void)vol; (void)sep; }

static int Sfx_Start(sfxinfo_t *sfx, int channel, int vol, int sep) {
  (void)sep;
  if (sfx->lumpnum < 0 || vol == 0 || !sfx_on) return channel;
  const byte *lump = W_CacheLumpNum(sfx->lumpnum, PU_STATIC);
  const int size = W_LumpLength(sfx->lumpnum);
  if (size < 4) return channel;
  int count = lump[2] | lump[3] << 8;
  if (count > size - 4) count = size - 4;
  sfx_data = NULL;
  sfx_len = count;
  sfx_start = now_ms();
  sfx_channel = channel;
  sfx_data = lump + 4;
  return channel;
}

static void Sfx_Stop(int channel) {
  if (channel == sfx_channel) sfx_data = NULL;
}

static boolean Sfx_IsPlaying(int channel) {
  return channel == sfx_channel && sfx_data != NULL ? true : false;
}

static void Sfx_Cache(sfxinfo_t *sounds, int num) { (void)sounds; (void)num; }

static snddevice_t sfx_devices[] = {SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32};

sound_module_t DG_sound_module = {
  sfx_devices, sizeof(sfx_devices) / sizeof(sfx_devices[0]),
  Sfx_Init, Sfx_Shutdown, Sfx_GetLumpNum, Sfx_Update, Sfx_UpdateParams,
  Sfx_Start, Sfx_Stop, Sfx_IsPlaying, Sfx_Cache,
};

// ---- music: every format becomes a list of (time, tone) segments

typedef struct { uint32_t tick; int order; byte kind, note; uint32_t tempo; } event_t;
enum { EV_NOTE_OFF, EV_NOTE_ON, EV_TEMPO };

typedef struct { event_t *items; int count, cap; } events_t;

static void add_event(events_t *e, uint32_t tick, byte kind, byte note, uint32_t tempo) {
  if (e->count == e->cap) {
    e->cap = e->cap ? e->cap * 2 : 1024;
    e->items = realloc(e->items, (size_t)e->cap * sizeof(event_t));
  }
  event_t ev = {tick, e->count, kind, note, tempo};
  e->items[e->count++] = ev;
}

static int compare_events(const void *a, const void *b) {
  const event_t *x = a, *y = b;
  if (x->tick != y->tick) return x->tick < y->tick ? -1 : 1;
  // at the same tick, a note ends before the next one starts
  if (x->kind != y->kind && (x->kind == EV_NOTE_OFF || y->kind == EV_NOTE_OFF))
    return x->kind == EV_NOTE_OFF ? -1 : 1;
  return x->order - y->order;
}

static int note_freq(int note) {
  return (int)(440.0 * pow(2.0, (note - 69) / 12.0));
}

// ticks_per_ms converts ticks at tempo 0, or tempo events do with ppq
static song_t *build_song(events_t *e, double fixed_ms_per_tick, int ppq) {
  qsort(e->items, (size_t)e->count, sizeof(event_t), compare_events);
  song_t *song = calloc(1, sizeof(song_t));
  song->segments = malloc((size_t)(e->count + 1) * sizeof(segment_t));
  int sounding[128] = {0};
  double ms = 0, ms_per_tick = ppq ? 500000.0 / ppq / 1000.0 : fixed_ms_per_tick;
  uint32_t last_tick = 0;
  int current = 0;
  for (int i = 0; i < e->count; i++) {
    const event_t *ev = &e->items[i];
    ms += (ev->tick - last_tick) * ms_per_tick;
    last_tick = ev->tick;
    if (ev->kind == EV_TEMPO) {
      if (ppq) ms_per_tick = ev->tempo / (double)ppq / 1000.0;
      continue;
    }
    if (ev->kind == EV_NOTE_ON) sounding[ev->note & 127]++;
    else if (sounding[ev->note & 127] > 0) sounding[ev->note & 127]--;
    int top = 127;
    while (top >= 0 && !sounding[top]) top--;
    const int freq = top >= 0 ? note_freq(top) : 0;
    if (freq == current) continue;
    current = freq;
    // a note too short to hear is replaced by what follows it
    if (song->count > 0 && (uint32_t)ms - song->segments[song->count - 1].at_ms < MIN_NOTE_MS) {
      song->segments[song->count - 1].freq = (uint16_t)freq;
    } else {
      segment_t s = {(uint32_t)ms, (uint16_t)freq};
      song->segments[song->count++] = s;
    }
  }
  song->length_ms = (uint32_t)ms;
  free(e->items);
  return song;
}

static uint32_t read_varlen(const byte **p, const byte *end) {
  uint32_t v = 0;
  while (*p < end) {
    const byte b = *(*p)++;
    v = v << 7 | (b & 0x7f);
    if (!(b & 0x80)) break;
  }
  return v;
}

static song_t *parse_midi(const byte *d, int len) {
  if (len < 14) return NULL;
  const int tracks = d[10] << 8 | d[11];
  const int ppq = d[12] << 8 | d[13];
  if (ppq & 0x8000 || ppq == 0) return NULL;  // no SMPTE timing
  events_t e = {0};
  const byte *pos = d + 8 + (d[4] << 24 | d[5] << 16 | d[6] << 8 | d[7]);
  const byte *const file_end = d + len;
  for (int t = 0; t < tracks && pos + 8 <= file_end; t++) {
    const uint32_t size = (uint32_t)pos[4] << 24 | pos[5] << 16 | pos[6] << 8 | pos[7];
    const byte *p = pos + 8;
    const byte *end = (size > (uint32_t)(file_end - p)) ? file_end : p + size;
    pos = end;
    if (memcmp(p - 8, "MTrk", 4) != 0) continue;
    uint32_t tick = 0;
    byte status = 0;
    while (p < end) {
      tick += read_varlen(&p, end);
      if (p >= end) break;
      if (*p & 0x80) status = *p++;
      const byte kind = status & 0xf0, chan = status & 0x0f;
      if (status == 0xff) {
        if (p >= end) break;
        const byte meta = *p++;
        const uint32_t l = read_varlen(&p, end);
        if (l > (uint32_t)(end - p)) break;
        if (meta == 0x51 && l == 3) add_event(&e, tick, EV_TEMPO, 0, (uint32_t)p[0] << 16 | p[1] << 8 | p[2]);
        p += l;
      } else if (status == 0xf0 || status == 0xf7) {
        const uint32_t l = read_varlen(&p, end);
        if (l > (uint32_t)(end - p)) break;
        p += l;
      } else if (kind == 0xc0 || kind == 0xd0) {
        p += 1;
      } else {
        if (p + 2 > end) break;
        const byte note = p[0], vel = p[1];
        p += 2;
        if (chan == 9) continue;  // drums have no pitch
        if (kind == 0x90 && vel) add_event(&e, tick, EV_NOTE_ON, note, 0);
        else if (kind == 0x80 || kind == 0x90) add_event(&e, tick, EV_NOTE_OFF, note, 0);
      }
    }
  }
  return e.count ? build_song(&e, 0, ppq) : NULL;
}

static song_t *parse_mus(const byte *d, int len) {
  if (len < 16) return NULL;
  const int score_len = d[4] | d[5] << 8, start = d[6] | d[7] << 8;
  if (start >= len) return NULL;
  const byte *p = d + start;
  const byte *end = (start + score_len < len) ? p + score_len : d + len;
  byte chan_note[16] = {0};
  events_t e = {0};
  uint32_t tick = 0;
  int finished = 0;
  while (p < end && !finished) {
    const byte head = *p++;
    const int type = head >> 4 & 7, chan = head & 15;
    switch (type) {
      case 0:
        if (p < end) { if (chan != 15) add_event(&e, tick, EV_NOTE_OFF, *p & 127, 0); p++; }
        break;
      case 1:
        if (p < end) {
          const byte note = *p++;
          if (note & 0x80 && p < end) p++;
          if (chan != 15) { add_event(&e, tick, EV_NOTE_ON, note & 127, 0); chan_note[chan] = note; }
        }
        break;
      case 2: case 3: p++; break;
      case 4: p += 2; break;
      case 5: finished = 1; break;
      default: break;
    }
    if (head & 0x80 && !finished) tick += read_varlen(&p, end);
  }
  (void)chan_note;
  // one last event so the song's length includes its tail
  add_event(&e, tick, EV_NOTE_OFF, 0, 0);
  return e.count ? build_song(&e, 1000.0 / SFX_RATE, 0) : NULL;
}

static boolean Mus_Init(void) { start_thread(); return running ? true : false; }
static void Mus_Shutdown(void) { music = NULL; }
static void Mus_SetVolume(int volume) { music_volume = volume; }

static void Mus_Pause(void) {
  if (!music_paused) { music_paused_at = now_ms(); music_paused = 1; }
}

static void Mus_Resume(void) {
  if (music_paused) { music_start += now_ms() - music_paused_at; music_paused = 0; }
}

static void *Mus_Register(void *data, int len) {
  const byte *d = data;
  if (len >= 4 && memcmp(d, "MThd", 4) == 0) return parse_midi(d, len);
  if (len >= 4 && memcmp(d, "MUS\x1a", 4) == 0) return parse_mus(d, len);
  return NULL;
}

static void Mus_Unregister(void *handle) {
  song_t *song = handle;
  if (song == NULL) return;
  // the thread may still be reading it, so it goes only once it is not current
  if (music == song) { music = NULL; usleep(10000); }
  free(song->segments);
  free(song);
}

static void Mus_Play(void *handle, boolean looping) {
  music = NULL;
  music_loop = looping;
  music_paused = 0;
  music_start = now_ms();
  music = handle;
}

static void Mus_Stop(void) { music = NULL; }

static boolean Mus_IsPlaying(void) {
  song_t *song = music;
  if (song == NULL) return false;
  return music_loop || now_ms() - music_start < song->length_ms ? true : false;
}

static void Mus_Poll(void) {}

static snddevice_t music_devices[] = {SNDDEVICE_GENMIDI};

music_module_t DG_music_module = {
  music_devices, 1,
  Mus_Init, Mus_Shutdown, Mus_SetVolume, Mus_Pause, Mus_Resume,
  Mus_Register, Mus_Unregister, Mus_Play, Mus_Stop, Mus_IsPlaying, Mus_Poll,
};
