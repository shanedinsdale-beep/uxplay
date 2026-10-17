/**
 * UxPlay - An open-source AirPlay mirroring server
 * Copyright (C) 2026 Shane Dinsdale
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
 */

/* -hls-forward: hand AirPlay video (HLS and direct URLs) to an external player process instead of the
   internal GStreamer player.  UxPlay connects to a Unix socket where the external process listens, and
   exchanges newline-delimited JSON messages with it (protocol version 1):

   UxPlay -> player:  hello, play, rate, scrub, stop, disconnect, volume
   player -> UxPlay:  hello, status (answers GET /playback-info), ended, failed

   Example:  {"v":1,"type":"play","session":"<uuid>","location":"https://...","start":0,"client":"","kind":"url"}  */

#ifndef HLS_FORWARD_H
#define HLS_FORWARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include "../lib/logger.h"

typedef struct hls_forward_status_s {
    bool ready;          /* the player has the video loaded (playing or paused) */
    bool finished;       /* the player reported "ended" or "failed" */
    double duration;     /* seconds, 0 while unknown */
    double position;     /* seconds */
    double seek_start;   /* seekable range, seconds */
    double seek_end;
    float rate;          /* 0 paused, 1 playing */
} hls_forward_status_t;

/* start connecting to the player's socket (retried every second while not connected); 0 on success */
int hls_forward_init(logger_t *logger, const char *socket_path, const char *version);
void hls_forward_destroy(void);
bool hls_forward_connected(void);

/* each returns false if the message could not be sent (no player connected); a play that was not sent
   is reported as finished by hls_forward_get_status, so the client resumes the video itself */
bool hls_forward_play(const char *session, const char *location, float start_position,
                      const char *client, const char *kind);
bool hls_forward_rate(float rate);
bool hls_forward_scrub(float position);
bool hls_forward_stop(const char *reason);   /* reason: "stop" or "playlistRemove"; ends the session */
/* the client went away without stopping; ends the session.  reason: "closed" (connections closed: it
   switched receivers or took the video back), "lost" (it stopped responding), "replaced" (another client) */
bool hls_forward_disconnect(const char *reason);
bool hls_forward_volume(double volume, double db);  /* 0.0 (mute) to 1.0, and the AirPlay dB value */

/* latest state of the current session from the player; false if there is no current session */
bool hls_forward_get_status(hls_forward_status_t *status);

#ifdef __cplusplus
}
#endif

#endif //HLS_FORWARD_H
