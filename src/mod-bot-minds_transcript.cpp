#include "mod-bot-minds_transcript.h"
#include "mod-bot-minds_config.h"

#include "Group.h"
#include "Player.h"

#include <ctime>
#include <deque>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace
{
    struct Line
    {
        std::string name;
        std::string text;
        uint32_t    atSec = 0;
    };

    struct Conversation
    {
        std::deque<Line> lines;
        uint32_t         lastActivitySec = 0;
    };

    struct ScopeKeyHash
    {
        size_t operator()(const ScopeKey& k) const
        {
            return (static_cast<size_t>(k.id) << 3) ^ static_cast<size_t>(k.scope);
        }
    };

    // One floor per (scope, person): who that person is talking to right now.
    struct FloorKey
    {
        ScopeKey scope;
        uint64_t humanGuid = 0;

        bool operator==(const FloorKey& other) const
        {
            return scope == other.scope && humanGuid == other.humanGuid;
        }
    };

    struct FloorKeyHash
    {
        size_t operator()(const FloorKey& k) const
        {
            return ScopeKeyHash()(k.scope) ^ (static_cast<size_t>(k.humanGuid) << 17);
        }
    };

    std::unordered_map<ScopeKey, Conversation, ScopeKeyHash> g_Conversations;
    std::unordered_map<FloorKey, FloorHolder, FloorKeyHash>  g_Floors;
    std::mutex                                              g_TranscriptMutex;

    uint32_t NowSec()
    {
        return static_cast<uint32_t>(time(nullptr));
    }
}

ScopeKey MakeScope(ChatScope scope, Player* actor, Player* counterpart, uint32_t channelId)
{
    ScopeKey key;
    key.scope = scope;

    switch (scope)
    {
        case ChatScope::Say:
            key.id = actor ? actor->GetZoneId() : 0;
            break;
        case ChatScope::Party:
            if (actor && actor->GetGroup())
                key.id = actor->GetGroup()->GetGUID().GetCounter();
            break;
        case ChatScope::Guild:
            key.id = actor ? actor->GetGuildId() : 0;
            break;
        case ChatScope::Channel:
            key.id = channelId;
            break;
        case ChatScope::Whisper:
            key.id = counterpart ? counterpart->GetGUID().GetCounter() : 0;
            break;
    }

    return key;
}

const char* ScopeName(ChatScope scope)
{
    switch (scope)
    {
        case ChatScope::Say:     return "Say";
        case ChatScope::Party:   return "Party";
        case ChatScope::Guild:   return "Guild";
        case ChatScope::Channel: return "Channel";
        case ChatScope::Whisper: return "Whisper";
    }
    return "Unknown";
}

void RecordChatLine(const ScopeKey& key, uint64_t /*speakerGuid*/, const std::string& speakerName,
                    const std::string& text)
{
    if (text.empty())
        return;

    uint32_t now = NowSec();

    std::lock_guard<std::mutex> lock(g_TranscriptMutex);

    Conversation& conv = g_Conversations[key];
    conv.lines.push_back({ speakerName, text, now });
    conv.lastActivitySec = now;

    size_t cap = g_TranscriptLines > 0 ? g_TranscriptLines : 8;
    while (conv.lines.size() > cap)
        conv.lines.pop_front();
}

std::string RenderTranscript(const ScopeKey& key, const std::string& excludeTrailing)
{
    std::lock_guard<std::mutex> lock(g_TranscriptMutex);

    auto it = g_Conversations.find(key);
    if (it == g_Conversations.end() || it->second.lines.empty())
        return "";

    const std::deque<Line>& lines = it->second.lines;

    // Build newest-first so the character cap drops the oldest lines, then flip.
    std::deque<std::string> rendered;
    size_t used = 0;
    size_t cap  = g_TranscriptMaxChars > 0 ? g_TranscriptMaxChars : 600;

    auto first = lines.rbegin();
    if (!excludeTrailing.empty() && first != lines.rend() && first->text == excludeTrailing)
        ++first;

    for (auto line = first; line != lines.rend(); ++line)
    {
        std::string entry = line->name + ": " + line->text + "\n";
        if (used + entry.size() > cap && !rendered.empty())
            break;
        used += entry.size();
        rendered.push_front(std::move(entry));
    }

    std::ostringstream out;
    for (const std::string& entry : rendered)
        out << entry;

    return out.str();
}

void SetConversationFloor(const ScopeKey& key, uint64_t humanGuid, uint64_t botGuid)
{
    if (humanGuid == 0 || botGuid == 0)
        return;

    std::lock_guard<std::mutex> lock(g_TranscriptMutex);

    FloorHolder& floor = g_Floors[FloorKey{ key, humanGuid }];
    floor.guid  = botGuid;
    floor.atSec = NowSec();
}

FloorHolder GetConversationFloor(const ScopeKey& key, uint64_t humanGuid)
{
    std::lock_guard<std::mutex> lock(g_TranscriptMutex);

    auto it = g_Floors.find(FloorKey{ key, humanGuid });
    if (it == g_Floors.end())
        return FloorHolder();

    return it->second;
}

void PruneTranscripts()
{
    uint32_t now = NowSec();
    uint32_t ttl = g_TranscriptTtlSec > 0 ? g_TranscriptTtlSec : 900;

    std::lock_guard<std::mutex> lock(g_TranscriptMutex);

    for (auto it = g_Conversations.begin(); it != g_Conversations.end(); )
    {
        if (now - it->second.lastActivitySec > ttl)
            it = g_Conversations.erase(it);
        else
            ++it;
    }

    for (auto it = g_Floors.begin(); it != g_Floors.end(); )
    {
        if (now - it->second.atSec > ttl)
            it = g_Floors.erase(it);
        else
            ++it;
    }
}
