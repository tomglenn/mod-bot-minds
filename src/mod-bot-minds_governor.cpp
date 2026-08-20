#include "mod-bot-minds_governor.h"
#include "mod-bot-minds_config.h"
#include "mod-bot-minds_llmclient.h"

#include "Player.h"
#include "Playerbots.h"

#include <atomic>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <unordered_map>

// --------------------------------------------
// Governor state
// --------------------------------------------
static std::atomic<int>      g_inFlight{0};
static std::atomic<uint32_t> g_callsThisInterval{0};
static std::atomic<uint32_t> g_callsTotal{0};
static uint32_t              g_intervalElapsedMs = 0;

static std::mutex                             g_lastCallMutex;
static std::unordered_map<uint64_t, uint32_t> g_lastCallSec; // bot guid -> last call (epoch seconds)

namespace
{
    inline bool IsBot(Player* p)
    {
        return p && GET_PLAYERBOT_AI(p);
    }

    // Say and channel chat are overheard, so the bot has to be close enough for
    // an answer to make sense. Party, guild and whisper carry any distance.
    bool ScopeNeedsProximity(ChatScope scope)
    {
        return scope == ChatScope::Say || scope == ChatScope::Channel;
    }
}

namespace BotMindsGovernor
{
    bool Allow(Player* bot, Player* addresser, ChatScope scope, bool forced)
    {
        if (GetProvider() == nullptr)
            return false;

        if (!IsBot(bot))
            return false;

        if (g_HardCapCallsPerInterval > 0 && g_callsThisInterval.load() >= g_HardCapCallsPerInterval)
        {
            if (g_DebugEnabled)
                LOG_INFO("server.loading", "[BotMinds] {} silent: hard cap of {} calls per {}s reached",
                         bot->GetName(), g_HardCapCallsPerInterval, g_HardCapIntervalSec);
            return false;
        }

        if (g_inFlight.load() >= static_cast<int>(g_MaxConcurrentCalls))
            return false;

        if (forced)
            return true;

        uint32_t now = static_cast<uint32_t>(time(nullptr));
        {
            std::lock_guard<std::mutex> lock(g_lastCallMutex);
            auto it = g_lastCallSec.find(bot->GetGUID().GetRawValue());
            if (it != g_lastCallSec.end() && (now - it->second) < g_PerBotCooldownSec)
                return false;
        }

        if (addresser && ScopeNeedsProximity(scope) && !bot->IsWithinDistInMap(addresser, g_ProximityRadius))
            return false;

        return true;
    }

    void OnSubmit(uint64_t botGuid)
    {
        uint32_t now = static_cast<uint32_t>(time(nullptr));
        {
            std::lock_guard<std::mutex> lock(g_lastCallMutex);
            g_lastCallSec[botGuid] = now;
        }
        ++g_inFlight;
        ++g_callsThisInterval;
        ++g_callsTotal;
    }

    void OnComplete()
    {
        --g_inFlight;
    }

    uint32_t CallsSinceStartup()
    {
        return g_callsTotal.load();
    }

    int CallsInFlight()
    {
        return g_inFlight.load();
    }

    void Tick(uint32_t diffMs)
    {
        g_intervalElapsedMs += diffMs;

        uint32_t windowMs = g_HardCapIntervalSec * 1000;
        if (windowMs != 0 && g_intervalElapsedMs >= windowMs)
        {
            g_callsThisInterval.store(0);
            g_intervalElapsedMs = 0;
        }
    }
}
