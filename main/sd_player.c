// SPDX-FileCopyrightText: 2026 airplay-esp32 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file sd_player.c
 * @brief SD-card MP3 player
 *
 * Scans CONFIG_SD_MUSIC_DIR (falling back to the card root) for .mp3 files
 * and plays them in relative-path order. Decoding uses the Espressif MP3
 * decoder, and the PCM goes through audio_output_write_pcm() — the same path
 * the Bluetooth and USB sinks use, so channel mode and the DAC's own volume
 * all apply.
 *
 * The player never runs at the same time as another source: connecting
 * AirPlay or Bluetooth stops it, and it refuses to start while either is
 * connected.
 */

#include "sd_player.h"

#include "audio_output.h"
#include "playback_control.h"
#include "playback_events.h"

#include "driver/sdmmc_host.h"
#include "esp_audio_dec.h"
#include "decoder/impl/esp_mp3_dec.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#if CONFIG_SD_PLAYER_ENABLE

static const char *TAG = "sd_player";

#define SD_MAX_TRACKS 1000
#define SD_PATH_MAX   192
#define SD_TEXT_MAX   64
#define SD_READ_BYTES (8 * 1024)
// One MP3 frame is at most 1152 frames x 2 ch x 2 bytes = 4608 bytes.
#define SD_FRAME_BYTES 4608
#define SD_PCM_BYTES   (SD_FRAME_BYTES * 1)
// Room for the decoder call's own frames as well as the task's state.
#define SD_TASK_STACK 12288
#define SD_TASK_PRIO  10
// Core 1 (APP CPU): core 0 carries WiFi, Bluetooth and the I2S driver.
#define SD_TASK_CORE 1

static char *s_paths; // SD_MAX_TRACKS * SD_PATH_MAX, PSRAM
static char *s_names; // SD_MAX_TRACKS * SD_PATH_MAX, PSRAM
static int s_count = 0;
static SemaphoreHandle_t s_lock = NULL;

static TaskHandle_t s_task = NULL;
static sdmmc_card_t *s_card = NULL;

static volatile bool s_want_play = false;
static volatile bool s_paused = false;
static volatile bool s_seek = false;
static volatile int s_index = -1;
static volatile bool s_playing = false;

static char *track_path(int i) {
  return s_paths + (size_t)i * SD_PATH_MAX;
}
static char *track_name(int i) {
  return s_names + (size_t)i * SD_PATH_MAX;
}

// ============================================================================
// Card and folder
// ============================================================================

static esp_err_t ensure_mounted(void) {
  if (s_card) {
    return ESP_OK;
  }
  if (CONFIG_SD_CLK_GPIO < 0 || CONFIG_SD_CMD_GPIO < 0 ||
      CONFIG_SD_D0_GPIO < 0) {
    ESP_LOGE(TAG, "SD pins are not configured");
    return ESP_ERR_INVALID_STATE;
  }

  sdmmc_host_t host = SDMMC_HOST_DEFAULT();
  host.slot = SDMMC_HOST_SLOT_1;

  sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
  slot.clk = CONFIG_SD_CLK_GPIO;
  slot.cmd = CONFIG_SD_CMD_GPIO;
  slot.d0 = CONFIG_SD_D0_GPIO;
  slot.width = 1;
  slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

  const esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
      .format_if_mount_failed = false,
      .max_files = 4,
      .allocation_unit_size = 16 * 1024,
  };
  esp_err_t err = esp_vfs_fat_sdmmc_mount(CONFIG_SD_MOUNT_POINT, &host, &slot,
                                          &mount_cfg, &s_card);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "No card at %s: %s", CONFIG_SD_MOUNT_POINT,
             esp_err_to_name(err));
    s_card = NULL;
    return err;
  }
  ESP_LOGI(TAG, "Card mounted at %s: %s %uMB", CONFIG_SD_MOUNT_POINT,
           s_card->cid.name,
           (unsigned)((uint64_t)s_card->csd.capacity * s_card->csd.sector_size /
                      (1024ULL * 1024ULL)));
  return ESP_OK;
}

static bool is_audio(const char *name) {
  const char *dot = strrchr(name, '.');
  return dot && strcasecmp(dot, ".mp3") == 0;
}

