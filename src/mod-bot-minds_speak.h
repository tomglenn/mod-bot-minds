#ifndef MOD_BOT_MINDS_SPEAK_H
#define MOD_BOT_MINDS_SPEAK_H

#include "mod-bot-minds_prompt.h"

// --------------------------------------------
// The one path from "a bot has something to say" to words in chat.
//
// Direct replies, interjections, ambient chatter and event reactions all come
// through here: same gate, same prompt builder, same provider, same memory
// write-back. There is deliberately no second route.
// --------------------------------------------

// `forced` marks a line the bot owes the person who addressed it: it skips the
// per-bot cooldown and the proximity check. Returns true if a call was
// submitted, false if the bot stays silent.
bool RequestBotTurn(const TurnRequest& request, bool forced);

#endif // MOD_BOT_MINDS_SPEAK_H
