# AI-Thinker ESP32-A1S Audio Kit

The [ESP32-A1S Audio Kit](https://docs.ai-thinker.com/en/esp32-audio-kit) is an ESP32-WROVER
board with an **ES8388** codec, two 3 W class-D amplifiers, a headphone jack, line-in and six
push buttons on one PCB. It is one of the cheapest ways to get AirPlay into a pair of passive
speakers: everything except the speakers is on the board.

## Which module do you have?

The ESP32-A1S module exists in two electrically different revisions, and they **swap two I2S
pins**:

| Module | Codec | I2S WS (LRCK) | I2S DOUT |
| --- | --- | :-: | :-: |
| V2.x (this page) | ES8388 | 25 | 26 |
| V1.x, discontinued | AC101 | 26 | 25 |

This board definition targets the **ES8388** revision (Audio Kit V2.2 and later, the only one
still sold). An AC101 module will produce silence with it and needs its own codec driver â€”
upstream carries none.

## Variants

| Environment | Flash | Bluetooth | Notes |
| --- | :-: | :-: | --- |
| `esp32-a1s` | 8 MB | â€” | 3 MB OTA slots; the layout most boards actually ship with |
| `esp32-a1s-bt` | 8 MB | yes | Same image plus Classic BT A2DP |
| `esp32-a1s-4m` | 4 MB | â€” | Datasheet-spec boards: 1.92 MB OTA slots, no Sendspin |

The Ai-Thinker datasheet says **4 MB flash**, but most modules sold today carry **8 MB**.
Check before flashing:

```bash
python -m esptool -p <port> flash-id     # "Detected flash size: 8MB"
```

Build `esp32-a1s` for 8 MB and `esp32-a1s-4m` for 4 MB. Flashing the 8 MB layout onto a
4 MB chip fails to boot, so get this right. There is no prebuilt binary.

## Features

- ES8388 codec over I2C at 0x10, with the codec's own digital volume
  (`CONFIG_DAC_CONTROLS_VOLUME`)
- MCLK from GPIO0, 44.1 kHz output (no resampler on the FPU-less ESP32)
- Two NS4150 amplifiers behind GPIO21, enabled only while playing
- Green status LED on GPIO22 (active low)
- Play/pause and volume up/down on KEY4-6, previous/next on KEY2/KEY3 (DIP switch
  dependent) and next album on KEY1
- 8 MB flash, 8 MB PSRAM, OTA updates, 44.1 kHz

Not used by this firmware: the microphones, line-in, the JTAG header and the battery
charger.

## Flashing

=== "PlatformIO"

    ```bash
    pio run -e esp32-a1s -t upload
    pio run -e esp32-a1s -t uploadfs   # required â€” writes the web UI to SPIFFS
    ```

    For Bluetooth, swap in `esp32-a1s-bt`. Both builds keep OTA (`ota_0`/`ota_1` of
    3 MB each). On a 4 MB board use `esp32-a1s-4m` instead: its slots are 1.92 MB, which
    leaves no room for Bluetooth or Sendspin.

=== "ESP-IDF"

    ```bash
    idf.py set-target esp32
    idf.py -DSDKCONFIG_DEFAULTS="config/sdkconfig.defaults;config/sdkconfig.defaults.esp32-a1s" build
    idf.py -p /dev/ttyUSB0 flash
    idf.py -p /dev/ttyUSB0 -b 115200 monitor
    ```

    Append `config/sdkconfig.defaults.bt` for the Bluetooth build. Delete any cached
    `sdkconfig` when switching between them.

The board's CP2102 shows up as a normal USB serial port. GPIO0 is the codec's MCLK, so the
BOOT button cannot be used as a playback button; flashing still works through the CP2102's
DTR/RTS lines.

## Default GPIO assignments

| Function | GPIO | Notes |
| --- | :-: | --- |
| I2S MCLK | 0 | Codec master clock; shared with BOOT strapping |
| I2S BCK | 27 | |
| I2S WS (LRCK) | 25 | |
| I2S DOUT | 26 | |
| I2S DIN | 35 | Not used (no microphone support) |
| Codec I2C SDA | 33 | ES8388 at 0x10 |
| Codec I2C SCL | 32 | |
| Amplifier enable | 21 | Both NS4150s, active high |
| Status LED | 22 | Green, active low |
| Key 4 (play/pause) | 23 | |
| Key 5 (volume up) | 18 | |
| Key 6 (volume down) | 5 | |
| Key 1 (next album) | 36 | Input-only; needs an external pull-up, SD player only |
| Key 2 (previous) | 13 | Optional, shares the SD card's D3 line |
| Key 3 (next) | 19 | Optional, shares the D5 LED footprint |
| SD card CLK | 14 | SDMMC 1-bit |
| SD card CMD | 15 | DIP switch 3 (IO15-CMD) must be ON |
| SD card D0 | 2 | |

KEY2 shares GPIO13 with the SD card's CS and KEY3 shares GPIO19 with the LED labelled
D5; both depend on the DIP switches in the middle of the board. KEY1 sits on GPIO36,
which is input-only and has no internal pull-up: it works only if the line has an
external pull-up, and it is used for *next album* in the SD player.

## Amplifier handling

The ES8388 keeps its line and headphone outputs enabled all the time, so the 3.5 mm jack
works without any extra setup. The only muteable stage is the pair of NS4150 amplifiers:
the board raises GPIO21 on `PLAYBACK_EVENT_PLAYING` and drops it on pause or disconnect.
There is no jack detection or output switching (GPIO39 is unreliable on these boards), so
the speakers also play while headphones are connected.

## SD-card MP3 player

The card slot works as a local player: put audio files in `/music` on a FAT32 card (the card
root is scanned when that folder does not exist), then pick a track on the Overview page.
**Subfolders are scanned recursively** (up to six levels, 1000 files) and playback is in
relative-path order; the last track stops. MP3 is supported.

Titles come from the file name; the **album is the parent folder** and the **artist the
folder above it**, so `Artist/Album/Track.mp3` appears under Album with Track in it, and the
artist shows in the now-playing line. Files directly in `/music` have neither. No tag or
artwork parsing is done, which keeps boot fast.

- **DIP switches**: switch **1 (IO13-KEY2) ON** enables the *previous* key, switch
  **2 (IO13-DATA3) OFF** keeps the card in 1-bit SDMMC, and switch **3 (IO15-CMD) ON**
  is required for the card. KEY3 (`19`) is *next*; both keys control the player only
  while it is the active source. KEY1 (`36`) jumps to the next album and needs an
  external pull-up.
- **Priority**: starting AirPlay or connecting Bluetooth stops the card player, and it
  will not start while either is connected.
- **Format**: MP3 (MPEG-1/2/2.5 Layer III). Sample rate follows the file (44.1 kHz is not
  resampled) and mono is duplicated to both channels.
- **Limits**: 1000 files, names up to 191 characters, `CONFIG_FATFS_LFN_HEAP` enabled.

`CONFIG_SD_PLAYER_ENABLE`, `CONFIG_SD_MUSIC_DIR`, `CONFIG_SD_CLK_GPIO`, `CONFIG_SD_CMD_GPIO`
and `CONFIG_SD_D0_GPIO` are in menuconfig under *Airplay ESP Configuration â†’ SD Card
Player*. The web API is `GET /api/sd/list` and `POST /api/sd/control` with
`{"action":"play","index":0}`, `stop`, `next`, `prev`, `pause` or `rescan`.

## Related

- [ESP32-S3 + PCM5102A](esp32s3-pcm5102a.md) â€” the generic wiring this port is based on
- [Hardware buttons](../features/buttons.md)
- [Bluetooth A2DP](../features/bluetooth.md)
- [Build environments](../reference/build-environments.md)



