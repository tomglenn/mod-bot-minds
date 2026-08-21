#include "mod-bot-minds_speak.h"
#include "mod-bot-minds_action.h"
#include "mod-bot-minds_attention.h"
#include "mod-bot-minds_config.h"
#include "mod-bot-minds_governor.h"
#include "mod-bot-minds_llmclient.h"
#include "mod-bot-minds_memory.h"
#include "mod-bot-minds_relationship.h"
#include "mod-bot-minds_transcript.h"

#include "Channel.h"
#include "ChannelMgr.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"

#include <cctype>
#include <chrono>
#include <string>
#include <thread>

namespace
{
    // Strip a leading "Name:" the model sometimes prepends despite the rules.
    void StripNamePrefix(std::string& reply, const std::string& name)
    {
        if (reply.size() <= name.size() + 1)
            return;
        if (reply.compare(0, name.size(), name) != 0 || reply[name.size()] != ':')
            return;

        reply = reply.substr(name.size() + 1);
        size_t start = reply.find_first_not_of(' ');
        reply = (start == std::string::npos) ? "" : reply.substr(start);
    }

    // Only speak out loud where a person can actually hear it. Bots talking to an
    // empty field costs calls and fills the say transcript with lines the player
    // never heard.
    bool RealPlayerWithinSayRange(Player* bot)
    {
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* other = pair.second;
            if (!other || other == bot || !other->IsInWorld())
                continue;
            if (PlayerbotsMgr::instance().GetPlayerbotAI(other))
                continue;
            if (bot->GetMapId() == other->GetMapId() && bot->GetDistance(other) <= g_SayDistance)
                return true;
        }
        return false;
    }

    // Put the line in the right channel. Returns false if it could not be spoken.
    bool SpeakInScope(Player* bot, PlayerbotAI* botAI, const std::string& reply,
                      ChatScope scope, const std::string& channelName, const std::string& whisperTarget)
    {
        switch (scope)
        {
            case ChatScope::Say:
                if (!RealPlayerWithinSayRange(bot))
                    return false;
                botAI->Say(reply);
                return true;

            case ChatScope::Party:
                if (!bot->GetGroup())
                    return false;
                if (bot->GetGroup()->isRaidGroup())
                    botAI->SayToRaid(reply);
                else
                    botAI->SayToParty(reply);
                return true;

            case ChatScope::Guild:
                if (!bot->GetGuild())
                    return false;
                botAI->SayToGuild(reply);
                return true;

            case ChatScope::Whisper:
                if (whisperTarget.empty())
                    return false;
                botAI->Whisper(reply, whisperTarget);
                return true;

            case ChatScope::Channel:
            {
                if (channelName.find("General") != std::string::npos)
                    return botAI->SayToChannel(reply, ChatChannelId::GENERAL);

                ChannelMgr* manager = ChannelMgr::forTeam(bot->GetTeamId());
                if (!manager)
                    return false;

                Channel* channel = manager->GetChannel(channelName, bot);
                if (!channel || !bot->IsInChannel(channel))
                    return false;

                channel->Say(bot->GetGUID(), reply, LANG_UNIVERSAL);
                return true;
            }
        }

        return false;
    }

    // Releases the governor's concurrency slot exactly once, however the turn ends.
    struct SlotGuard
    {
        ~SlotGuard() { BotMindsGovernor::OnComplete(); }
    };
}

