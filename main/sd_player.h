// SPDX-FileCopyrightText: 2026 airplay-esp32 contributors
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <stdbool.h>
#include <stddef.h>

/**
 * SD-card MP3 player.
 *
 * Scans one directory on the card for .mp3 files and plays them in filename
 * order through the same I2S path Bluetooth uses. AirPlay and Bluetooth
 * outrank it: connecting either stops local playback.
 */

/** Mount the card, scan the folder and start the player task. */
void sd_player_init(void);

/** Rescan the folder. Safe to call while playing (keeps the current file). */
int sd_player_refresh(void);

/** Number of tracks found by the last scan. */
int sd_player_get_track_count(void);

/** Index of the track being played, or -1 when idle. */
int sd_player_get_current(void);

/** File name (no path) of track @p index, or NULL. */
const char *sd_player_get_track_name(int index);

/** Folder-derived labels: title (file name), artist (top folder), album
 *  (parent folder). Missing levels are empty. */
void sd_player_get_track_meta(int index, char *title, size_t tcap, char *artist,
                              size_t acap, char *album, size_t lcap);

/** True while a track is playing (paused counts as stopped here). */
bool sd_player_is_playing(void);

/** True while a track is loaded but paused. */
bool sd_player_is_paused(void);

/** Start track @p index; stops whatever was playing. */
void sd_player_play(int index);

/** Move to the next/previous track, or start the first one when idle. */
void sd_player_next(void);
void sd_player_prev(void);

/** Stop and release the file and the amplifier. */
void sd_player_stop(void);

/** Pause/resume without losing the position. */
void sd_player_toggle_pause(void);
