# StarTrek2022-RPU

New rules for Bally's Star Trek (1979), using RPU (Retro Pin Upgrade), and reporting scores, achievements and
settings to PinNode. It runs on an Arduino Mega in an RPU Rev 4 board with a PinNode card, or on a PinNode 400-J5
(RP2350) on the MPU's J5. It plays its sounds through a WAV Trigger, or on the 400-J5 through the board's own player.

A fan project for one machine at home. GPL-3.0, as RPU is (`RPU400Audio` has its own licence, in
`LICENSE-RPU400Audio.txt`).

## Build

Install two Arduino libraries: **PinNode** (0.3.20 or later) and
[**RPUMachines**](https://github.com/wifijt/RPUMachines). `RPU_Config.h` picks the board: Rev 400 for the RP2350,
Rev 4 for the Mega.

```
# Arduino Mega 2560 (RPU Rev 4)
arduino-cli compile -b arduino:avr:mega StarTrek2022-RPU

# PinNode 400-J5 (arduino-pico, Waveshare RP2350 Core)
arduino-cli compile -b rp2040:rp2040:waveshare_rp2350_core \
  --build-property "compiler.c.extra_flags=-fsigned-char" \
  --build-property "compiler.cpp.extra_flags=-fsigned-char -Wno-narrowing" \
  StarTrek2022-RPU
```

Each release has the Mega's `.hex`, the 400-J5's `.uf2` and the `pinnode.game.json` they were built with.