static int name_cmp(const void *a, const void *b) {
  return strcasecmp((const char *)a, (const char *)b);
}

// Collects the paths recursively (subfolders included), then sorts them by
// relative path. The list is swapped in whole, so a reader that runs under
// s_lock never sees a half-filled list.
#define SD_MAX_DEPTH 6

static int s_scan_count;

static void scan_dir(const char *dir, const char *rel, int depth) {
  if (depth > SD_MAX_DEPTH || s_scan_count >= SD_MAX_TRACKS) {
    return;
  }
  DIR *d = opendir(dir);
  if (!d) {
    return;
  }

  struct dirent *ent;
  while ((ent = readdir(d)) != NULL && s_scan_count < SD_MAX_TRACKS) {
    if (ent->d_name[0] == '.') {
      continue;
    }
    char path[SD_PATH_MAX];
    int written = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
    if (written <= 0 || written >= (int)sizeof(path)) {
      continue;
    }
    struct stat st;
    if (stat(path, &st) != 0) {
      continue;
    }
    char relpath[SD_PATH_MAX];
    int rw = rel[0]
                 ? snprintf(relpath, sizeof(relpath), "%s/%s", rel, ent->d_name)
                 : snprintf(relpath, sizeof(relpath), "%s", ent->d_name);
    if (rw <= 0 || rw >= (int)sizeof(relpath)) {
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      scan_dir(path, relpath, depth + 1);
    } else if (S_ISREG(st.st_mode) && is_audio(ent->d_name)) {
      strlcpy(track_path(s_scan_count), path, SD_PATH_MAX);
      strlcpy(track_name(s_scan_count), relpath, SD_PATH_MAX);
      s_scan_count++;
    }
  }
  closedir(d);
}

static void scan_folder(const char *dir) {
  s_scan_count = 0;
  scan_dir(dir, "", 0);
  int n = s_scan_count;

  if (n > 0) {
    qsort(s_names, (size_t)n, SD_PATH_MAX, name_cmp);
    // qsort moved the names, so reload each path from the relative name.
    for (int i = 0; i < n; i++) {
      snprintf(track_path(i), SD_PATH_MAX, "%s/%s", dir, track_name(i));
    }
  }

  xSemaphoreTake(s_lock, portMAX_DELAY);
  s_count = n;
  xSemaphoreGive(s_lock);

  ESP_LOGI(TAG, "%d track%s in %s (recursive)", n, n == 1 ? "" : "s", dir);
}

// ============================================================================
// Decoding
// ============================================================================

// The channel is initialised at CONFIG_OUTPUT_SAMPLE_RATE_HZ; only retune for
// files that actually differ, since reconfiguring the clock is disruptive.
static uint32_t s_i2s_rate = CONFIG_OUTPUT_SAMPLE_RATE_HZ;

static void use_rate(uint32_t hz) {
  if (hz == 0 || hz == s_i2s_rate) {
    return;
  }
  audio_output_set_sample_rate(hz);
  s_i2s_rate = hz;
}

static bool skip_id3v2(FILE *f) {
  uint8_t hdr[10];
  size_t got = fread(hdr, 1, sizeof(hdr), f);
  long offset = 0;
  if (got == sizeof(hdr) && memcmp(hdr, "ID3", 3) == 0) {
    uint32_t size = ((uint32_t)(hdr[6] & 0x7f) << 21) |
                    ((uint32_t)(hdr[7] & 0x7f) << 14) |
                    ((uint32_t)(hdr[8] & 0x7f) << 7) | (hdr[9] & 0x7f);
    offset = (long)sizeof(hdr) + (long)size;
  }
  // A short probe read leaves the EOF flag set, and a failed seek leaves the
  // position indeterminate; clear the flag and do one checked absolute seek.
  clearerr(f);
  return fseek(f, offset, SEEK_SET) == 0;
}

static void copy_span(const char *s, const char *e, char *dst, size_t cap) {
  size_t n = (size_t)(e - s);
  if (n >= cap) {
    n = cap - 1;
  }
  memcpy(dst, s, n);
  dst[n] = '\0';
}

