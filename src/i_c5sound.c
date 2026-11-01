// Doom sound on the Creator 5's buzzer, by the method FastDoom uses on a PC speaker:
// a real-time thread mixes samples and writes each one as a PWM duty cycle. Music
// plays every instrument as a square wave, because the buzzer can't reproduce a sine
// at these pitches, with noise for drums. Effects are the WAD's PC speaker lumps
// (DP*), mixed on top. cmd_pwm sets the channel up, then samples go straight to the
// PWM driver.
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
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

#define SAMPLE_RATE 16000
#define PWM_PERIOD 1000       // counts: 64 MHz at prescale 6 makes a 64 kHz carrier
#define PWM_MARGIN 20         // counts kept off each end of the period
#define PWM_REQUEST 0x8001500B  // /dev/jz_pwm, as libhardware2 calls it
#define PWM_SET_WC 0xC004502D
#define PWM_RETRIES 20        // the driver refuses until the last sample has latched
#define SPIN_NS 20000         // sleep to this close to each sample, then spin
#define RT_PRIORITY 20
#define AUDIO_CPU 1
#define RESYNC_S 0.005        // further behind than this, skip rather than catch up

#define SFX_RATE 140          // PC speaker lumps and MUS music both tick at 140 Hz
#define BASE_FREQ 175.0       // the tone of PC speaker value 1
#define SFX_CHANNELS 16
#define MUSIC_VOICES 24
#define DRUM_VOICES 4
#define CHANNELS 16
#define MUSIC_TRANSPOSE 12    // semitones: the buzzer is thin below this
#define MUSIC_GAIN 0.6f
#define SFX_GAIN 0.5f
#define HELD_FLOOR 0.35f      // held notes fade to this, so they don't bury the tune
#define HELD_FADE 0.0015f     // per ms

typedef struct { uint32_t at_ms; byte kind, chan, note, value; } song_event_t;
typedef struct { song_event_t *events; int count; uint32_t length_ms; } song_t;
enum { EV_NOTE_OFF, EV_NOTE_ON, EV_VOLUME, EV_ALL_OFF, EV_TEMPO };

typedef struct { uint32_t phase, step; float amp, env; byte chan, note, on; } voice_t;
typedef struct { float env, fade, amp; int hold, count; float value; } drum_t;
typedef struct { const byte *volatile data; volatile int len; volatile float amp; uint32_t pos; uint32_t phase; } sfx_t;

static pthread_t thread;
static volatile int running;
static const char *pwm_cmd = "cmd_pwm";
static const char *pwm_channel = "pc12";
static float master = 1.f;
static int pwm_fd = -1, pwm_index;

// shared with the thread; each is written whole
static sfx_t sfx[SFX_CHANNELS];
static song_t *volatile music;
static volatile int music_loop, music_paused, music_volume = 64;
static volatile int music_generation;
static volatile int music_done;

// the thread's own
static voice_t voices[MUSIC_VOICES];
static drum_t drums[DRUM_VOICES];
static float chan_volume[CHANNELS];
static int playing_generation = -1, next_event;
static song_t *playing;
static uint32_t music_samples;
static uint32_t noise = 0xace1;
static uint32_t sfx_steps[256];

static void pwm(const char *a, const char *b, const char *c, const char *d, const char *e) {
  char *argv[] = {(char *)pwm_cmd, (char *)a, (char *)pwm_channel, (char *)b, (char *)c, (char *)d, (char *)e, NULL};
  pid_t pid;
  if (posix_spawnp(&pid, pwm_cmd, NULL, NULL, argv, environ) == 0) {
    waitpid(pid, NULL, 0);
  }
}

static void write_sample(int high, int low) {
  uint32_t arg[2] = {(uint32_t)pwm_index, (uint32_t)high << 16 | (uint32_t)low};
  for (int i = 0; i < PWM_RETRIES; i++) {
    if (ioctl(pwm_fd, PWM_SET_WC, arg) == 0) return;
  }
}

