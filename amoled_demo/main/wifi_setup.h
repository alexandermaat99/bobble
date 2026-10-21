#pragma once
#include <stdbool.h>

typedef enum {
    WS_PORTAL,      // no working network: setup hotspot is up
    WS_CONNECTING,
    WS_CONNECTED,
} wifi_state_t;

void wifi_setup_start(void);
wifi_state_t wifi_state(void);
const char *wifi_ssid(void);     // network we're joined to / trying to join
const char *wifi_ip(void);
const char *wifi_ap_ssid(void);  // setup hotspot name
const char *wifi_ap_pass(void);
int wifi_ap_clients(void);       // phones currently joined to the setup hotspot
bool wifi_time_synced(void);
void wifi_forget(void);          // forget this network (others stay remembered) and go back to setup

typedef struct { char ssid[33]; int rssi; bool secure; } wifi_net_t;
void wifi_scan(void);            // starts a background scan
bool wifi_scanning(void);
int wifi_networks(wifi_net_t *out, int max);   // last scan, strongest first
void wifi_join(const char *ssid, const char *pass);
bool wifi_ap_active(void);
bool wifi_last_failed(void);     // the last join attempt gave up
void wifi_pause_retries(bool pause);   // stop background reconnects while the user is choosing
void wifi_suspend(bool on);      // radio fully off (locked) / back on
void wifi_set_name(const char *name);   // what the board calls itself on the network (its Bonjour name)
