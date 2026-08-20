#ifndef MOD_BOT_MINDS_PROMPT_H
#define MOD_BOT_MINDS_PROMPT_H

#include "mod-bot-minds_transcript.h"

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------
// Why a bot is about to speak. This is the only thing that varies between
// prompts: identity, world state, memory, transcript and the voice rules are
// identical for every kind of line a bot says.
// --------------------------------------------
enum class TurnKind : uint8_t
{
    DirectReply,   // addressed directly; answer it
    Interjection,  // someone else was addressed; speak only if it fits
    Ambient,       // unprompted remark about the bot's surroundings
    Event          // reaction to something that just happened
};

struct TurnRequest
{
    Player*     bot   = nullptr;
    Player*     other = nullptr;      // who is being answered, or the event's actor
    TurnKind    kind  = TurnKind::DirectReply;
    ScopeKey    key;                  // which conversation this line belongs to
    std::string trigger;              // the message being answered, or the situation
    std::string channelName;          // only for ChatScope::Channel
    uint32_t    chainDepth = 0;       // bot-to-bot hops so far; 0 for anything a human started
};

struct TurnPrompt
{
    std::string system;
    std::string user;
};

// Build the prompt for a turn. Every line a bot speaks comes through here, so
// the module has exactly one voice.
TurnPrompt BuildTurnPrompt(const TurnRequest& request);

#endif // MOD_BOT_MINDS_PROMPT_H
