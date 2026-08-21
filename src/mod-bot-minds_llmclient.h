#ifndef MOD_BOT_MINDS_LLMCLIENT_H
#define MOD_BOT_MINDS_LLMCLIENT_H

#include <string>
#include <cstdint>
#include <nlohmann/json.hpp>

// Result of one bot_turn call.
struct LLMResult
{
    bool        ok = false;              // a usable response came back
    bool        shouldReply = true;      // the bot chose to speak
    std::string reply;
    nlohmann::json memory_additions = nlohmann::json::array();   // [{kind,text,salience}]
    nlohmann::json relationship_delta = nullptr;                 // {affinity_change, reason} or null
    nlohmann::json action = nullptr;                             // {kind, spell, copper} or null
};

class ILLMProvider
{
public:
    virtual ~ILLMProvider() = default;
    virtual LLMResult Complete(const std::string& systemPrompt, const std::string& userPrompt) = 0;
};

// Trim and clean a raw model reply: strip quoting, emotes and newlines, then cut
// to `maxChars` on a sentence or word boundary rather than mid-word.
std::string SanitizeReply(std::string reply, size_t maxChars);

void InitLLMProviders();      // build the provider from config; call after config load
ILLMProvider* GetProvider();  // null if unusable, in which case bots stay silent

#endif // MOD_BOT_MINDS_LLMCLIENT_H
