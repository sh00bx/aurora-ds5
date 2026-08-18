#pragma once

#include <stdbool.h>

/*
 * Drive the TV's game mode for the duration of a stream.
 *
 * Everything this does needs root: stopping the discovery/cast services that
 * fight the DS5 for the combo chip's airtime, pinning the cores LG's power
 * governor keeps hotplugging, boosting the stream threads, and switching the
 * panel to its game picture preset. The work itself lives in gamemode.sh, which
 * ships inside this IPK; this is the part that decides WHEN it runs.
 *
 * Begin/end are safe to call unbalanced: end without begin does nothing, and a
 * second begin while one is running is ignored.
 */
void tv_game_mode_stream_begin(void);

void tv_game_mode_stream_end(void);

/*
 * Undo a game mode that outlived the app. Call once at start-up: if a previous
 * run was killed mid-stream, the TV is still on stopped services and a game
 * picture preset, and nothing else will ever put that back.
 */
void tv_game_mode_recover_stale(void);