static uint32_t step_for(double freq) {
  return (uint32_t)(freq * 4294967296.0 / SAMPLE_RATE);
}

static void all_notes_off(void) {
  memset(voices, 0, sizeof(voices));
  memset(drums, 0, sizeof(drums));
  for (int i = 0; i < CHANNELS; i++) chan_volume[i] = 100 / 127.f;
}

static void start_drum(int note, float velocity) {
  static int next;
  drum_t *d = &drums[next];
  next = (next + 1) % DRUM_VOICES;
  d->env = 1.f;
  d->count = 0;
  if (note == 35 || note == 36) {         // kick: low, dull noise
    d->hold = 8; d->fade = 0.992f; d->amp = velocity * 1.2f;
  } else if (note == 38 || note == 40) {  // snare
    d->hold = 1; d->fade = 0.996f; d->amp = velocity;
  } else {                                // hats and cymbals
    d->hold = 1; d->fade = 0.985f; d->amp = velocity * 0.5f;
  }
}

static void note_on(int chan, int note, float velocity) {
  voice_t *v = NULL;
  for (int i = 0; i < MUSIC_VOICES && !v; i++) if (!voices[i].on) v = &voices[i];
  if (!v) {  // none free: take over whichever has faded most
    v = &voices[0];
    for (int i = 1; i < MUSIC_VOICES; i++) if (voices[i].env < v->env) v = &voices[i];
  }
  v->step = step_for(440.0 * pow(2.0, (note + MUSIC_TRANSPOSE - 69) / 12.0));
  v->amp = velocity;
  v->env = 1.f;
  v->chan = (byte)chan;
  v->note = (byte)note;
  v->on = 1;
}

static void note_off(int chan, int note) {
  for (int i = 0; i < MUSIC_VOICES; i++) {
    if (voices[i].on && voices[i].chan == chan && voices[i].note == note) {
      voices[i].on = 0;
      return;
    }
  }
}

static void run_event(const song_event_t *ev) {
  switch (ev->kind) {
    case EV_NOTE_ON:
      if (ev->chan == 15) start_drum(ev->note, ev->value / 127.f);
      else note_on(ev->chan, ev->note, ev->value / 127.f);
      break;
    case EV_NOTE_OFF:
      note_off(ev->chan, ev->note);
      break;
    case EV_VOLUME:
      chan_volume[ev->chan] = ev->value / 127.f;
      break;
    case EV_ALL_OFF:
      for (int i = 0; i < MUSIC_VOICES; i++) if (voices[i].chan == ev->chan) voices[i].on = 0;
      break;
  }
}

static float next_music_sample(void) {
  song_t *song = music;
  const int generation = music_generation;
  if (song != playing || generation != playing_generation) {
    all_notes_off();
    playing = song;
    playing_generation = generation;
    next_event = 0;
    music_samples = 0;
    music_done = 0;
  }
  if (song == NULL || music_paused || music_done) return 0.f;

  const uint32_t now_ms = music_samples / (SAMPLE_RATE / 1000);
  while (next_event < song->count && song->events[next_event].at_ms <= now_ms) {
    run_event(&song->events[next_event++]);
  }
  if (now_ms >= song->length_ms) {
    all_notes_off();
    next_event = 0;
    music_samples = 0;
    if (!music_loop) music_done = 1;
    return 0.f;
  }
  const int tick_ms = music_samples % (SAMPLE_RATE / 1000) == 0;
  music_samples++;

  float mix = 0.f;
  for (int i = 0; i < MUSIC_VOICES; i++) {
    voice_t *v = &voices[i];
    if (!v->on) continue;
    v->phase += v->step;
    const float level = v->amp * v->env * chan_volume[v->chan];
    mix += v->phase & 0x80000000u ? level : -level;
    if (tick_ms && v->env > HELD_FLOOR) v->env -= v->env * HELD_FADE;
  }
  for (int i = 0; i < DRUM_VOICES; i++) {
    drum_t *d = &drums[i];
    if (d->env < 0.01f) continue;
    if (d->count-- <= 0) {
      d->count = d->hold - 1;
      noise = noise * 1664525u + 1013904223u;
      d->value = noise & 0x80000000u ? 1.f : -1.f;
    }
    mix += d->value * d->amp * d->env * chan_volume[15];
    d->env *= d->fade;
  }
  return mix * MUSIC_GAIN * music_volume / 127.f;
}

