#ifndef MOD_BOT_MINDS_ATTENTION_H
#define MOD_BOT_MINDS_ATTENTION_H

#include "mod-bot-minds_transcript.h"

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------
// Who answers, and who keeps quiet.
//
// A real conversation has a floor: if a bot asked you something and you answer,
// that bot is the one who should respond, not three bystanders. This is where
// that is worked out, and it is the only place reply probability lives.
// --------------------------------------------

// A human said something. Records it in the transcript and dispatches turns for
// whichever bots should answer.
void OnPlayerLine(Player* speaker, const std::string& text, ChatScope scope, const ScopeKey& key,
                  const std::string& channelName, Player* whisperTarget);

// A bot said something (already recorded in the transcript). Lets another bot
// pick it up, bounded by the chain depth so bots cannot talk forever.
void OnLineSpoken(uint64_t speakerGuid, const std::string& speakerName, const std::string& text,
                  const ScopeKey& key, const std::string& channelName, uint32_t chainDepth);

#endif // MOD_BOT_MINDS_ATTENTION_H
