#ifndef MOD_BOT_MINDS_GOVERNOR_H
#define MOD_BOT_MINDS_GOVERNOR_H

#include "mod-bot-minds_transcript.h"

#include <cstdint>

class Player;

// --------------------------------------------
// Resource limits on LLM calls: a usable provider, the rolling hard cap, the
// concurrency slots, the per-bot cooldown and proximity.
//
// Whether a bot *wants* to speak is decided in _attention.*; the governor only
// decides whether it *may*. Keeping probability out of here is deliberate: two
// independent chance rolls in two layers is what used to make bots answer at
// random.
// --------------------------------------------
namespace BotMindsGovernor
{
    // `forced` marks a line the bot owes someone: a direct answer to a player who
    // addressed it. Those skip the cooldown and the proximity check, but still
    // respect the hard cap and the concurrency limit.
    bool Allow(Player* bot, Player* addresser, ChatScope scope, bool forced);

    // Call immediately after Allow passes: sets the bot's cooldown, ++in-flight,
    // ++calls-this-interval.
    void OnSubmit(uint64_t botGuid);

    // Call when a call completes, fails or is abandoned: --in-flight. Always pair
    // with OnSubmit.
    void OnComplete();

    // Call from a WorldScript OnUpdate: advances the rolling hard-cap window.
    void Tick(uint32_t diffMs);

    // Calls submitted since startup, and how many are in flight right now, for
    // `.botminds status`. Every call costs money, so it should be countable.
    uint32_t CallsSinceStartup();
    int      CallsInFlight();
}

#endif // MOD_BOT_MINDS_GOVERNOR_H
