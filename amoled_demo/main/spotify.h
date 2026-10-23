#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_http_server.h"

// Spotify Web API: what's playing + playback control. Sign-in happens once from the Mac
// (spotify_auth.py), which posts a refresh token to the board; it's kept in NVS.

typedef struct {
    bool configured;            // has a Spotify sign-in
    bool active;                // a device is playing or paused on this account
    bool playing;
    char title[96], artist[64], device[48];
    int volume;                 // 0..100, or -1 if the device doesn't allow it
    int progress_ms, duration_ms;
    int64_t fetched_us;         // when progress_ms was measured
    char note[48];              // last problem worth showing ("Needs Spotify Premium", ...)
    bool shuffle;
    bool episode;               // a podcast episode (not a song) is playing
    char context_uri[96];       // playlist/album being played, if any
} spotify_state_t;

enum { SP_PLAY_PAUSE, SP_NEXT, SP_PREV, SP_VOL_UP, SP_VOL_DOWN, SP_SHUFFLE, SP_PLAY_CONTEXT, SP_TRANSFER, SP_LOAD_LIBRARY,
       SP_LOAD_EPISODES, SP_PLAY_EPISODE, SP_SEEK_BACK, SP_SEEK_FWD, SP_POLL };   // SP_POLL: just look again   // seeks jump 10 s

// Your library, for the Library page
typedef struct { char name[64]; char uri[96]; int tracks; } sp_playlist_t;
typedef struct { char id[64]; char name[48]; char type[20]; bool active; } sp_device_t;
typedef struct { char id[32]; char name[64]; char publisher[48]; } sp_show_t;
typedef struct { char id[32]; char name[96]; char date[12]; int duration_ms, resume_ms; bool played; } sp_episode_t;

void spotify_start(void);
void spotify_register(httpd_handle_t server);    // POST /spotify (sign-in hand-off)
void spotify_get(spotify_state_t *out);
void spotify_command(int cmd);
void spotify_set_awake(bool on);        // screen on/off: poll slowly while off
void spotify_command_arg(int cmd, const char *arg);       // SP_PLAY_CONTEXT (uri) / SP_TRANSFER (device id)
int spotify_playlists(sp_playlist_t *out, int max);       // Liked Songs first, then your playlists
int spotify_devices(sp_device_t *out, int max);
int spotify_shows(sp_show_t *out, int max);              // podcasts you follow; -1 until loaded
int spotify_episodes(sp_episode_t *out, int max, char *show_id, int id_len);   // latest episodes of the last show asked for
// SP_LOAD_EPISODES takes a show id; SP_PLAY_EPISODE takes "<show id> <episode id> <position ms>"
uint32_t spotify_library_version(void);                  // changes when either list does
void spotify_set_library_visible(bool visible);           // keep the lists fresh while the page is up
void spotify_set_poll_fast(bool fast);           // 1 s polling while the Now Playing screen is up
// Album art scaled to the 466x466 screen and dimmed for a background (the square: a full-screen frame with the
// cover as a rounded tile at ART_X/ART_Y, ART_SZ px, black around it); NULL if none.
// The version changes whenever the picture does.
#define ART_SZ 256
#define ART_X ((480 - ART_SZ) / 2)
#define ART_Y 46
const uint16_t *spotify_art(uint32_t *version);
