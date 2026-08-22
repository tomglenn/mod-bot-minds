#include "mod-bot-minds_llmclient.h"
#include "mod-bot-minds_config.h"
#include "mod-bot-minds_httpclient.h"

#include "Log.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <utility>

using nlohmann::json;

// --------------------------------------------
// bot_turn structured-output schema. Every line a bot speaks comes back through
// this tool: the words, whether to speak at all, what to remember, and how the
// exchange moved the relationship.
// --------------------------------------------
static const json& BotTurnSchema()
{
    static const json schema = json{
        {"type", "object"},
        {"properties", {
            {"should_reply", {
                {"type", "boolean"},
                {"description", "False to stay silent, for example when the message was meant for someone else."}
            }},
            {"reply", {
                {"type", "string"},
                {"description", "The words the bot says out loud. Empty when should_reply is false."}
            }},
            {"emote", {
                {"type", "string"},
                {"description", "Optional gesture to go with the line: wave, laugh, nod, shrug, thank, "
                                "cheer, salute, bow or sigh. Use these rarely. An occasional wave says "
                                "something; one attached to every greeting is noise, and most lines want "
                                "none at all."}
            }},
            {"memory_additions", {
                {"type", "array"},
                {"description", "Anything from this exchange worth remembering later."},
                {"items", {
                    {"type", "object"},
                    {"properties", {
                        {"kind", {
                            {"type", "string"},
                            {"enum", json::array({"event", "fact"})}
                        }},
                        {"text", {{"type", "string"}}},
                        {"salience", {{"type", "number"}}}
                    }},
                    {"required", json::array({"kind", "text", "salience"})}
                }}
            }},
            {"relationship_delta", {
                {"type", "object"},
                {"description", "How this exchange changes the bot's feelings toward the other person."},
                {"properties", {
                    {"affinity_change", {{"type", "number"}}},
                    {"reason", {{"type", "string"}}}
                }},
                {"required", json::array({"affinity_change", "reason"})}
            }},
            {"action", {
                {"type", "object"},
                {"description", "Something the bot actually does, alongside saying its line. Fill this "
                                "in whenever the list of what you can do covers what was asked: agreeing "
                                "in words without setting it means nothing happens in the game. Leave it "
                                "out only when you genuinely cannot do the thing."},
                {"properties", {
                    {"kind", {
                        {"type", "string"},
                        {"enum", json::array({"none", "buff", "heal", "give_gold", "follow", "stay"})}
                    }},
                    {"spell", {
                        {"type", "string"},
                        {"description", "For buff or heal: the exact name from the list you were offered."}
                    }},
                    {"copper", {
                        {"type", "integer"},
                        {"description", "For give_gold: the amount in copper, never more than you were told you would spare."}
                    }}
                }},
                {"required", json::array({"kind"})}
            }}
        }},
        {"required", json::array({"should_reply", "reply"})}
    };
    return schema;
}

static json BotTurnToolAnthropic()
{
    return json{
        {"name", "bot_turn"},
        {"description", "Record the bot's spoken reply along with any new memories and relationship changes."},
        {"input_schema", BotTurnSchema()}
    };
}

static json BotTurnToolOpenAI()
{
    // Same schema, plus a closed object so the model cannot invent fields.
    // `strict: true` is deliberately not set: OpenAI's strict mode requires every
    // property to be listed in `required`, and reshaping the schema for a path
    // nobody has been able to exercise would be guesswork.
    json parameters = BotTurnSchema();
    parameters["additionalProperties"] = false;

    return json{
        {"type", "function"},
        {"function", {
            {"name", "bot_turn"},
            {"description", "Record the bot's spoken reply along with any new memories and relationship changes."},
            {"parameters", std::move(parameters)}
        }}
    };
}

// --------------------------------------------
// Reply cleanup
// --------------------------------------------
static void TrimInPlace(std::string& text)
{
    size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos)
    {
        text.clear();
        return;
    }
    size_t end = text.find_last_not_of(" \t\r\n");
    text = text.substr(start, end - start + 1);
}

