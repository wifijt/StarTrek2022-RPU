// RPU400Audio: the RPU Rev 400 WAV player (RP2350, SD card, I2S DAC).
// Copyright (c) 2026 John Turner (wifijt)
// SPDX-License-Identifier: MIT (the full text is in LICENSE-RPU400Audio.txt)
#ifndef RPU400_AUDIO_H
#define RPU400_AUDIO_H
// RPU Rev 400's own sound, in place of a WAV Trigger. AudioHandler's wavTrigger class talks to this as it
// would to the WAV Trigger's serial port (same packets, same replies); core 1 plays the tracks from the
// micro-SD card (SPI1, GPIO40-43) through the PCM5100 DAC (I2S, GPIO44-46). The card is laid out as for
// the WAV Trigger: 001_name.wav or 0001_name.wav in its root, 16-bit mono or stereo, any sample rate.
#include <Arduino.h>

class RPU400Audio {
public:
  void begin(unsigned long baud);
  int available();
  int read();
  size_t write(const uint8_t *buf, size_t n);
  size_t write(uint8_t b) { return write(&b, 1); }
};

#endif