static float next_sfx_sample(void) {
  float mix = 0.f;
  for (int i = 0; i < SFX_CHANNELS; i++) {
    sfx_t *s = &sfx[i];
    const byte *data = s->data;
    if (data == NULL) continue;
    const uint32_t tick = s->pos * SFX_RATE / SAMPLE_RATE;
    if ((int)tick >= s->len) { s->data = NULL; continue; }
    s->pos++;
    const int tone = data[tick];
    if (tone == 0) continue;
    s->phase += sfx_steps[tone];
    mix += s->phase & 0x80000000u ? s->amp : -s->amp;
  }
  return mix * SFX_GAIN;
}

// -1 to 1
static float next_sample(void) {
  float mix = (next_music_sample() + next_sfx_sample()) * master;
  return mix > 1.f ? 1.f : mix < -1.f ? -1.f : mix;
}

static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void *sound_thread(void *unused) {
  (void)unused;
  // same setup as Klipper's creator5_beeper, which leaves the channel like this too
  pwm("disable", NULL, NULL, NULL, NULL);
  pwm("config", "freq=50000000", "max_level=300", "active_level=1", "accuracy_priority=freq");
  pwm("set_level", "100", NULL, NULL, NULL);
  pwm("set_prescale", "6", NULL, NULL, NULL);
  pwm("set_wc", "1", "0", NULL, NULL);
  pwm("enable_channels", NULL, NULL, NULL, NULL);

  pwm_fd = open("/dev/jz_pwm", O_RDWR);
  if (pwm_fd < 0) return NULL;
  pwm_index = ioctl(pwm_fd, PWM_REQUEST, pwm_channel);
  if (pwm_index < 0) return NULL;

  // sleeping most of each sample keeps it under the kernel's real-time limit, which
  // otherwise stops the thread for 50 ms every second
  cpu_set_t cpus;
  CPU_ZERO(&cpus);
  CPU_SET(AUDIO_CPU, &cpus);
  pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
  struct sched_param param = {.sched_priority = RT_PRIORITY};
  pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

  const double step = 1.0 / SAMPLE_RATE;
  const int swing = PWM_PERIOD / 2 - PWM_MARGIN;
  double next = now_s();
  while (running) {
    const int high = PWM_PERIOD / 2 + (int)(next_sample() * swing);
    const double wait = next - now_s() - SPIN_NS * 1e-9;
    if (wait > 0) {
      struct timespec ts = {0, (long)(wait * 1e9)};
      nanosleep(&ts, NULL);
    }
    double now;
    while ((now = now_s()) < next) {}
    if (now - next > RESYNC_S) next = now;
    write_sample(high, PWM_PERIOD - high);
    next += step;
  }
  return NULL;
}

static void start_thread(void) {
  if (running) return;
  const char *env = getenv("DOOM_PWM");
  if (env) pwm_cmd = env;
  env = getenv("DOOM_BEEP_LEVEL");
  if (env) { const int l = atoi(env); if (l >= 0 && l <= 100) master = l / 100.f; }
  for (int tone = 1; tone < 256; tone++) {
    sfx_steps[tone] = step_for(BASE_FREQ * pow(2.0, (tone - 1) / 24.0));
  }
  all_notes_off();
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
  // the launcher silences the buzzer: the thread may be mid-sample
  running = 0;
}

static int Sfx_GetLumpNum(sfxinfo_t *sfx_info) {
  char name[16];
  snprintf(name, sizeof(name), "dp%s", sfx_info->link ? sfx_info->link->name : sfx_info->name);
  return W_CheckNumForName(name);
}

static void Sfx_Update(void) {}