std::string SanitizeReply(std::string reply, size_t maxChars)
{
    // Models occasionally wrap the line in prose formatting despite the rules.
    for (char& c : reply)
    {
        if (c == '\n' || c == '\r' || c == '\t')
            c = ' ';
    }

    // Drop *emote* segments, which read as narration in chat.
    std::string stripped;
    stripped.reserve(reply.size());
    bool inEmote = false;
    for (char c : reply)
    {
        if (c == '*')
        {
            inEmote = !inEmote;
            continue;
        }
        if (!inEmote)
            stripped.push_back(c);
    }
    reply = std::move(stripped);

    TrimInPlace(reply);

    if (reply.size() >= 2)
    {
        const char front = reply.front();
        const char back  = reply.back();
        if ((front == '"' && back == '"') || (front == '\'' && back == '\''))
        {
            reply = reply.substr(1, reply.size() - 2);
            TrimInPlace(reply);
        }
    }

    // Collapse runs of spaces left behind by the substitutions above.
    std::string collapsed;
    collapsed.reserve(reply.size());
    bool lastWasSpace = false;
    for (char c : reply)
    {
        bool isSpace = (c == ' ');
        if (isSpace && lastWasSpace)
            continue;
        collapsed.push_back(c);
        lastWasSpace = isSpace;
    }
    reply = std::move(collapsed);

    if (maxChars == 0 || reply.size() <= maxChars)
        return reply;

    // Over budget: prefer the last sentence end, fall back to the last word
    // boundary, so the line never stops mid-word.
    std::string clipped = reply.substr(0, maxChars);

    size_t sentenceEnd = clipped.find_last_of(".!?");
    if (sentenceEnd != std::string::npos && sentenceEnd + 1 >= maxChars / 2)
    {
        clipped.resize(sentenceEnd + 1);
        return clipped;
    }

    size_t wordEnd = clipped.find_last_of(' ');
    if (wordEnd != std::string::npos && wordEnd >= maxChars / 2)
        clipped.resize(wordEnd);

    TrimInPlace(clipped);
    return clipped;
}

static float ClampFloat(float value, float lo, float hi)
{
    if (value < lo) return lo;
    if (value > hi) return hi;
    return value;
}

static void NormalizeResult(LLMResult& result)
{
    result.reply = SanitizeReply(std::move(result.reply), g_MaxReplyChars);

    if (result.memory_additions.is_array())
    {
        for (auto& item : result.memory_additions)
        {
            if (item.is_object() && item.contains("salience") && item["salience"].is_number())
                item["salience"] = ClampFloat(item["salience"].get<float>(), 0.0f, 1.0f);
        }
    }

    if (result.relationship_delta.is_object()
        && result.relationship_delta.contains("affinity_change")
        && result.relationship_delta["affinity_change"].is_number())
    {
        result.relationship_delta["affinity_change"] =
            ClampFloat(result.relationship_delta["affinity_change"].get<float>(), -1.0f, 1.0f);
    }
}

static void FillFromToolInput(LLMResult& result, const json& input)
{
    if (input.contains("reply") && input["reply"].is_string())
        result.reply = input["reply"].get<std::string>();

    if (input.contains("should_reply") && input["should_reply"].is_boolean())
        result.shouldReply = input["should_reply"].get<bool>();

    if (input.contains("emote") && input["emote"].is_string())
        result.emote = input["emote"].get<std::string>();

    if (input.contains("memory_additions") && input["memory_additions"].is_array())
        result.memory_additions = input["memory_additions"];

    if (input.contains("relationship_delta") && input["relationship_delta"].is_object())
        result.relationship_delta = input["relationship_delta"];

    if (input.contains("action") && input["action"].is_object())
        result.action = input["action"];
}

