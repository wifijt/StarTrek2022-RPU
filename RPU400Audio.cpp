// RPU400Audio: the RPU Rev 400 WAV player (RP2350, SD card, I2S DAC).
// Copyright (c) 2026 John Turner (wifijt)
// SPDX-License-Identifier: MIT (the full text is in LICENSE-RPU400Audio.txt)
#include "RPU_Config.h"
#include "RPU400Audio.h"

#if ((RPU_OS_HARDWARE_REV==400) || (RPU_OS_HARDWARE_REV==401) || (RPU_OS_HARDWARE_REV==402)) && defined(ARDUINO_ARCH_RP2040) && !defined(PINNODE_HOST_BUILD)
#include <SPI.h>
#include <SdFat.h>
#include <I2S.h>
#include <pico/util/queue.h>
#include <algorithm>
#include <stdarg.h>
#include <vector>

namespace {
const int SD_MISO = 40, SD_CS = 41, SD_SCK = 42, SD_MOSI = 43;
const int I2S_BCK = 44, I2S_DIN = 46;   // WSEL is BCK + 1
const uint32_t OUT_RATE = 44100;
// The WAV Trigger plays every file at 44.1 kHz whatever its header says, and games' sounds were tuned by
// ear on it (a 48 kHz file plays a little slow, a 22 kHz one twice as fast). Same here, unless a game
// defines RPU400_AUDIO_USE_FILE_RATE to play each file at its own rate.
// A file op's reply data (LIST, READ): the link's bulk frame, from the PinNode library when it's there
#if __has_include(<PinNodeBulk.h>)
#include <PinNodeBulk.h>
#include "hardware/structs/qmi.h"
const unsigned FILE_CHUNK = BULK_DATA;
#else
const unsigned FILE_CHUNK = 4096;
#endif
const int VOICES = 14;                   // as the WAV Trigger
const int BUF_FRAMES = 4096;             // per voice, stereo: 93 ms
const int READ_FRAMES = 1024;
const int BLOCK = 64;                    // frames mixed at a time

// WAV Trigger protocol
const uint8_t SOM1 = 0xF0, SOM2 = 0xAA, EOM = 0x55;
enum { CMD_GET_VERSION = 1, CMD_GET_SYS_INFO = 2, CMD_TRACK_CONTROL = 3, CMD_STOP_ALL = 4, CMD_MASTER_VOLUME = 5,
       CMD_TRACK_VOLUME = 8, CMD_TRACK_FADE = 10, CMD_RESUME_ALL_SYNC = 11, CMD_TRACK_CONTROL_EX = 13,
       CMD_SET_REPORTING = 14,
       CMD_START_VOICE = 0x40,           // ours, core 0 -> core 1: code, track (2), lock, voice, generation
       CMD_PIP = 0x41 };                 // ours: a short tone at gain_db (i8), for volume changes
enum { TRK_PLAY_SOLO, TRK_PLAY_POLY, TRK_PAUSE, TRK_RESUME, TRK_STOP, TRK_LOOP_ON, TRK_LOOP_OFF, TRK_LOAD };
enum { RSP_VERSION_STRING = 129, RSP_SYSTEM_INFO = 130, RSP_TRACK_REPORT = 132 };

// Splits a byte stream into the WAV Trigger's packets: SOM1 SOM2 length(whole packet) command data... EOM
struct Framer {
  uint8_t pkt[32];
  int got = 0, len = 0;
  bool take(uint8_t b) {                 // true: pkt holds a whole packet, len long
    if (got == 0) { if (b == SOM1) pkt[got++] = b; return false; }
    if (got == 1) { if (b == SOM2) pkt[got++] = b; else got = 0; return false; }
    if (got == 2) {
      if (b < 5 || b > sizeof pkt) { got = 0; return false; }
      len = b; pkt[got++] = b; return false;
    }
    pkt[got++] = b;
    if (got < len) return false;
    got = 0;
    return b == EOM;
  }
};

queue_t toPlayer;                        // core 0 -> 1: packets
queue_t ended;                           // core 1 -> 0: a voice finished (voice << 8 | generation)
volatile bool started = false;
volatile bool indexReady = false;        // core 1 has listed the card: present[] is good
uint8_t present[4096 / 8];               // the card's tracks
volatile uint16_t lastTrack = 0;
volatile uint32_t dropsIn = 0;

// Files on the card (PinNode 400 bulk mode, the owner's own sounds over the LAN): the link on core 0
// hands each op to core 1, which owns the card, and waits for it
struct FileJob {
  volatile uint8_t state;                // 0 idle, 1 for core 1, 2 done
  uint8_t op;
  char name[48];
  uint32_t a0, a1;
  const uint8_t *data;
  unsigned len;
  uint8_t *out;
  unsigned outLen;
  uint32_t r0, r1;
  int status;
};
FileJob job;
volatile bool fileSession = false;       // sounds paused while files change
volatile bool silenced = false;          // the node is about to halt the RP (an update): no more sound

// ---------------------------------------------------------------- core 0: the game's side
// Core 0 hands out the voices and answers the game's questions itself, so "is it playing?" is right the
// moment the game has asked for a track. Asked over to core 1, the game's loop sees "not playing" before
// core 1 has started it, and starts it again, thousands of times (the WAV Trigger has the same race; the
// Mega's loop is slow enough to hide it).
struct Slot { bool active, locked; uint16_t track; uint8_t gen; uint32_t order; };
// The app's preview (PinNode400_Preview): a file from the card by its path, on a voice of its own,
// never reported to the game
const uint16_t PREVIEW_TRACK = 0xFFFF;
char previewPath[48];                    // set by core 0 before it asks core 1 to start the voice
volatile bool previewFailed = false;     // core 1: the preview's file didn't open
volatile bool previewOpened = false;     // core 1: it opened and plays (reported once)
volatile int previewEnd = 0;             // core 0: PINNODE_PREVIEW_ENDED / _NO_FILE once it's over
Slot slot[VOICES];
uint32_t slotOrder = 0;
bool reporting = false;
uint8_t outBuf[512];                     // replies for the game to read
int outHead = 0, outCount = 0;
Framer fromGame;

void reply(const uint8_t *msg, uint8_t n) {           // msg = code + data
  if (outCount + n + 4 > (int)sizeof outBuf) return;
  uint8_t pkt[32] = {SOM1, SOM2, (uint8_t)(n + 4)};
  memcpy(pkt + 3, msg, n);
  pkt[n + 3] = EOM;
  for (int i = 0; i < n + 4; i++) outBuf[(outHead + outCount++) % sizeof outBuf] = pkt[i];
}

void report(int v, bool on) {
  if (!reporting || slot[v].track == PREVIEW_TRACK) return;   // the app's preview isn't the game's
  uint16_t t = slot[v].track - 1;                     // the WAV Trigger reports tracks from 0
  uint8_t m[5] = {RSP_TRACK_REPORT, (uint8_t)t, (uint8_t)(t >> 8), (uint8_t)v, (uint8_t)on};
  reply(m, 5);
}

void freeSlot(int v) {
  if (!slot[v].active) return;
  report(v, false);
  slot[v].active = false;
  if (slot[v].track == PREVIEW_TRACK) previewEnd = previewFailed ? 6 : 5;   // NO_FILE : ENDED
}

void forward(const uint8_t *pkt, int n) {
  if (queue_get_level(&toPlayer) + n > 1024) { dropsIn++; return; }   // core 1 stuck: whole packets only
  for (int i = 0; i < n; i++) queue_try_add(&toPlayer, &pkt[i]);
}

void takeEnded() {
  uint16_t e;
  while (queue_try_remove(&ended, &e)) {
    int v = e >> 8;
    if (v < VOICES && slot[v].active && slot[v].gen == (uint8_t)e) freeSlot(v);
  }
}

void startTrack(uint8_t code, uint16_t track, bool lock) {
  if (track != PREVIEW_TRACK && indexReady && (track >= 4096 || !(present[track / 8] & (1 << (track % 8))))) return;   // not on the card
  if (code == TRK_PLAY_SOLO) for (int i = 0; i < VOICES; i++) freeSlot(i);
  int v = -1;
  for (int i = 0; i < VOICES && v < 0; i++) if (!slot[i].active) v = i;
  if (v < 0) {                                        // all busy: the oldest unlocked one goes
    for (int i = 0; i < VOICES; i++)
      if (!slot[i].locked && (v < 0 || slot[i].order < slot[v].order)) v = i;
    if (v < 0) return;
    freeSlot(v);
  }
  Slot &s = slot[v];
  s.active = true; s.locked = lock; s.track = track; s.gen++; s.order = slotOrder++;
  report(v, true);
  uint8_t pkt[11] = {SOM1, SOM2, 11, CMD_START_VOICE, code, (uint8_t)track, (uint8_t)(track >> 8), lock, (uint8_t)v,
                     s.gen, EOM};
  forward(pkt, 11);
}

void fromGamePacket(const uint8_t *pkt, int len) {
  const uint8_t *m = pkt + 3;                         // command, then its data
  int n = len - 4;
  switch (m[0]) {
    case CMD_GET_VERSION: {
      uint8_t r[21] = {RSP_VERSION_STRING};
      memcpy(r + 1, "RPU 400 audio 1.0   ", 20);
      reply(r, 21);
      return;
    }
    case CMD_GET_SYS_INFO: {
      uint16_t nt = lastTrack;
      uint8_t r[4] = {RSP_SYSTEM_INFO, VOICES, (uint8_t)nt, (uint8_t)(nt >> 8)};
      reply(r, 4);
      return;
    }
    case CMD_SET_REPORTING: if (n >= 2) reporting = m[1]; return;
    case CMD_TRACK_CONTROL:
    case CMD_TRACK_CONTROL_EX: {
      if (n < 4) return;
      uint8_t code = m[1];
      uint16_t track = m[2] | m[3] << 8;
      if (code == TRK_PLAY_SOLO || code == TRK_PLAY_POLY || code == TRK_LOAD) {
        startTrack(code, track, m[0] == CMD_TRACK_CONTROL_EX && n >= 5 && m[4]);
        return;
      }
      if (code == TRK_STOP) for (int i = 0; i < VOICES; i++) if (slot[i].active && slot[i].track == track) freeSlot(i);
      break;
    }
    case CMD_STOP_ALL: for (int i = 0; i < VOICES; i++) freeSlot(i); break;
  }
  forward(pkt, len);
}

// ---------------------------------------------------------------- core 1: the player
// opened by its place in its folder (the root, or this slot's folder): no name search
struct TrackFile { uint16_t track; uint32_t dirIndex; bool inSlot; };
std::vector<TrackFile> tracks;           // sorted by track

struct Voice {
  bool active, paused, loop, eof, stopAtFadeEnd;
  uint16_t track;
  uint8_t gen;                           // core 0's number for this start
  FsFile f;
  uint32_t dataStart, dataLen, dataLeft;
  uint16_t channels;
  uint32_t step, frac;                   // source frames per output frame, Q16
  int16_t buf[BUF_FRAMES * 2];
  uint16_t head, count;
  float gainDb, fadeStepDb, fadeTargetDb;
  int fadeBlocks;
  int32_t gainQ14;                       // the level it should be at
  int32_t curQ14;                        // the level it's at: eased to gainQ14 across a block, so no clicks
};
Voice voice[VOICES];
I2S i2s(OUTPUT);
SdFs sd;
FsFile root;
bool cardOk = false;
uint32_t cardMHz = 0;
// the whole output's level: eased to 0 and back around a flash write (PinNode400_AudioHold), so the
// pause it causes on this core doesn't pop
volatile int32_t masterTarget = 16384;
int32_t masterQ14 = 16384;
volatile uint32_t silentBlocks = 0;      // blocks sent at level 0 since the hold began
volatile bool ringSilence = false, ringSilent = false;   // core 0 asks core 1 to zero the whole ring

// a status line for the node log (PinNode400_AudioStatus): core 1 writes it, core 0 hands it over
char statusLine[60];
volatile uint32_t statusSeq = 0;
uint32_t statusSent = 0;
void setStatus(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(statusLine, sizeof statusLine, fmt, ap);
  va_end(ap);
  __dmb();
  statusSeq++;
}

// the volume pip: a 1 kHz tone, 80 ms, with 5 ms ramps so it doesn't click
const int PIP_FRAMES = OUT_RATE * 80 / 1000, PIP_RAMP = OUT_RATE * 5 / 1000;
int pipLeft = 0;
int32_t pipAmp = 0;
uint32_t pipPhase = 0;

// what the player has been through, on USB serial every few seconds when it changes
struct { uint32_t plays, missing, badFile, underruns, readErrs, maxOpenUs, maxReadUs, maxVoices; } st;

void voiceEnded(int v, uint8_t gen) {
  uint16_t e = v << 8 | gen;
  queue_try_add(&ended, &e);
}

int32_t dbToQ14(float db) {
  if (db <= -70) return 0;
  if (db > 10) db = 10;
  return (int32_t)(powf(10.0f, db / 20.0f) * 16384.0f);
}

// PinNode game slots (owner, 2026-11-20): a game launched from slot N plays its sounds from the card's folder
// "slotN" (any case), each track that isn't there from the root as before. The starter maps the slot at the
// start of flash with QMI ATRANS0, whose base says which slot (pinnode-rpboot slots.h: games 1-6 at 0x300000 +
// (N - 1) MB). A board without slots has ATRANS0 at its reset value (base 0): the root only.
FsFile slotDir;
int launchedSlot() {
  uint32_t base = (qmi_hw->atrans[0] & 0xFFFu) << 12;
  return base >= 0x300000 && base < 0x900000 ? 1 + (int)((base - 0x300000) >> 20) : 0;
}
void openSlotDir() {
  slotDir.close();
  int s = launchedSlot();
  if (!s) return;
  char name[8];
  snprintf(name, sizeof name, "slot%d", s);
  if (!slotDir.open(&root, name, O_RDONLY) || !slotDir.isDir()) slotDir.close();
}

void indexFolder(FsFile &dir, bool inSlot) {
  dir.rewind();
  FsFile f;
  char n[64];
  while (f.openNext(&dir, O_RDONLY)) {
    f.getName(n, sizeof n);
    size_t len = strlen(n);
    if (!f.isDir() && isdigit(n[0]) && len > 4 && !strcasecmp(n + len - 4, ".wav"))
      tracks.push_back({(uint16_t)atoi(n), (uint32_t)f.dirIndex(), inSlot});
    f.close();
  }
}

void indexCard() {
  tracks.clear();
  openSlotDir();
  if (slotDir) indexFolder(slotDir, true);
  indexFolder(root, false);
  // the slot's own track first, so the root's copy of the same number is dropped
  std::stable_sort(tracks.begin(), tracks.end(), [](const TrackFile &a, const TrackFile &b) {
    return a.track < b.track || (a.track == b.track && a.inSlot && !b.inSlot);
  });
  tracks.erase(std::unique(tracks.begin(), tracks.end(),
                           [](const TrackFile &a, const TrackFile &b) { return a.track == b.track; }),
               tracks.end());
}

// Names the node can't reach: the link carries a path of at most 47 characters (BULK_NAME_MAX), so a file or folder
// copied onto the card by a computer with a longer path can't be moved, deleted or played from the app or the
// board's page (owner, 2026-11-20: Star Trek 2025's sounds). At start-up the player shortens them where they are:
// the end of the name is cut, ".wav" and the track number in front stay. A name already taken is left alone.
const size_t LINK_PATH_MAX = 47;
void shortenLongNames(FsFile &dir, size_t dirLen, int depth) {
  if (depth > 4) return;
  FsFile f;
  char n[256], to[64];
  bool renamed = true;
  while (renamed) {                      // a rename moves entries: start the folder again until nothing changes
    renamed = false;
    dir.rewind();
    while (!renamed && f.openNext(&dir, O_RDONLY)) {
      f.getName(n, sizeof n);
      size_t len = strlen(n), full = dirLen + len;
      bool isDir = f.isDir();
      if (n[0] != '.' && full > LINK_PATH_MAX && dirLen + 9 <= LINK_PATH_MAX) {
        bool wav = !isDir && len > 4 && !strcasecmp(n + len - 4, ".wav");
        size_t keep = LINK_PATH_MAX - dirLen - (wav ? 4 : 0);
        if (isDir) keep -= 8;            // room for a name inside it
        snprintf(to, sizeof to, "%.*s%s", (int)keep, n, wav ? ".wav" : "");
        f.close();
        if (!dir.exists(to)) {
          FsFile g;
          if (g.open(&dir, n, O_RDONLY) && g.rename(&dir, to)) { renamed = true; setStatus("audio: renamed %s", to); }
          g.close();
        }
        continue;
      }
      if (isDir && n[0] != '.') {
        FsFile sub;
        f.close();
        if (sub.open(&dir, n, O_RDONLY)) { shortenLongNames(sub, dirLen + len + 1, depth + 1); sub.close(); }
        continue;
      }
      f.close();
    }
  }
}

const TrackFile *findTrack(uint16_t t) {
  auto it = std::lower_bound(tracks.begin(), tracks.end(), t, [](const TrackFile &a, uint16_t t) { return a.track < t; });
  return it != tracks.end() && it->track == t ? &*it : nullptr;
}

// A stopped sound fades to nothing over one block instead of being cut (a cut mid-waveform pops):
// its last block goes into `tail`, which the next mixBlock adds in
int32_t tail[BLOCK * 2];
void renderTail(Voice &vc) {
  for (int i = 0; i < BLOCK && vc.count >= 2; i++) {
    int32_t g = vc.curQ14 * (BLOCK - i) / BLOCK;
    int a = vc.head * 2, b = ((vc.head + 1) % BUF_FRAMES) * 2;
    int32_t l = vc.buf[a] + (((vc.buf[b] - vc.buf[a]) * (int32_t)(vc.frac >> 1)) >> 15);
    int32_t r = vc.buf[a + 1] + (((vc.buf[b + 1] - vc.buf[a + 1]) * (int32_t)(vc.frac >> 1)) >> 15);
    tail[i * 2] += (l * g) >> 14;
    tail[i * 2 + 1] += (r * g) >> 14;
    vc.frac += vc.step;
    while (vc.frac >= 65536 && vc.count) { vc.frac -= 65536; vc.head = (vc.head + 1) % BUF_FRAMES; vc.count--; }
  }
}

void stopVoice(int v) {
  if (!voice[v].active) return;
  if (!voice[v].paused) renderTail(voice[v]);
  voice[v].f.close();
  voice[v].active = false;
  voiceEnded(v, voice[v].gen);
}

bool openWav(Voice &vc, const TrackFile *t) {           // a track, or (null) the preview's path
  if (t ? !vc.f.open(t->inSlot ? &slotDir : &root, t->dirIndex, O_RDONLY) : !vc.f.open(previewPath, O_RDONLY)) return false;
  uint8_t h[12];
  if (vc.f.read(h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return false;
  uint32_t rate = 0;
  uint16_t bits = 0;
  vc.channels = 0;
  while (true) {
    uint8_t c[8];
    if (vc.f.read(c, 8) != 8) return false;
    uint32_t len = c[4] | c[5] << 8 | c[6] << 16 | (uint32_t)c[7] << 24;
    if (!memcmp(c, "fmt ", 4)) {
      uint8_t fm[16];
      if (len < 16 || vc.f.read(fm, 16) != 16) return false;
      vc.channels = fm[2] | fm[3] << 8;
      rate = fm[4] | fm[5] << 8 | fm[6] << 16 | (uint32_t)fm[7] << 24;
      bits = fm[14] | fm[15] << 8;
      vc.f.seekSet(vc.f.curPosition() + len - 16 + (len & 1));
    } else if (!memcmp(c, "data", 4)) {
      vc.dataStart = vc.f.curPosition();
      vc.dataLen = vc.dataLeft = len;
      break;
    } else {
      vc.f.seekSet(vc.f.curPosition() + len + (len & 1));
    }
  }
  if (bits != 16 || vc.channels < 1 || vc.channels > 2 || !rate) return false;
#ifdef RPU400_AUDIO_USE_FILE_RATE
  vc.step = (uint32_t)(((uint64_t)rate << 16) / OUT_RATE);
#else
  vc.step = 65536;
#endif
  return true;
}

// Reads the next chunk of the file into the voice's ring, as stereo frames
void refill(Voice &vc) {
  static int16_t raw[READ_FRAMES * 2];
  if (vc.eof || BUF_FRAMES - vc.count < READ_FRAMES) return;
  if (!vc.dataLeft) {
    if (!vc.loop) { vc.eof = true; return; }
    vc.f.seekSet(vc.dataStart);
    vc.dataLeft = vc.dataLen;
  }
  uint32_t want = READ_FRAMES * 2 * vc.channels;
  if (want > vc.dataLeft) want = vc.dataLeft;
  uint32_t t0 = micros();
  int got = vc.f.read((uint8_t *)raw, want);
  t0 = micros() - t0;
  if (t0 > st.maxReadUs) st.maxReadUs = t0;
  if (got <= 0) { st.readErrs++; vc.eof = true; return; }
  vc.dataLeft -= got;
  int frames = got / (2 * vc.channels);
  uint16_t tail = (vc.head + vc.count) % BUF_FRAMES;
  for (int i = 0; i < frames; i++) {
    vc.buf[tail * 2] = raw[i * vc.channels];
    vc.buf[tail * 2 + 1] = raw[i * vc.channels + vc.channels - 1];
    tail = (tail + 1) % BUF_FRAMES;
  }
  vc.count += frames;
}

// On the voice core 0 chose. A start that fails still reports the voice ended, so core 0 frees it.
void startVoice(int v, uint8_t gen, uint16_t track, bool paused) {
  if (v >= VOICES) return;
  stopVoice(v);
  Voice &vc = voice[v];
  vc.gen = gen;
  bool preview = track == PREVIEW_TRACK;
  if (preview) previewFailed = false;
  const TrackFile *t = preview ? nullptr : findTrack(track);
  if (!preview && !t) { st.missing++; voiceEnded(v, gen); return; }
  uint32_t t0 = micros();
  bool ok = openWav(vc, t);
  t0 = micros() - t0;
  if (t0 > st.maxOpenUs) st.maxOpenUs = t0;
  if (!ok) { st.badFile++; vc.f.close(); if (preview) previewFailed = true; voiceEnded(v, gen); return; }
  if (preview) previewOpened = true;
  st.plays++;
  vc.active = true; vc.paused = paused; vc.loop = false; vc.eof = false; vc.stopAtFadeEnd = false;
  vc.track = track;
  vc.frac = 0; vc.head = 0; vc.count = 0;
  vc.gainDb = 0; vc.fadeBlocks = 0; vc.gainQ14 = dbToQ14(0);
  vc.curQ14 = 0;                         // eases in over the first block
  refill(vc);
}

void trackControl(uint8_t code, uint16_t track) {
  for (int i = 0; i < VOICES; i++) {
    Voice &vc = voice[i];
    if (!vc.active || vc.track != track) continue;
    if (code == TRK_PAUSE) vc.paused = true;
    else if (code == TRK_RESUME) vc.paused = false;
    else if (code == TRK_STOP) stopVoice(i);
    else if (code == TRK_LOOP_ON) vc.loop = true;
    else if (code == TRK_LOOP_OFF) vc.loop = false;
  }
}

void command(const uint8_t *m, int n) {               // m[0] = command, then its data
  auto u16 = [&](int i) { return (uint16_t)(m[i] | m[i + 1] << 8); };
  switch (m[0]) {
    case CMD_START_VOICE:
      if (fileSession || silenced) { voiceEnded(m[5], m[6]); break; }   // files changing, or an update: no sounds
      if (n >= 7) {
        if (m[1] == TRK_PLAY_SOLO) for (int i = 0; i < VOICES; i++) stopVoice(i);
        startVoice(m[5], m[6], u16(2), m[1] == TRK_LOAD);
      }
      break;
    case CMD_TRACK_CONTROL:
    case CMD_TRACK_CONTROL_EX: if (n >= 4) trackControl(m[1], u16(2)); break;
    case CMD_STOP_ALL: for (int i = 0; i < VOICES; i++) stopVoice(i); break;
    case CMD_TRACK_VOLUME:
      if (n >= 5) for (auto &vc : voice) if (vc.active && vc.track == u16(1)) {
        vc.gainDb = (int16_t)u16(3); vc.fadeBlocks = 0; vc.gainQ14 = dbToQ14(vc.gainDb);
      }
      break;
    case CMD_TRACK_FADE:
      if (n >= 8) for (auto &vc : voice) if (vc.active && vc.track == u16(1)) {
        float target = (int16_t)u16(3);
        int blocks = (int)((uint32_t)u16(5) * OUT_RATE / 1000 / BLOCK);
        if (blocks < 1) blocks = 1;
        vc.fadeTargetDb = target;
        vc.fadeStepDb = (target - vc.gainDb) / blocks;
        vc.fadeBlocks = blocks;
        vc.stopAtFadeEnd = m[7];
      }
      break;
    case CMD_RESUME_ALL_SYNC: for (auto &vc : voice) if (vc.active) vc.paused = false; break;
    case CMD_PIP:
      if (n >= 2) { pipAmp = (int32_t)(24000L * dbToQ14((int8_t)m[1]) >> 14); pipLeft = PIP_FRAMES; pipPhase = 0; }
      break;
    default: break;          // master volume (RPU never sends it), amp power, sample-rate offset, trigger bank
  }
}

// ---- file ops (core 1). Paths are relative to the card's root ("" = the root); the root's files play (as on
// the WAV Trigger), and a game slot's folder "slotN" ahead of them (indexCard); other folders are storage
// (owner, 2026-10-19 and 2026-11-20).
enum { F_OK = 0, F_NO_CARD = 2, F_BAD_NAME = 3, F_IO = 4, F_NO_FILE = 5, F_OFFSET = 6, F_FULL = 7, F_OP = 9,
       F_EXISTS = 10, F_NOT_EMPTY = 11 };
enum { OP_NOP = 1, OP_INFO, OP_LIST, OP_OPEN, OP_WRITE, OP_CLOSE, OP_DELETE, OP_END,
       OP_MKDIR = 10, OP_MOVE, OP_COPY, OP_COPY_STEP, OP_READ };
const char *TMP_NAME = ".upload.tmp", *COPY_TMP = ".copy.tmp";
FsFile upload, copySrc, copyDst, readFile;
uint32_t uploaded = 0, copyDone = 0, copyTotal = 0;
char copyTo[48], readName[48];
uint8_t copyBuf[8192];

bool trackName(const char *n) {            // 001_name.wav: digits first, .wav last, nothing odd between
  size_t len = strlen(n);
  if (len < 6 || len > 47 || !isdigit((unsigned char)n[0]) || strcasecmp(n + len - 4, ".wav")) return false;
  for (size_t i = 0; i < len; i++)
    if (!(isalnum((unsigned char)n[i]) || n[i] == '_' || n[i] == '-' || n[i] == '.' || n[i] == ' ')) return false;
  return true;
}

// a path on the card: segments joined by '/', none empty, none starting with '.', no leading or trailing
// '/'; printable characters except \ : * ? " < > | (FAT's own rule). "" (the root) only where allowEmpty.
bool pathOk(const char *p, bool allowEmpty) {
  size_t len = strlen(p);
  if (!len) return allowEmpty;
  if (len > 47 || p[0] == '/' || p[len - 1] == '/') return false;
  bool segStart = true;
  for (size_t i = 0; i < len; i++) {
    char c = p[i];
    if (c == '/') { if (segStart) return false; segStart = true; continue; }
    if (segStart && c == '.') return false;              // no "..", no hidden files
    segStart = false;
    if (c < 32 || c > 126 || strchr("\\:*?\"<>|", c)) return false;
  }
  return true;
}

bool wavPath(const char *p) { size_t n = strlen(p); return pathOk(p, false) && n > 4 && !strcasecmp(p + n - 4, ".wav"); }

void reindex() {
  root.close();
  root.open("/");
  indexCard();
  memset(present, 0, sizeof present);
  for (auto &t : tracks) if (t.track < 4096) present[t.track / 8] |= 1 << (t.track % 8);
  lastTrack = tracks.empty() ? 0 : tracks.back().track;
}

void closeAll() {
  if (upload) { upload.close(); sd.remove(TMP_NAME); }
  if (copyDst) { copyDst.close(); sd.remove(COPY_TMP); }
  if (copySrc) copySrc.close();
  if (readFile) readFile.close();
}

// File sessions run the card at 25 MHz, the SD spec's limit in SPI mode: playback's 50 MHz is fine for
// sound (a rare bad read is a click) but not for files, where it gave stale and failed reads (2026-10-19).
const uint32_t FILE_MHZ = 25;
bool cardClock(uint32_t mhz) {
  slotDir.close();
  root.close();
  sd.end();
  if (!sd.begin(SdSpiConfig(SD_CS, DEDICATED_SPI, SD_SCK_MHZ(mhz), &SPI1)) || !root.open("/")) return false;
  openSlotDir();                                       // the slot's tracks open through it by their index
  return true;
}

int fileOp(FileJob &j) {
  if (j.op == OP_END) {
    closeAll();
    if (fileSession && cardOk && cardMHz > FILE_MHZ && !cardClock(cardMHz)) cardOk = cardClock(FILE_MHZ);
    fileSession = false;
    return F_OK;
  }
  if (!fileSession) {                      // the first op of a session: stop every sound, slow the card
    fileSession = true;
    for (int i = 0; i < VOICES; i++) stopVoice(i);
    if (cardOk && cardMHz > FILE_MHZ) cardOk = cardClock(FILE_MHZ);
  }
  if (j.op == OP_NOP) return F_OK;
  if (!cardOk) return F_NO_CARD;
  if (j.op != OP_READ && readFile) readFile.close();     // nothing changes under an open download
  char to[48] = "";
  if (j.op == OP_MOVE || j.op == OP_COPY) {             // the destination path comes in the data
    unsigned n = j.len < sizeof to - 1 ? j.len : sizeof to - 1;
    memcpy(to, j.data, n);
    to[n] = 0;
  }
  switch (j.op) {
    case OP_INFO: j.r0 = 1; return F_OK;
    case OP_LIST: {                        // a folder: [size u32][flags u8: 1 folder][n u8][name n] ..., from entry a0
      if (!pathOk(j.name, true)) return F_BAD_NAME;
      FsFile dir;
      if (!(j.name[0] ? dir.open(j.name, O_RDONLY) : dir.open("/", O_RDONLY)) || !dir.isDir()) return F_NO_FILE;
      FsFile f;
      char n[64];
      uint32_t idx = 0, count = 0;
      unsigned pos = 0;
      j.r1 = 0;
      while (f.openNext(&dir, O_RDONLY)) {
        f.getName(n, sizeof n);
        bool isDir = f.isDir();
        uint32_t size = isDir ? 0 : f.fileSize();
        f.close();
        if (n[0] == '.' || idx++ < j.a0) continue;          // hidden (macOS ._ files, our temp files)
        unsigned nl = strlen(n);
        if (pos + 6 + nl > FILE_CHUNK) { j.r1 = 1; break; }
        j.out[pos] = size; j.out[pos + 1] = size >> 8; j.out[pos + 2] = size >> 16; j.out[pos + 3] = size >> 24;
        j.out[pos + 4] = isDir ? 1 : 0;
        j.out[pos + 5] = nl;
        memcpy(j.out + pos + 6, n, nl);
        pos += 6 + nl;
        count++;
      }
      dir.close();
      j.outLen = pos;
      j.r0 = count;
      return F_OK;
    }
    case OP_OPEN:
      if (!wavPath(j.name)) return F_BAD_NAME;
      if (upload) upload.close();
      upload = sd.open(TMP_NAME, O_WRONLY | O_CREAT | O_TRUNC);
      uploaded = 0;
      return upload ? F_OK : F_IO;
    case OP_WRITE:
      if (!upload) return F_IO;
      if (j.a0 != uploaded) return F_OFFSET;
      if (upload.write(j.data, j.len) != j.len) return F_FULL;
      uploaded += j.len;
      return F_OK;
    case OP_CLOSE: {
      if (!upload || !wavPath(j.name)) return F_BAD_NAME;
      bool whole = uploaded == j.a0;
      bool synced = upload.sync();
      upload.close();
      if (!whole || !synced) { sd.remove(TMP_NAME); return whole ? F_IO : F_OFFSET; }
      if (sd.exists(j.name)) sd.remove(j.name);
      if (!sd.rename(TMP_NAME, j.name)) return F_IO;
      reindex();
      return F_OK;
    }
    case OP_DELETE: {                      // a file, or an empty folder
      if (!pathOk(j.name, false)) return F_BAD_NAME;
      FsFile f;
      if (!f.open(j.name, O_RDONLY)) return F_NO_FILE;
      bool isDir = f.isDir();
      f.close();
      if (isDir ? !sd.rmdir(j.name) : !sd.remove(j.name)) return isDir ? F_NOT_EMPTY : F_IO;
      reindex();
      return F_OK;
    }
    case OP_MKDIR:
      if (!pathOk(j.name, false)) return F_BAD_NAME;
      if (sd.exists(j.name)) return F_EXISTS;
      return sd.mkdir(j.name, false) ? F_OK : F_IO;
    case OP_MOVE:                          // a directory change on the card: instant, whatever the size
      if (!pathOk(j.name, false) || !pathOk(to, false)) return F_BAD_NAME;
      if (!sd.exists(j.name)) return F_NO_FILE;
      if (sd.exists(to)) return F_EXISTS;
      if (!sd.rename(j.name, to)) return F_IO;
      reindex();
      return F_OK;
    case OP_COPY: {                        // starts a copy, card to card; OP_COPY_STEP does the work
      if (!wavPath(j.name) || !wavPath(to)) return F_BAD_NAME;
      if (sd.exists(to)) return F_EXISTS;
      if (copySrc) copySrc.close();
      if (copyDst) copyDst.close();
      copySrc = sd.open(j.name, O_RDONLY);
      if (!copySrc) return F_NO_FILE;
      copyDst = sd.open(COPY_TMP, O_WRONLY | O_CREAT | O_TRUNC);
      if (!copyDst) { copySrc.close(); return F_IO; }
      strlcpy(copyTo, to, sizeof copyTo);
      copyDone = 0;
      copyTotal = copySrc.fileSize();
      j.r0 = 0; j.r1 = copyTotal;
      return F_OK;
    }
    case OP_COPY_STEP: {                   // up to 64 KB; r0 = done, r1 = total; done when they're equal
      if (!copySrc || !copyDst) return F_IO;
      for (int k = 0; k < 8 && copyDone < copyTotal; k++) {      // 64 KB a step
        int n = copySrc.read(copyBuf, sizeof copyBuf);
        if (n <= 0) { closeAll(); return F_IO; }
        if (copyDst.write(copyBuf, n) != (size_t)n) { closeAll(); return F_FULL; }
        copyDone += n;
      }
      j.r0 = copyDone; j.r1 = copyTotal;
      if (copyDone < copyTotal) return F_OK;
      bool synced = copyDst.sync();
      copyDst.close();
      copySrc.close();
      if (!synced || !sd.rename(COPY_TMP, copyTo)) { sd.remove(COPY_TMP); return F_IO; }
      reindex();
      return F_OK;
    }
    case OP_READ: {                        // 4 KB from a0: out, r0 = bytes, r1 = the file's size
      if (!pathOk(j.name, false)) return F_BAD_NAME;
      if (!readFile || strcmp(readName, j.name)) {
        if (readFile) readFile.close();
        readFile = sd.open(j.name, O_RDONLY);
        if (!readFile || readFile.isDir()) { if (readFile) readFile.close(); return F_NO_FILE; }
        strlcpy(readName, j.name, sizeof readName);
      }
      j.r1 = readFile.fileSize();
      if (j.a0 > j.r1) return F_OFFSET;
      readFile.seekSet(j.a0);
      int n = readFile.read(j.out, FILE_CHUNK);
      if (n < 0) return F_IO;
      j.outLen = n;
      j.r0 = n;
      return F_OK;
    }
  }
  return F_OP;
}

void takeFileJob() {
  if (job.state != 1) return;
  job.status = fileOp(job);
  __dmb();
  job.state = 2;
}

void takeCommands() {
  static Framer f;
  uint8_t b;
  while (queue_try_remove(&toPlayer, &b))
    if (f.take(b)) command(f.pkt + 3, f.len - 4);
}

void mixBlock() {
  int32_t acc[BLOCK * 2];
  memcpy(acc, tail, sizeof acc);                       // the last block of sounds just stopped
  memset(tail, 0, sizeof tail);
  for (int v = 0; v < VOICES; v++) {
    Voice &vc = voice[v];
    if (!vc.active || vc.paused) continue;
    int32_t g0 = vc.curQ14, dg = vc.gainQ14 - vc.curQ14;
    vc.curQ14 = vc.gainQ14;
    for (int i = 0; i < BLOCK; i++) {
      int32_t g = g0 + dg * i / BLOCK;
      if (vc.count < 2) {
        if (vc.eof) { stopVoice(v); break; }
        st.underruns++;
        break;                                        // the card fell behind: this voice is silent briefly
      }
      int a = vc.head * 2, b = ((vc.head + 1) % BUF_FRAMES) * 2;
      int32_t l = vc.buf[a] + (((vc.buf[b] - vc.buf[a]) * (int32_t)(vc.frac >> 1)) >> 15);
      int32_t r = vc.buf[a + 1] + (((vc.buf[b + 1] - vc.buf[a + 1]) * (int32_t)(vc.frac >> 1)) >> 15);
      acc[i * 2] += (l * g) >> 14;
      acc[i * 2 + 1] += (r * g) >> 14;
      vc.frac += vc.step;
      while (vc.frac >= 65536 && vc.count) { vc.frac -= 65536; vc.head = (vc.head + 1) % BUF_FRAMES; vc.count--; }
    }
    if (vc.active && vc.fadeBlocks > 0) {
      vc.gainDb += vc.fadeStepDb;
      if (--vc.fadeBlocks == 0) {
        vc.gainDb = vc.fadeTargetDb;
        if (vc.stopAtFadeEnd) stopVoice(v);
      }
      vc.gainQ14 = dbToQ14(vc.gainDb);
    }
  }
  for (int i = 0; i < BLOCK && pipLeft > 0; i++, pipLeft--) {          // the pip, over whatever plays
    int done = PIP_FRAMES - pipLeft, ramp = done < PIP_RAMP ? done : pipLeft < PIP_RAMP ? pipLeft : PIP_RAMP;
    int32_t a = pipAmp * ramp / PIP_RAMP;
    pipPhase += (uint32_t)(((uint64_t)1000 << 32) / OUT_RATE);
    int32_t v = (int32_t)(sinf((float)pipPhase * (2.0f * PI / 4294967296.0f)) * a);
    acc[i * 2] += v;
    acc[i * 2 + 1] += v;
  }
  int32_t m0 = masterQ14, dm = masterTarget - masterQ14;
  masterQ14 = masterTarget;
  if (m0 == 0 && dm == 0) silentBlocks++;
  for (int i = 0; i < BLOCK; i++) {
    int32_t m = m0 + dm * i / BLOCK;
    if (m != 16384) { acc[i * 2] = (acc[i * 2] * m) >> 14; acc[i * 2 + 1] = (acc[i * 2 + 1] * m) >> 14; }
    int32_t l = constrain(acc[i * 2], -32768, 32767), r = constrain(acc[i * 2 + 1], -32768, 32767);
    i2s.write16((int16_t)l, (int16_t)r);
  }
}
}  // namespace

// From the RP's first instant until the player starts I2S, the DAC's lines would float, and the PCM5100
// turns noise on its clock into static (heard at every reset and boot). Hold them low: no clock, silent.
static void __attribute__((constructor)) QuietDacAtBoot() {
  for (int pin : {I2S_BCK, I2S_BCK + 1, I2S_DIN}) {
    gpio_init(pin);
    gpio_put(pin, 0);
    gpio_set_dir(pin, GPIO_OUT);
  }
}

void RPU400Audio::begin(unsigned long) {
  if (started) return;
  queue_init(&toPlayer, 1, 1024);
  queue_init(&ended, 2, 64);
  started = true;          // core 1 waits for this in setup1()
}

int RPU400Audio::available() {
  if (started) takeEnded();
  return outCount;
}

int RPU400Audio::read() {
  if (!outCount) return -1;
  uint8_t b = outBuf[outHead];
  outHead = (outHead + 1) % sizeof outBuf;
  outCount--;
  return b;
}

size_t RPU400Audio::write(const uint8_t *buf, size_t n) {
  if (!started) return n;
  takeEnded();
  for (size_t i = 0; i < n; i++)
    if (fromGame.take(buf[i])) fromGamePacket(fromGame.pkt, fromGame.len);
  return n;
}

void setup1() {
  while (!started) delay(1);
  i2s.setBCLK(I2S_BCK);
  i2s.setDATA(I2S_DIN);
  i2s.setBitsPerSample(16);
  i2s.setBuffers(8, 256);
  bool ringOk = i2s.begin(OUT_RATE);
  SPI1.setRX(SD_MISO);
  SPI1.setCS(SD_CS);
  SPI1.setSCK(SD_SCK);
  SPI1.setTX(SD_MOSI);
  for (uint32_t mhz : {50u, 25u, 12u}) {             // the fastest the card and its wiring take
    if (sd.begin(SdSpiConfig(SD_CS, DEDICATED_SPI, SD_SCK_MHZ(mhz), &SPI1)) && root.open("/")) { cardOk = true; cardMHz = mhz; break; }
    sd.end();
  }
  if (cardOk) { shortenLongNames(root, 0, 0); indexCard(); }
  for (auto &t : tracks) if (t.track < 4096) present[t.track / 8] |= 1 << (t.track % 8);
  lastTrack = tracks.empty() ? 0 : tracks.back().track;
  indexReady = true;
  unsigned fromSlot = 0;
  for (auto &t : tracks) fromSlot += t.inSlot;
  Serial.printf("RPU400 audio: card %s (%lu MHz), %u tracks, %u from slot%d\n", cardOk ? "ok" : "MISSING",
                (unsigned long)cardMHz, (unsigned)tracks.size(), fromSlot, launchedSlot());
  setStatus("audio: out %s, card %s %luMHz, %u tracks (%u slot%d)", ringOk ? "ok" : "FAILED", cardOk ? "ok" : "none",
            (unsigned long)cardMHz, (unsigned)tracks.size(), fromSlot, launchedSlot());
}

void loop1() {
  if (silenced) {
    static bool done = false;
    if (!done) { for (int i = 0; i < VOICES; i++) stopVoice(i); done = true; }
  }
  takeFileJob();
  takeCommands();
  if (ringSilence) { ringSilence = false; ringSilent = true; }
  while (i2s.availableForWrite() >= BLOCK) mixBlock();
  // top up the voice with the least buffered
  int need = -1;
  for (int v = 0; v < VOICES; v++) {
    Voice &vc = voice[v];
    if (vc.active && !vc.eof && BUF_FRAMES - vc.count >= READ_FRAMES && (need < 0 || vc.count < voice[need].count)) need = v;
  }
  if (need >= 0) refill(voice[need]);

  int active = 0;
  for (auto &vc : voice) active += vc.active;
  if ((uint32_t)active > st.maxVoices) st.maxVoices = active;
  static uint32_t lastPrint = 0, lastSum = 0;
  uint32_t sum = st.plays + st.missing + st.badFile + st.underruns + st.readErrs + dropsIn + st.maxVoices;
  if (millis() - lastPrint > 3000 && sum != lastSum) {
    lastPrint = millis(); lastSum = sum;
    setStatus("audio: plays %lu missing %lu bad %lu under %lu", st.plays, st.missing, st.badFile, st.underruns);
    Serial.printf("AUD plays %lu missing %lu bad %lu underruns %lu readErr %lu maxOpen %lu us maxRead %lu us voices %d (max %lu) dropped %lu\n",
                  st.plays, st.missing, st.badFile, st.underruns, st.readErrs, st.maxOpenUs, st.maxReadUs, active,
                  st.maxVoices, (unsigned long)dropsIn);
  }
}

// PinNode 400 update: the node halts the RP to flash it, and the DAC then repeats its buffers forever.
// Stop every sound and give the mixer time to fill them all with silence (8 x 256 frames, 46 ms).
extern "C" void PinNode400_AudioSilence() {
  if (!started) return;
  silenced = true;
  delay(150);
}

// RPU Rev 400, around an EEPROM flash write (it pauses this player's core): ease the output to silence
// and wait until the DAC's buffers hold only silence (8 x 256 frames), then back up afterwards
extern "C" void PinNode400_AudioHold(bool hold) {
  if (!started) return;
  if (!hold) { masterTarget = 16384; return; }
  silentBlocks = 0;
  masterTarget = 0;                                    // fade the output out (a block)
  uint32_t t0 = millis();
  while (silentBlocks < 2 && millis() - t0 < 50) tight_loop_contents();
  ringSilent = false;
  ringSilence = true;                                  // then core 1 zeroes the whole ring
  while (!ringSilent && millis() - t0 < 100) tight_loop_contents();
}

// PinNode 400: the player's newest status line, once (the PinNode library sends it to the node log)
extern "C" int PinNode400_AudioStatus(char *out, unsigned int size) {
  if (!started || statusSeq == statusSent) return 0;
  statusSent = statusSeq;
  __dmb();
  strlcpy(out, statusLine, size);
  return 1;
}

// PinNode 400: the app plays a file from the card on the machine (the library's SOUND_PREVIEW). op 1 play
// (replacing any preview), 0 stop. Returns PINNODE_PREVIEW_ states: 0 starting (PinNode400_PreviewPoll
// says when it plays), 1 stopped, 2 bad name, 3 not now (the card is busy with files).
extern "C" int PinNode400_Preview(int op, const char *path) {
  if (!started) return 4;
  for (int i = 0; i < VOICES; i++)
    if (slot[i].active && slot[i].track == PREVIEW_TRACK) {
      uint8_t pkt[8] = {SOM1, SOM2, 8, CMD_TRACK_CONTROL, TRK_STOP, (uint8_t)PREVIEW_TRACK, (uint8_t)(PREVIEW_TRACK >> 8), EOM};
      forward(pkt, 8);
      freeSlot(i);
    }
  previewEnd = 0;
  previewOpened = false;
  if (!op) return 1;
  if (!wavPath(path)) return 2;
  if (fileSession) return 3;
  strlcpy(previewPath, path, sizeof previewPath);
  __dmb();
  startTrack(TRK_PLAY_POLY, PREVIEW_TRACK, false);
  return 0;
}

// What happened to the preview since the last call, once each: 10 its file opened and it plays (the
// first answer waits for this), 5 it ended, 6 the file didn't open; 0 nothing new
extern "C" int PinNode400_PreviewPoll() {
  if (!started) return 0;
  takeEnded();
  if (previewOpened) { previewOpened = false; return 10; }
  int e = previewEnd;
  previewEnd = 0;
  return e;
}

// PinNode 400: a volume pip (the PinNode library's PinNode_VolumePip, after a volume change)
extern "C" void PinNode400_Pip(int gainDb) {
  if (!started) return;
  uint8_t pkt[6] = {SOM1, SOM2, 6, CMD_PIP, (uint8_t)(int8_t)gainDb, EOM};
  forward(pkt, 6);
}

// PinNode 400: the card's state for the node's health report (the PinNode library's CARD_STATUS)
extern "C" int PinNode400_CardStatus(unsigned int *tracksOut) {
  if (!indexReady) return 0;
  *tracksOut = tracks.size();
  return cardOk ? 1 : 0;
}

// PinNode 400 bulk mode (the PinNode library's PinNodeBulk.h): one file op, run on core 1
extern "C" int PinNode400_FileOp(unsigned char op, const char *name, unsigned long arg0, unsigned long arg1,
                                 const unsigned char *data, unsigned int len, unsigned char *out,
                                 unsigned int *outLen, unsigned long *r0, unsigned long *r1) {
  if (!started) return 2;                  // no player running: no card
  job.op = op;
  strlcpy(job.name, name ? name : "", sizeof job.name);
  job.a0 = arg0; job.a1 = arg1;
  job.data = data; job.len = len;
  job.out = out; job.outLen = 0;
  job.r0 = job.r1 = 0;
  __dmb();
  job.state = 1;
  uint32_t t0 = millis();
  while (job.state != 2) {
    if (millis() - t0 > 5000) return 4;    // core 1 stuck: an I/O error to the node
    tight_loop_contents();
  }
  *outLen = job.outLen; *r0 = job.r0; *r1 = job.r1;
  job.state = 0;
  return job.status;
}

#else
// The host test rig: no sound
void RPU400Audio::begin(unsigned long) {}
int RPU400Audio::available() { return 0; }
int RPU400Audio::read() { return -1; }
size_t RPU400Audio::write(const uint8_t *, size_t n) { return n; }
#endif