// "Artist/Album/title.mp3" -> title="title.mp3", album="Album",
// artist="Artist". Folders are only labels; a missing level is left empty.
static void split_labels(const char *rel, char *title, size_t tcap,
                         char *artist, size_t acap, char *album, size_t lcap) {
  title[0] = '\0';
  artist[0] = '\0';
  album[0] = '\0';
  const char *base = strrchr(rel, '/');
  if (!base) {
    strlcpy(title, rel, tcap);
    return;
  }
  strlcpy(title, base + 1, tcap);

  const char *album_end = base;
  const char *album_start = album_end;
  while (album_start > rel && album_start[-1] != '/') {
    album_start--;
  }
  copy_span(album_start, album_end, album, lcap);
  if (album_start == rel) {
    return;
  }
  const char *artist_end = album_start - 1;
  const char *artist_start = artist_end;
  while (artist_start > rel && artist_start[-1] != '/') {
    artist_start--;
  }
  copy_span(artist_start, artist_end, artist, acap);
}

static void emit_metadata(const char *relpath) {
  playback_event_data_t data = {0};
  char title[SD_TEXT_MAX];
  char artist[SD_TEXT_MAX];
  char album[SD_TEXT_MAX];
  split_labels(relpath, title, sizeof(title), artist, sizeof(artist), album,
               sizeof(album));
  strlcpy(data.metadata.title, title, METADATA_STRING_MAX);
  strlcpy(data.metadata.artist, artist, METADATA_STRING_MAX);
  strlcpy(data.metadata.album, album, METADATA_STRING_MAX);
  playback_events_emit(PLAYBACK_SOURCE_SD, PLAYBACK_EVENT_METADATA, &data);
}

static void end_session(void) {
  if (!s_playing) {
    return;
  }
  s_playing = false;
  playback_events_emit(PLAYBACK_SOURCE_SD, PLAYBACK_EVENT_DISCONNECTED, NULL);
  if (s_i2s_rate != CONFIG_OUTPUT_SAMPLE_RATE_HZ) {
    audio_output_set_sample_rate(CONFIG_OUTPUT_SAMPLE_RATE_HZ);
    s_i2s_rate = CONFIG_OUTPUT_SAMPLE_RATE_HZ;
  }
  // Hand the output back. AirPlay services are started at boot whether or not
  // a session is open, so an idle output belongs to AirPlay again; its render
  // task was stopped when the card session began and has to be restarted.
  playback_source_t active = playback_events_active_source();
  if (active == PLAYBACK_SOURCE_BLUETOOTH || active == PLAYBACK_SOURCE_USB) {
    playback_control_set_source(active);
  } else {
    playback_control_set_source(PLAYBACK_SOURCE_AIRPLAY);
    audio_output_start();
  }
}

// Bring decoder output to 16-bit interleaved stereo, in place.
static size_t convert_pcm(uint8_t *buf, size_t bytes, int channels) {
  if (channels == 1) {
    size_t frames = bytes / 2;
    int16_t *p = (int16_t *)buf;
    for (size_t i = frames; i-- > 0;) {
      p[2 * i] = p[i];
      p[2 * i + 1] = p[i];
    }
    bytes *= 2;
  }
  return bytes;
}

