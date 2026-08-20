#include "mod-bot-minds_handler.h"
#include "mod-bot-minds_attention.h"
#include "mod-bot-minds_config.h"
#include "mod-bot-minds_transcript.h"

#include "Channel.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "SharedDefines.h"

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

    // Playerbot commands typed in chat ("follow", "stay", addon traffic) are
    // instructions, not conversation.
    bool IsBotCommand(const std::string& msg)
    {
        const std::string trimmed = RightTrim(msg);

        for (const std::string& command : g_BlacklistCommands)
        {
            if (trimmed.size() < command.size())
                continue;
            if (trimmed.compare(0, command.size(), command) != 0)
                continue;
            if (trimmed.size() == command.size()
                || !std::isalnum(static_cast<unsigned char>(trimmed[command.size()])))
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