static void Sfx_UpdateParams(int channel, int vol, int sep) {
  (void)sep;
  if (channel >= 0 && channel < SFX_CHANNELS) sfx[channel].amp = vol / 127.f;
}

static int Sfx_Start(sfxinfo_t *sfx_info, int channel, int vol, int sep) {
  (void)sep;
  if (sfx_info->lumpnum < 0 || channel < 0 || channel >= SFX_CHANNELS) return channel;
  const byte *lump = W_CacheLumpNum(sfx_info->lumpnum, PU_STATIC);
  const int size = W_LumpLength(sfx_info->lumpnum);
  if (size < 4) return channel;
  int count = lump[2] | lump[3] << 8;
  if (count > size - 4) count = size - 4;
  sfx_t *s = &sfx[channel];
  s->data = NULL;
  usleep(100);  // the thread reads data once per sample
  s->len = count;
  s->amp = vol / 127.f;
  s->pos = 0;
  s->data = lump + 4;
  return channel;
}

static void Sfx_Stop(int channel) {
  if (channel >= 0 && channel < SFX_CHANNELS) sfx[channel].data = NULL;
}

static boolean Sfx_IsPlaying(int channel) {
  return channel >= 0 && channel < SFX_CHANNELS && sfx[channel].data != NULL ? true : false;
}

static void Sfx_Cache(sfxinfo_t *sounds, int num) { (void)sounds; (void)num; }

static snddevice_t sfx_devices[] = {SNDDEVICE_SB, SNDDEVICE_PAS, SNDDEVICE_GUS,
    SNDDEVICE_WAVEBLASTER, SNDDEVICE_SOUNDCANVAS, SNDDEVICE_AWE32};

sound_module_t DG_sound_module = {
  sfx_devices, sizeof(sfx_devices) / sizeof(sfx_devices[0]),
  Sfx_Init, Sfx_Shutdown, Sfx_GetLumpNum, Sfx_Update, Sfx_UpdateParams,
  Sfx_Start, Sfx_Stop, Sfx_IsPlaying, Sfx_Cache,
};

// ---- music: every format becomes a list of timed events. Channel 15 is drums.

typedef struct { uint32_t tick; int order; byte kind, chan, note, value; uint32_t tempo; } event_t;
typedef struct { event_t *items; int count, cap; } events_t;