bool RequestBotTurn(TurnRequest& request, bool forced)
{
    if (!g_Enable || !request.bot)
        return false;

    if (g_DisableRepliesInCombat && request.bot->IsInCombat())
        return false;

    // Check audibility before spending a call, not just before speaking: a say
    // nobody is close enough to hear gets dropped on arrival, and paying for a
    // line that never lands is the one waste that is entirely avoidable.
    if (request.key.scope == ChatScope::Say && !RealPlayerWithinSayRange(request.bot))
        return false;

    if (!BotMindsGovernor::Allow(request.bot, request.other, request.key.scope, forced))
        return false;

    TurnPrompt prompt = BuildTurnPrompt(request);
    if (prompt.system.empty())
        return false;

    const uint64_t    botGuid       = request.bot->GetGUID().GetRawValue();
    const uint64_t    otherGuid     = request.other ? request.other->GetGUID().GetRawValue() : 0;
    const bool        otherIsBot    = request.other && PlayerbotsMgr::instance().GetPlayerbotAI(request.other) != nullptr;
    const std::string botName       = request.bot->GetName();
    const std::string whisperTarget = request.other ? request.other->GetName() : "";
    const ScopeKey    key           = request.key;
    const std::string channelName   = request.channelName;
    const uint32_t    chainDepth    = request.chainDepth;
    const TurnKind    kind          = request.kind;
    const ActionMenu  menu          = request.menu;

    BotMindsGovernor::OnSubmit(botGuid);

    ILLMProvider* provider = GetProvider();

    std::thread([=, system = std::move(prompt.system), user = std::move(prompt.user)]()
    {
        SlotGuard slot;

        try
        {
            LLMResult result = provider->Complete(system, user);

            // A bot that owes someone an answer speaks as long as it produced
            // words, even if it also set should_reply false. Only turns it was
            // merely offered are allowed to decline.
            const bool declined = !result.shouldReply && !forced;

            if (!result.ok || declined || result.reply.empty())
            {
                if (g_DebugEnabled)
                    LOG_INFO("server.loading", "[BotMinds] {} stayed silent ({}).",
                             botName, result.ok ? "chose not to reply" : "no usable response");
                return;
            }

            std::string reply = std::move(result.reply);
            StripNamePrefix(reply, botName);
            if (reply.empty())
                return;

            if (g_EnableTypingSimulation)
            {
                uint32_t delay = g_TypingSimulationBaseDelay + (reply.length() * g_TypingSimulationDelayPerChar);
                std::this_thread::sleep_for(std::chrono::milliseconds(delay));
            }

            Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(botGuid));
            if (!bot || !bot->IsInWorld())
                return;

            PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
            if (!botAI)
                return;

            if (!SpeakInScope(bot, botAI, reply, key.scope, channelName, whisperTarget))
            {
                if (g_DebugEnabled)
                    LOG_INFO("server.loading", "[BotMinds] {} could not speak in {}; line dropped.",
                             botName, ScopeName(key.scope));
                return;
            }

            if (g_DebugEnabled)
                LOG_INFO("server.loading", "[BotMinds] {} ({} in {}): {}",
                         botName, static_cast<int>(kind), ScopeName(key.scope), reply);

            RecordChatLine(key, botGuid, botName, reply);

            // Answering a person puts this bot in conversation with them, so their
            // next message comes back here rather than to whoever spoke last.
            if (!otherIsBot && otherGuid != 0
                && (kind == TurnKind::DirectReply || kind == TurnKind::Interjection))
            {
                SetConversationFloor(key, otherGuid, botGuid);
            }

            // Idle remarks are not worth remembering; they would crowd out the
            // memories that matter.
            if (kind != TurnKind::Ambient && result.memory_additions.is_array())
            {
                for (const auto& entry : result.memory_additions)
                {
                    std::string text = entry.value("text", "");
                    if (text.empty())
                        continue;
                    AddMemory(botGuid, otherGuid, entry.value("kind", "event"), text,
                              entry.value("salience", 0.5f));
                }
            }

            if (otherGuid != 0 && result.relationship_delta.is_object())
            {
                ApplyRelationshipDelta(botGuid, otherGuid, otherIsBot,
                                       result.relationship_delta.value("affinity_change", 0.0f),
                                       result.relationship_delta.value("reason", std::string()));
            }

            // Say it first, then do it. That is the order a person would use, and it
            // means the words are already out if the action needs a retry or two.
            if (result.action.is_object())
            {
                BotAction action;
                action.botGuid    = botGuid;
                action.targetGuid = otherGuid;
                action.kind       = ActionKindFromName(result.action.value("kind", std::string("none")));
                action.spellName  = result.action.value("spell", std::string());
                action.copper     = result.action.value("copper", 0u);
                action.promised    = (kind == TurnKind::DirectReply || kind == TurnKind::Interjection);

                // Did the bot already say it was posting the money? Checked here
                // because this is where the spoken words are.
                std::string lowered = reply;
                for (char& c : lowered)
                    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

                for (const char* hint : { "mail", "post", "inbox", "sent it", "send it" })
                {
                    if (lowered.find(hint) != std::string::npos)
                    {
                        action.mentionedPost = true;
                        break;
                    }
                }

                if (action.kind != ActionKind::None && ValidateAction(menu, action))
                {
                    SubmitBotAction(action);
                }
                else if (g_DebugEnabled && action.kind != ActionKind::None)
                {
                    LOG_INFO("server.loading",
                             "[BotMinds] Dropped an action {} tried to take that was not on offer.", botName);
                }
            }

            // A bot speaking can draw a reply from another bot, up to the chain limit.
            OnLineSpoken(botGuid, botName, reply, key, channelName, chainDepth);
        }
        catch (const std::exception& ex)
        {
            LOG_ERROR("server.loading", "[BotMinds] Exception in bot turn thread: {}", ex.what());
        }
    }).detach();

    return true;
}
