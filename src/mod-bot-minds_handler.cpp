#include "mod-bot-minds_handler.h"
#include "mod-bot-minds_attention.h"
#include "mod-bot-minds_config.h"
#include "mod-bot-minds_transcript.h"

#include "Channel.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "SharedDefines.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace
{
    // Chat types we act on, folded into the scopes the module reasons about.
    // Yell shares the Say scope, so bots within say range answer a yell.
    bool ScopeForChatType(uint32_t type, ChatScope& scope)
    {
        switch (type)
        {
            case CHAT_MSG_SAY:
            case CHAT_MSG_YELL:
                scope = ChatScope::Say;
                return true;
            case CHAT_MSG_PARTY:
            case CHAT_MSG_PARTY_LEADER:
            case CHAT_MSG_RAID:
            case CHAT_MSG_RAID_LEADER:
            case CHAT_MSG_RAID_WARNING:
                scope = ChatScope::Party;
                return true;
            case CHAT_MSG_GUILD:
            case CHAT_MSG_OFFICER:
                scope = ChatScope::Guild;
                return true;
            case CHAT_MSG_WHISPER:
                scope = ChatScope::Whisper;
                return true;
            case CHAT_MSG_CHANNEL:
                scope = ChatScope::Channel;
                return true;
            default:
                return false;
        }
    }

    std::string RightTrim(const std::string& text)
    {
        size_t end = text.find_last_not_of(" \t\n\r");
        return (end == std::string::npos) ? "" : text.substr(0, end + 1);
    }

    // Words that only turn up when somebody is talking, not issuing an order.
    // "drop 12345" is a command; "drop me a heal" is a request, and the blacklist
    // used to swallow it because it starts with a command word.
    bool LooksConversational(const std::string& lowered)
    {
        static const char* markers[] = {
            " me", " my ", " you", " your", " us ", " our ", " please", " thanks", " cheers",
            "can ", "could ", "would ", "will you", "got any", "any chance", "?"
        };

        for (const char* marker : markers)
        {
            if (lowered.find(marker) != std::string::npos)
                return true;
        }

        return false;
    }

    // Addon chatter that leaks into a normal channel: one word, no spaces, with an
    // underscore or a digit in it. "ELVUI_VERSIONCHK", "BWVQ3", "GTFO_v".
    bool LooksLikeAddonTraffic(const std::string& text)
    {
        if (text.size() < 4 || text.find(' ') != std::string::npos)
            return false;

        return text.find('_') != std::string::npos
            || std::any_of(text.begin(), text.end(),
                           [](unsigned char c) { return std::isdigit(c) != 0; });
    }

    // Playerbot commands typed in chat ("follow", "stay", addon traffic) are
    // instructions, not conversation.
    bool IsBotCommand(const std::string& msg)
    {
        const std::string trimmed = RightTrim(msg);

        // When playerbots is configured with a command prefix it ignores everything
        // else, so anything unprefixed is ours to interpret, including a bare
        // "follow". Deferring to its own setting beats maintaining a word list that
        // disagrees with it.
        const std::string& prefix = sPlayerbotAIConfig.commandPrefix;
        if (!prefix.empty())
        {
            return trimmed.compare(0, prefix.size(), prefix) == 0
                || LooksLikeAddonTraffic(trimmed);
        }

        std::string lowered = trimmed;
        for (char& c : lowered)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        for (const std::string& command : g_BlacklistCommands)
        {
            if (trimmed.size() < command.size())
                continue;
            if (trimmed.compare(0, command.size(), command) != 0)
                continue;
            if (trimmed.size() != command.size()
                && std::isalnum(static_cast<unsigned char>(trimmed[command.size()])))
                continue;

            // An exact command is always a command. Anything longer is only one if
            // it does not read like somebody talking.
            if (trimmed.size() == command.size() || !LooksConversational(lowered))
                return true;
        }

        return false;
    }

    bool SenderIsBot(Player* player)
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(player);
        return ai && ai->IsBotAI();
    }

    // One route in: a human said something, so work out who should answer.
    // Lines spoken by bots arrive through OnLineSpoken instead, from the module's
    // own dispatch, so they are ignored here.
    void HandleChat(Player* player, uint32_t type, uint32_t lang, const std::string& msg,
                    Channel* channel, Player* receiver)
    {
        if (!g_Enable || !player || msg.empty() || lang == LANG_ADDON)
            return;

        ChatScope scope;
        if (!ScopeForChatType(type, scope))
            return;

        if (SenderIsBot(player))
            return;

        if (IsBotCommand(msg))
        {
            if (g_DebugEnabled)
                LOG_INFO("server.loading", "[BotMinds] Ignoring '{}' from {} (playerbot command).", msg, player->GetName());
            return;
        }

        const std::string channelName = channel ? channel->GetName() : "";
        const uint32_t    channelId   = channel ? channel->GetChannelId() : 0;

        Player* whisperTarget = (scope == ChatScope::Whisper) ? receiver : nullptr;
        if (scope == ChatScope::Whisper && !whisperTarget)
            return;

        ScopeKey key = MakeScope(scope, player, whisperTarget, channelId);

        if (g_DebugEnabled)
            LOG_INFO("server.loading", "[BotMinds] {} in {}: {}", player->GetName(), ScopeName(scope), msg);

        OnPlayerLine(player, msg, scope, key, channelName, whisperTarget);
    }
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg)
{
    HandleChat(player, type, lang, msg, nullptr, nullptr);
    return true;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Group* /*group*/)
{
    HandleChat(player, type, lang, msg, nullptr, nullptr);
    return true;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Guild* /*guild*/)
{
    HandleChat(player, type, lang, msg, nullptr, nullptr);
    return true;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel)
{
    HandleChat(player, type, lang, msg, channel, nullptr);
    return true;
}

bool PlayerBotChatHandler::OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Player* receiver)
{
    HandleChat(player, type, lang, msg, nullptr, receiver);
    return true;
}
