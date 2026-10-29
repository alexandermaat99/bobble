#pragma once
#include <stdint.h>
#include "esp_http_server.h"

// Coding agents on the Mac (Claude Code hooks, via agent_hook.sh) report what they're doing:
// POST /agent {"session":"…","state":"working|waiting|done|idle","detail":"Editing main.c","project":"amoled_demo",
// "agent":"claude","model":"Opus 5.5","t":<ms>}, token-protected. detail can be a whole question ("Run git push origin main?").
// Several sessions are tracked; the eyes show the one that matters most (waiting > working > just done), or the one
// picked with a tap on the bubble (agent_select_next) for a while.
// (stopped: you interrupted it; its last message stays up, greyed, until the chat's closed or you send it something new)
typedef enum { AG_IDLE, AG_WORKING, AG_WAITING, AG_DONE, AG_STOPPED } agent_state_t;
#define AGENT_MAX 6
#define AGENT_OPTS 4        // answers offered on the display at most

typedef struct {
    agent_state_t state;
    char detail[200];       // "Editing main.c", "Run git push origin main?" (the hook keeps it under 200)
    char project[32];       // the folder the session runs in
    char title[48];         // the chat's title ("Access to other chats"), if Claude Code has named it
    char tool[24];          // the integration it's using right now ("linear", "slack"), or ""
    char agent[12];         // which integration: "claude" (later "codex", "cursor"...)
    char model[24];         // "Opus 5.5"
    char provider[16];      // who made the model ("claude", "openai", "googlegemini", "cursor"): the mark at the top
    int working;            // how many sessions are busy
    int n;                  // sessions with something to show (not idle), and their states in a steady order:
    agent_state_t dots[AGENT_MAX];
    char dot_agent[AGENT_MAX][12];   // each one's agent ("claude", "cursor", "codex"): its dot is that agent's mark
    uint8_t dot_fade[AGENT_MAX];
    uint8_t dot_done[AGENT_MAX];   // tenths of a second since it finished, while that's under 5 s (0: not just finished)   // how stale (0 in use .. 15 long finished or quiet): its logo greys and darkens
    int dot_slot[AGENT_MAX];   // each one's slot (0 .. AGENT_MAX-1): steady for the session, so it can keep a colour
    int dot_shown;          // which of those is the one above
    int slot;               // the shown session's slot
    int64_t since;          // esp_timer time the shown state began
    uint32_t attention;     // goes up each time a session starts waiting or finishes
    uint32_t activity;      // goes up with every new message (state or words changed): wakes the screen to show it
} agent_status_t;

// One chat with something to show, all of it (agent_get has the words of just the shown one): for the square board,
// which shows several at once. bubble: its words are still up (a long-finished one is just its logo).
typedef struct {
    agent_state_t state;
    int slot;               // as dot_slot: steady for the session (its colour)
    int64_t since;          // esp_timer time its state began
    bool bubble;
    char detail[200], title[48], project[32], agent[12], model[24];
    int nopt;               // waiting on a choice you can make right here ("Yes" / "No", a question's answers): how many
    char opt[AGENT_OPTS][32];
    int picked;             // the one tapped (-1: none yet), till the Mac moves the chat on
} agent_chat_t;

void agent_register(httpd_handle_t server);
void agent_get(agent_status_t *out);
int agent_get_chats(agent_chat_t out[AGENT_MAX]);   // every chat not idle, in slot order; how many
void agent_select_next(void);   // show the next session (a tap on the bubble); back to the most urgent after 30 s
void agent_select_slot(int slot);   // show this one (a tap on its dot), the same way
// A waiting chat can come with choices ("options": ["Yes", "No"], and "qid": which question they answer). A tap on
// one (agent_answer) is kept for the Mac, which collects it with POST /agent/answers {"sessions": [...]} and answers
// the agent for you.
void agent_answer(int slot, int choice);

// Pairing with the Mac's app: POST /pair/start shows a 6-digit code on the display (for 2 minutes); the app sends it
// back with POST /pair/finish {"code"} and gets a key of its own ({"key"}), kept in NVS, for its X-OTA-Token header.
// (The development token keeps working too.)
bool agent_pairing(char code[8]);   // a code is up: copies it (code may be NULL)
void agent_pair_cancel(void);      // take the code down (a tap on it, or POST /pair/cancel from the app)
bool agent_just_paired(void);       // paired in the last few seconds (the display says so)
bool agent_known(void);
bool agent_token_ok(httpd_req_t *req);
bool agent_key_ok(const char *token);   // the same check on a key that came some other way (the chat connection)
const char *agent_device_id(void);     // this board: 12 hex digits from its hardware address
void agent_set_name(const char *name);   // printable ASCII, up to 31 ("": back to "Bobble 3F2A"); saved, and advertised
const char *agent_device_name(void);   // "Bobble 3F2A", or what it's been named in the app   // the request carries the paired Mac's key (or the development one)             // a Mac has paired, or sent a chat, at some point (else it's still being set up)
