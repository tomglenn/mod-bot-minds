#ifndef MOD_BOT_MINDS_TRANSCRIPT_H
#define MOD_BOT_MINDS_TRANSCRIPT_H

#include <cstdint>
#include <string>

class Player;

// --------------------------------------------
// Where a line was spoken. One transcript is kept per scope instance, so party
// talk, guild talk and local say never bleed into each other.
// --------------------------------------------
enum class ChatScope : uint8_t
{
    Say = 0,
    Party,
    Guild,
    Channel,
    Whisper
};

struct ScopeKey
{
    ChatScope scope = ChatScope::Say;
    uint32_t  id    = 0;   // zone for Say, group/guild/channel id, human's guid for Whisper

    bool operator==(const ScopeKey& other) const { return scope == other.scope && id == other.id; }
};

// The bot a particular person is in conversation with, and when it last answered
// them (epoch seconds). guid == 0 means nobody holds the floor.
//
// Deliberately not "the last bot to speak here": ambient remarks and event
// reactions from bots across the zone would steal the floor, and your follow-up
// would go to a bot that was never talking to you.
struct FloorHolder
{
    uint64_t guid  = 0;
    uint32_t atSec = 0;
};

// Build the key for a scope. `counterpart` is only needed for Whisper, where it
// identifies the other end of the thread; `channelId` only for Channel.
ScopeKey MakeScope(ChatScope scope, Player* actor, Player* counterpart = nullptr, uint32_t channelId = 0);

// Human-readable name of a scope, for logs.
const char* ScopeName(ChatScope scope);

// Record a spoken line.
void RecordChatLine(const ScopeKey& key, uint64_t speakerGuid, const std::string& speakerName,
                    const std::string& text);

// Newest-last "Name: text" lines for the scope, capped by g_TranscriptLines and
// g_TranscriptMaxChars. Empty when nothing has been said there. A trailing line
// matching `excludeTrailing` is left out, so the message a bot is answering is
// not shown to it twice.
std::string RenderTranscript(const ScopeKey& key, const std::string& excludeTrailing = "");

// Note that `botGuid` just answered `humanGuid` here, giving it the floor.
void SetConversationFloor(const ScopeKey& key, uint64_t humanGuid, uint64_t botGuid);

// Which bot this person is currently talking to in this scope, if any.
FloorHolder GetConversationFloor(const ScopeKey& key, uint64_t humanGuid);

// Drop transcripts that have seen no traffic for a while. Called periodically so
// long sessions do not accumulate dead scopes.
void PruneTranscripts();

#endif // MOD_BOT_MINDS_TRANSCRIPT_H