// --------------------------------------------
// AnthropicProvider: POST https://api.anthropic.com/v1/messages
// --------------------------------------------
class AnthropicProvider : public ILLMProvider
{
public:
    LLMResult Complete(const std::string& systemPrompt, const std::string& userPrompt) override
    {
        LLMResult result;

        json body = {
            {"model", g_CloudModel},
            {"max_tokens", g_CloudMaxTokens},
            {"system", systemPrompt},
            {"messages", json::array({
                json{{"role", "user"}, {"content", userPrompt}}
            })},
            {"tools", json::array({ BotTurnToolAnthropic() })},
            {"tool_choice", json{{"type", "tool"}, {"name", "bot_turn"}}}
        };

        std::vector<std::pair<std::string, std::string>> headers = {
            {"x-api-key", g_CloudApiKey},
            {"anthropic-version", "2023-06-01"},
            {"content-type", "application/json"}
        };

        static BotMindsHttpClient httpClient;
        httpClient.SetTimeout(static_cast<int>(g_CloudTimeoutSec));

        std::string responseBody = httpClient.PostSecure(
            "api.anthropic.com", "/v1/messages", body.dump(), headers);

        if (responseBody.empty())
            return result;

        json response;
        try
        {
            response = json::parse(responseBody);
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("server.loading", "[BotMinds] Anthropic response parse failed: {}", e.what());
            return result;
        }

        std::string fallbackText;
        bool filledFromTool = false;

        if (response.contains("content") && response["content"].is_array())
        {
            for (const auto& block : response["content"])
            {
                if (!block.is_object() || !block.contains("type"))
                    continue;

                const std::string type = block["type"].get<std::string>();

                if (type == "tool_use"
                    && block.contains("name") && block["name"].is_string()
                    && block["name"].get<std::string>() == "bot_turn"
                    && block.contains("input") && block["input"].is_object())
                {
                    try
                    {
                        FillFromToolInput(result, block["input"]);
                        filledFromTool = true;
                    }
                    catch (const std::exception& e)
                    {
                        LOG_ERROR("server.loading", "[BotMinds] Anthropic tool input parse failed: {}", e.what());
                    }
                    break;
                }

                if (type == "text" && fallbackText.empty()
                    && block.contains("text") && block["text"].is_string())
                {
                    fallbackText = block["text"].get<std::string>();
                }
            }
        }

        if (!filledFromTool && !fallbackText.empty())
        {
            result.reply = fallbackText;
            result.memory_additions = json::array();
            result.relationship_delta = nullptr;
            result.action = nullptr;
        }

        result.ok = filledFromTool || !result.reply.empty();
        NormalizeResult(result);
        return result;
    }
};