static void player_task(void *arg) {
  (void)arg;

  int16_t *pcm = heap_caps_malloc(SD_PCM_BYTES, MALLOC_CAP_SPIRAM);
  uint8_t *buf = heap_caps_malloc(SD_READ_BYTES, MALLOC_CAP_SPIRAM);
  void *dec_mp3 = NULL;
  esp_audio_err_t mp3_err = esp_mp3_dec_open(NULL, 0, &dec_mp3);
  if (!pcm || !buf || mp3_err != ESP_AUDIO_ERR_OK) {
    ESP_LOGE(TAG, "MP3 decoder/buffers unavailable (mp3=%d)", mp3_err);
    free(pcm);
    free(buf);
    if (dec_mp3) {
      esp_mp3_dec_close(dec_mp3);
    }
    s_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  bool last_paused = false;

  for (;;) {
    if (!s_want_play) {
      end_session();
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }

    int idx = s_index;
    if (idx < 0 || idx >= s_count) {
      s_want_play = false;
      continue;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    char path[SD_PATH_MAX];
    strlcpy(path, track_path(idx), sizeof(path));
    char title[SD_PATH_MAX];
    strlcpy(title, track_name(idx), sizeof(title));
    xSemaphoreGive(s_lock);

    FILE *f = fopen(path, "rb");
    if (!f) {
      ESP_LOGE(TAG, "Cannot open %s", path);
      if (s_index + 1 < s_count) {
        s_index++;
        continue;
      }
      s_want_play = false;
      continue;
    }

    if (!skip_id3v2(f)) {
      // A failed seek leaves the stream position undefined; skip the file
      // rather than read from it.
      ESP_LOGW(TAG, "Cannot read %s", path);
      fclose(f);
      if (s_index + 1 < s_count) {
        s_index++;
        continue;
      }
      s_want_play = false;
      continue;
    }
    esp_mp3_dec_reset(dec_mp3);

    if (!s_playing) {
      playback_control_set_source(PLAYBACK_SOURCE_SD);
      s_playing = true;
      playback_events_emit(PLAYBACK_SOURCE_SD, PLAYBACK_EVENT_CONNECTED, NULL);
    }
    emit_metadata(title);
    last_paused = s_paused;
    playback_events_emit(
        PLAYBACK_SOURCE_SD,
        s_paused ? PLAYBACK_EVENT_PAUSED : PLAYBACK_EVENT_PLAYING, NULL);
    ESP_LOGI(TAG, "Playing %d/%d: %s", idx + 1, s_count, title);

    size_t avail = 0;
    bool eof = false;

    while (s_want_play && !s_seek) {
      if (s_paused != last_paused) {
        last_paused = s_paused;
        playback_events_emit(
            PLAYBACK_SOURCE_SD,
            s_paused ? PLAYBACK_EVENT_PAUSED : PLAYBACK_EVENT_PLAYING, NULL);
      }
      if (s_paused) {
        vTaskDelay(pdMS_TO_TICKS(30));
        continue;
      }

      if (!eof && avail < SD_READ_BYTES) {
        size_t want = SD_READ_BYTES - avail;
        size_t got = fread(buf + avail, 1, want, f);
        if (got < want) {
          // A regular file only reads short at end of file, which also sets
          // the stream's EOF flag. Stop reading rather than call fread again
          // with the stream in that state.
          eof = true;
        }
        avail += got;
      }
      if (avail == 0) {
        break;
      }

      size_t consumed = 0;
      size_t pending = 0;
      while (consumed < avail && s_want_play && !s_seek) {
        esp_audio_dec_in_raw_t raw = {
            .buffer = buf + consumed,
            .len = (uint32_t)(avail - consumed),
        };
        esp_audio_dec_out_frame_t frame = {
            .buffer = (uint8_t *)pcm + pending,
            .len = SD_PCM_BYTES - pending,
        };
        esp_audio_dec_info_t info = {0};
        esp_audio_err_t ret = esp_mp3_dec_decode(dec_mp3, &raw, &frame, &info);
        if (ret == ESP_AUDIO_ERR_DATA_LACK || raw.consumed == 0) {
          // A frame split across the read boundary: keep the tail and read
          // more. Garbage with no progress is dropped one byte at a time.
          if (ret == ESP_AUDIO_ERR_DATA_LACK || avail - consumed < 2048) {
            break;
          }
          consumed++;
          continue;
        }
        consumed += raw.consumed;
        if (ret != ESP_AUDIO_ERR_OK || frame.decoded_size == 0) {
          vTaskDelay(1); // never spin without yielding
          continue;
        }

        uint32_t channels = info.channel ? info.channel : 2;
        use_rate(info.sample_rate);
        uint8_t *out = (uint8_t *)pcm + pending;
        pending += convert_pcm(out, frame.decoded_size, (int)channels);

        // Write a batch once another full frame would not fit.
        if (pending + SD_FRAME_BYTES > SD_PCM_BYTES) {
          audio_output_write_pcm(pcm, pending, 32768, pdMS_TO_TICKS(500));
          pending = 0;
        }
      }

      if (consumed > 0) {
        memmove(buf, buf + consumed, avail - consumed);
        avail -= consumed;
      } else if (eof) {
        break; // nothing decodable left
      }

      if (consumed == 0) {
        if (avail >= SD_READ_BYTES) {
          // Full buffer with no progress: a corrupt frame. Drop one byte and
          // resync instead of abandoning the track.
          memmove(buf, buf + 1, avail - 1);
          avail--;
        }
        // Yield: otherwise a codec that keeps asking for data can starve IDLE.
        vTaskDelay(pdMS_TO_TICKS(2));
      }

      if (pending > 0) {
        audio_output_write_pcm(pcm, pending, 32768, pdMS_TO_TICKS(500));
      }

      if (eof && avail < 4) {
        break;
      }
    }

    fclose(f);

    if (s_seek) {
      s_seek = false;
      continue;
    }
    if (!s_want_play) {
      continue;
    }

    // Natural end of the file: advance, or finish the folder.
    if (s_index + 1 < s_count) {
      s_index++;
      continue;
    }
    s_want_play = false;
  }
}

// ============================================================================
// Playback arbitration
// ============================================================================

static void on_other_source(playback_source_t source, playback_event_t event,
                            const playback_event_data_t *data, void *user) {
  (void)data;
  (void)user;
  if (source == PLAYBACK_SOURCE_SD || event == PLAYBACK_EVENT_METADATA) {
    return;
  }
  if (s_want_play) {
    ESP_LOGI(TAG, "Stopping for source %d event %d", source, event);
    s_want_play = false;
    if (s_task) {
      xTaskNotifyGive(s_task);
    }
  }
}

// ============================================================================
// Public API
// ============================================================================

void sd_player_init(void) {
  if (s_paths) {
    return;
  }
  s_paths =
      heap_caps_malloc((size_t)SD_MAX_TRACKS * SD_PATH_MAX, MALLOC_CAP_SPIRAM);
  s_names =
      heap_caps_malloc((size_t)SD_MAX_TRACKS * SD_PATH_MAX, MALLOC_CAP_SPIRAM);
  s_lock = xSemaphoreCreateMutex();
  if (!s_paths || !s_names || !s_lock) {
    ESP_LOGE(TAG, "Out of memory");
    return;
  }

  xTaskCreatePinnedToCore(player_task, "sd_mp3", SD_TASK_STACK, NULL,
                          SD_TASK_PRIO, &s_task, SD_TASK_CORE);
  playback_events_register(on_other_source, NULL);

  if (ensure_mounted() != ESP_OK) {
    // Stay initialised: the card may be inserted later and refresh will pick
    // it up.
    ESP_LOGW(TAG, "No card at boot; insert one and press Refresh");
    return;
  }

  if (opendir(CONFIG_SD_MUSIC_DIR)) {
    scan_folder(CONFIG_SD_MUSIC_DIR);
  } else {
    scan_folder(CONFIG_SD_MOUNT_POINT);
  }

  ESP_LOGI(TAG, "Ready: %d track(s)", s_count);
}

int sd_player_refresh(void) {
  if (!s_paths) {
    return 0;
  }
  s_want_play = false;
  if (s_task) {
    xTaskNotifyGive(s_task);
  }
  vTaskDelay(pdMS_TO_TICKS(50)); // let the decoder close the file

  if (ensure_mounted() != ESP_OK) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count = 0;
    xSemaphoreGive(s_lock);
    return 0;
  }
  if (opendir(CONFIG_SD_MUSIC_DIR)) {
    scan_folder(CONFIG_SD_MUSIC_DIR);
  } else {
    scan_folder(CONFIG_SD_MOUNT_POINT);
  }
  return s_count;
}

int sd_player_get_track_count(void) {
  return s_count;
}

int sd_player_get_current(void) {
  return (s_want_play && s_index >= 0) ? s_index : -1;
}

const char *sd_player_get_track_name(int index) {
  if (index < 0 || index >= s_count) {
    return NULL;
  }
  return track_name(index);
}

void sd_player_get_track_meta(int index, char *title, size_t tcap, char *artist,
                              size_t acap, char *album, size_t lcap) {
  if (!title || !artist || !album || tcap == 0 || acap == 0 || lcap == 0) {
    return;
  }
  if (index < 0 || index >= s_count) {
    title[0] = artist[0] = album[0] = '\0';
    return;
  }
  split_labels(track_name(index), title, tcap, artist, acap, album, lcap);
}

bool sd_player_is_playing(void) {
  return s_playing && !s_paused;
}

bool sd_player_is_paused(void) {
  return s_playing && s_paused;
}

void sd_player_play(int index) {
  if (!s_task || s_count == 0) {
    ESP_LOGW(TAG, "Nothing to play");
    return;
  }
  // Never take the output from a source that is actually streaming. This has
  // to run before audio_output_stop(): stopping first and then refusing left
  // the other source silent with no task to render it.
  if (playback_events_any_playing() &&
      playback_events_active_source() != PLAYBACK_SOURCE_SD) {
    ESP_LOGW(TAG, "Another source is playing; not starting");
    return;
  }
  // The output channel is created lazily (AirPlay does the same). If it is
  // already up, init returns an error here and that is fine.
  if (audio_output_init() != ESP_OK) {
    ESP_LOGD(TAG, "Audio output already up");
  }
  // Take the output: AirPlay's render task would otherwise write into the
  // same DMA ring, which is audible as warble. It is restarted in
  // end_session() when the card player finishes.
  audio_output_stop();
  audio_output_flush();
  if (index < 0 || index >= s_count) {
    index = 0;
  }
  // Selecting a different track while a session is already open has to
  // interrupt the decode loop; a fresh start picks the index up anyway.
  const bool was_active = s_want_play;
  s_index = index;
  s_paused = false;
  if (was_active) {
    s_seek = true;
  }
  s_want_play = true;
  xTaskNotifyGive(s_task);
}

// Start the last loaded track (or the first one after a fresh boot).
void sd_player_resume(void) {
  if (!s_task || s_count == 0) {
    return;
  }
  int idx = s_index;
  if (idx < 0 || idx >= s_count) {
    idx = 0;
  }
  sd_player_play(idx);
}

void sd_player_next(void) {
  if (!s_task || s_count == 0) {
    return;
  }
  if (!s_want_play) {
    sd_player_play(0);
    return;
  }
  s_index = (s_index + 1) % s_count;
  s_seek = true;
  xTaskNotifyGive(s_task);
}

void sd_player_prev(void) {
  if (!s_task || s_count == 0) {
    return;
  }
  if (!s_want_play) {
    sd_player_play(s_count - 1);
    return;
  }
  s_index = (s_index + s_count - 1) % s_count;
  s_seek = true;
  xTaskNotifyGive(s_task);
}

// An album is the track's parent folder, so "next album" is the first later
// track whose folder differs. Wraps around to the first album at the end.
void sd_player_next_album(void) {
  if (!s_task || s_count == 0) {
    return;
  }
  if (!s_want_play) {
    // Album skip only: do not start playback from this key.
    ESP_LOGI(TAG, "Next album ignored: player idle");
    return;
  }
  int cur = s_index;
  if (cur < 0 || cur >= s_count) {
    return;
  }

  char title[SD_TEXT_MAX];
  char artist[SD_TEXT_MAX];
  char cur_album[SD_TEXT_MAX];
  char album[SD_TEXT_MAX];
  split_labels(track_name(cur), title, sizeof(title), artist, sizeof(artist),
               cur_album, sizeof(cur_album));

  for (int pass = 0; pass < 2; pass++) {
    int first = pass == 0 ? cur + 1 : 0;
    int last = pass == 0 ? s_count : cur;
    for (int i = first; i < last; i++) {
      split_labels(track_name(i), title, sizeof(title), artist, sizeof(artist),
                   album, sizeof(album));
      if (strcmp(album, cur_album) != 0) {
        sd_player_play(i);
        return;
      }
    }
  }
  // Only one album on the card: start it again.
  sd_player_play(0);
}

void sd_player_stop(void) {
  s_want_play = false;
  if (s_task) {
    xTaskNotifyGive(s_task);
  }
}

void sd_player_toggle_pause(void) {
  if (!s_want_play) {
    return;
  }
  s_paused = !s_paused;
  if (s_task) {
    xTaskNotifyGive(s_task);
  }
}

#endif // CONFIG_SD_PLAYER_ENABLE
