#pragma once
#include <stdint.h>
#include <stdbool.h>

// The square board's chat page (left of the chats): talk with Claude. A mic button in the middle starts a chat, and
// in one mutes and unmutes you; the speaker button beside it mutes Claude, and held, opens a menu to pick the model or
// start a new chat; the red X ends the chat. What you said and what Claude is saying show above.
void chat_ui_update(int tx, int ty, int64_t now_us);   // touch in (x = -1 when up)
void chat_ui_cancel_touch(void);                       // a swipe took the finger
void chat_ui_leave(void);                              // the page went away: the chat ends with it
void chat_ui_draw(uint16_t *buf, int y0, int h);
bool chat_ui_dirty(void);                              // something changed since the last frame
// Call when a dirty frame is about to be drawn: 1 and the area ({x0, y0, x1, y1}) if only the glow round the mic moved,
// 0 for the whole page
int chat_ui_take_rect(int r[4]);
bool chat_ui_animating(void);                          // a chat is on: the button breathes with the sound