// --------------------------------------------
// OpenAIProvider: POST https://api.openai.com/v1/chat/completions
//
// UNTESTED. The shape matches OpenAI's documented API and the code compiles and
// is wired to BotMinds.Provider = "openai", but no call has ever been made
// through it. Treat any failure here as a bug to be found, not as a limit of
// what the module can do.
// --------------------------------------------
class OpenAIProvider : public ILLMProvider
{
public:
    LLMResult Complete(const std::string& systemPrompt, const std::string& userPrompt) override
    {
        LLMResult result;

        std::vector<std::pair<std::string, std::string>> headers = {
            {"Authorization", "Bearer " + g_CloudApiKey},
            {"content-type", "application/json"}
        };

        static BotMindsHttpClient httpClient;
        httpClient.SetTimeout(static_cast<int>(g_CloudTimeoutSec));

        // Current models take max_completion_tokens; older ones only understand
        // max_tokens and reject the new name. Try the current spelling, and on a
        // rejection (not a network failure) try once with the old one.
        int status = 0;
        std::string responseBody = Post(httpClient, headers, systemPrompt, userPrompt,
                                       "max_completion_tokens", &status);

        if (responseBody.empty() && status >= 400 && status < 500)
        {
            LOG_INFO("server.loading",
                     "[BotMinds] OpenAI rejected max_completion_tokens (status {}); retrying with max_tokens.",
                     status);
            responseBody = Post(httpClient, headers, systemPrompt, userPrompt, "max_tokens", &status);
        }

        if (responseBody.empty())
            return result;

        json response;
        try
        {
            response = json::parse(responseBody);
        }
        catch (const std::exception& e)
        {
            LOG_ERROR("server.loading", "[BotMinds] OpenAI response parse failed: {}", e.what());
            return result;
        }

        if (!response.contains("choices") || !response["choices"].is_array() || response["choices"].empty())
            return result;

        const json& message = response["choices"][0].contains("message")
            ? response["choices"][0]["message"] : json::object();

        bool filledFromTool = false;
        if (message.contains("tool_calls") && message["tool_calls"].is_array() && !message["tool_calls"].empty())
        {
            const json& call = message["tool_calls"][0];
            if (call.is_object() && call.contains("function") && call["function"].is_object()
                && call["function"].contains("arguments") && call["function"]["arguments"].is_string())
            {
                try
                {
                    json input = json::parse(call["function"]["arguments"].get<std::string>());
                    if (input.is_object())
                    {
                        FillFromToolInput(result, input);
                        filledFromTool = true;
                    }
                }
                catch (const std::exception& e)
                {
                    LOG_ERROR("server.loading", "[BotMinds] OpenAI tool arguments parse failed: {}", e.what());
                }
            }
        }

        if (!filledFromTool && message.contains("content") && message["content"].is_string())
        {
            std::string content = message["content"].get<std::string>();
            if (!content.empty())
            {
                result.reply = content;
                result.memory_additions = json::array();
                result.relationship_delta = nullptr;
                result.action = nullptr;
            }
        }

        result.ok = filledFromTool || !result.reply.empty();
        NormalizeResult(result);
        return result;
    }

private:
    // One request, with the token limit under the given parameter name.
    static std::string Post(BotMindsHttpClient& httpClient,
                            const std::vector<std::pair<std::string, std::string>>& headers,
                            const std::string& systemPrompt,
                            const std::string& userPrompt,
                            const char* tokenLimitParam,
                            int* status)
    {
        json body = {
            {"model", g_CloudModel},
            {"messages", json::array({
                json{{"role", "system"}, {"content", systemPrompt}},
                json{{"role", "user"}, {"content", userPrompt}}
            })},
            {"tools", json::array({ BotTurnToolOpenAI() })},
            {"tool_choice", json{{"type", "function"}, {"function", {{"name", "bot_turn"}}}}}
        };

        body[tokenLimitParam] = g_CloudMaxTokens;

        return httpClient.PostSecure("api.openai.com", "/v1/chat/completions", body.dump(), headers, status);
    }
};

// --------------------------------------------
// Provider singleton
// --------------------------------------------
static std::unique_ptr<ILLMProvider> s_provider;

void InitLLMProviders()
{
    s_provider.reset();

    if (g_CloudApiKey.empty())
    {
        LOG_INFO("server.loading",
            "[BotMinds] Disabled: BotMinds.ApiKey is not set. Bots will not chat until it is.");
        return;
    }

    if (g_CloudProvider == "anthropic")
    {
        s_provider = std::make_unique<AnthropicProvider>();
        LOG_INFO("server.loading", "[BotMinds] Provider: Anthropic (model: {}).", g_CloudModel);
    }
    else if (g_CloudProvider == "openai")
    {
        s_provider = std::make_unique<OpenAIProvider>();
        LOG_INFO("server.loading",
                 "[BotMinds] Provider: OpenAI (model: {}). This path is untested; if bots stay "
                 "silent, check the server log for the API's response.", g_CloudModel);
    }
    else
    {
        LOG_ERROR("server.loading",
            "[BotMinds] Unknown provider '{}' (expected anthropic or openai); bots will not chat.",
            g_CloudProvider);
    }
}

ILLMProvider* GetProvider()
{
    return s_provider.get();
}