static void add_event(events_t *e, uint32_t tick, byte kind, byte chan, byte note, byte value, uint32_t tempo) {
  if (e->count == e->cap) {
    e->cap = e->cap ? e->cap * 2 : 1024;
    e->items = realloc(e->items, (size_t)e->cap * sizeof(event_t));
  }
  event_t ev = {tick, e->count, kind, chan, note, value, tempo};
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

// fixed_ms_per_tick converts ticks without ppq; with ppq, tempo events do
static song_t *build_song(events_t *e, double fixed_ms_per_tick, int ppq) {
  qsort(e->items, (size_t)e->count, sizeof(event_t), compare_events);
  song_t *song = calloc(1, sizeof(song_t));
  song->events = malloc((size_t)e->count * sizeof(song_event_t));
  double ms = 0, ms_per_tick = ppq ? 500000.0 / ppq / 1000.0 : fixed_ms_per_tick;
  uint32_t last_tick = 0;
  for (int i = 0; i < e->count; i++) {
    const event_t *ev = &e->items[i];
    ms += (ev->tick - last_tick) * ms_per_tick;
    last_tick = ev->tick;
    if (ev->kind == EV_TEMPO) {
      if (ppq) ms_per_tick = ev->tempo / (double)ppq / 1000.0;
      continue;
    }
    song_event_t out = {(uint32_t)ms, ev->kind, ev->chan, ev->note, ev->value};
    song->events[song->count++] = out;
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
      const byte kind = status & 0xf0;
      // MIDI drums are on 9, and whatever is on 15 stays pitched
      const byte raw = status & 0x0f;
      const byte chan = raw == 9 ? 15 : raw == 15 ? 9 : raw;
      if (status == 0xff) {
        if (p >= end) break;
        const byte meta = *p++;
        const uint32_t l = read_varlen(&p, end);
        if (l > (uint32_t)(end - p)) break;
        if (meta == 0x51 && l == 3) add_event(&e, tick, EV_TEMPO, 0, 0, 0, (uint32_t)p[0] << 16 | p[1] << 8 | p[2]);
        p += l;
      } else if (status == 0xf0 || status == 0xf7) {
        const uint32_t l = read_varlen(&p, end);
        if (l > (uint32_t)(end - p)) break;
        p += l;
      } else if (kind == 0xc0 || kind == 0xd0) {
        p += 1;
      } else {
        if (p + 2 > end) break;
        const byte a = p[0], b = p[1];
        p += 2;
        if (kind == 0x90 && b) add_event(&e, tick, EV_NOTE_ON, chan, a, b, 0);
        else if (kind == 0x80 || kind == 0x90) add_event(&e, tick, EV_NOTE_OFF, chan, a, 0, 0);
        else if (kind == 0xb0 && a == 7) add_event(&e, tick, EV_VOLUME, chan, 0, b, 0);
        else if (kind == 0xb0 && (a == 120 || a == 123)) add_event(&e, tick, EV_ALL_OFF, chan, 0, 0, 0);
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
  byte velocity[CHANNELS];
  memset(velocity, 100, sizeof(velocity));
  events_t e = {0};
  uint32_t tick = 0;
  int finished = 0;
  while (p < end && !finished) {
    const byte head = *p++;
    const int type = head >> 4 & 7, chan = head & 15;
    switch (type) {
      case 0:
        if (p < end) add_event(&e, tick, EV_NOTE_OFF, chan, *p++ & 127, 0, 0);
        break;
      case 1:
        if (p < end) {
          const byte note = *p++;
          if (note & 0x80 && p < end) velocity[chan] = *p++ & 127;
          add_event(&e, tick, EV_NOTE_ON, chan, note & 127, velocity[chan], 0);
        }
        break;
      case 2: p++; break;
      case 3:  // system: 10 is all sounds off, 11 all notes off
        if (p < end) {
          const byte sys = *p++;
          if (sys == 10 || sys == 11) add_event(&e, tick, EV_ALL_OFF, chan, 0, 0, 0);
        }
        break;
      case 4:
        if (p + 2 <= end) {
          if (p[0] == 3) add_event(&e, tick, EV_VOLUME, chan, 0, p[1] & 127, 0);
        }
        p += 2;
        break;
      case 6: finished = 1; break;
      default: break;  // 5 marks the end of a measure and has no data
    }
    if (head & 0x80 && !finished) tick += read_varlen(&p, end);
  }
  // one last event so the song's length includes its tail
  add_event(&e, tick, EV_ALL_OFF, 0, 0, 0, 0);
  return build_song(&e, 1000.0 / SFX_RATE, 0);
}

static boolean Mus_Init(void) { start_thread(); return running ? true : false; }
static void Mus_Shutdown(void) { music = NULL; }
static void Mus_SetVolume(int volume) { music_volume = volume; }
static void Mus_Pause(void) { music_paused = 1; }
static void Mus_Resume(void) { music_paused = 0; }

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
  free(song->events);
  free(song);
}

static void Mus_Play(void *handle, boolean looping) {
  music = NULL;
  music_loop = looping;
  music_paused = 0;
  music_generation++;
  music = handle;
}

static void Mus_Stop(void) { music = NULL; }

static boolean Mus_IsPlaying(void) {
  return music != NULL && !music_done ? true : false;
}

static void Mus_Poll(void) {}

static snddevice_t music_devices[] = {SNDDEVICE_GENMIDI};

music_module_t DG_music_module = {
  music_devices, 1,
  Mus_Init, Mus_Shutdown, Mus_SetVolume, Mus_Pause, Mus_Resume,
  Mus_Register, Mus_Unregister, Mus_Play, Mus_Stop, Mus_IsPlaying, Mus_Poll,
};
