#include "mod-bot-minds_attention.h"
#include "mod-bot-minds_config.h"
#include "mod-bot-minds_speak.h"

#include "Channel.h"
#include "ChannelMgr.h"
#include "Group.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Random.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <string>
#include <vector>

namespace
{
    bool IsBot(Player* player)
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(player);
        return ai && ai->IsBotAI();
    }

    std::string ToLower(const std::string& text)
    {
        std::string lowered = text;
        for (char& c : lowered)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return lowered;
    }

    bool ContainsWord(const std::string& loweredText, const std::string& word)
    {
        size_t pos = 0;
        while ((pos = loweredText.find(word, pos)) != std::string::npos)
        {
            bool startsClean = (pos == 0) || !std::isalnum(static_cast<unsigned char>(loweredText[pos - 1]));
            size_t end = pos + word.size();
            bool endsClean = (end >= loweredText.size()) || !std::isalnum(static_cast<unsigned char>(loweredText[end]));
            if (startsClean && endsClean)
                return true;
            ++pos;
        }
        return false;
    }

    // Position of a bot's name in the message, or npos. Word-bounded so "Al" does
    // not match "always".
    size_t NameMentionPos(const std::string& loweredText, const std::string& botName)
    {
        std::string loweredName = ToLower(botName);
        size_t pos = 0;
        while ((pos = loweredText.find(loweredName, pos)) != std::string::npos)
        {
            bool startsClean = (pos == 0) || !std::isalnum(static_cast<unsigned char>(loweredText[pos - 1]));
            size_t end = pos + loweredName.size();
            bool endsClean = (end >= loweredText.size()) || !std::isalnum(static_cast<unsigned char>(loweredText[end]));
            if (startsClean && endsClean)
                return pos;
            ++pos;
        }
        return std::string::npos;
    }

    // Is this aimed at the room rather than at one person? Greetings and plural
    // address mean several bots answering is natural; anything else is treated as
    // a reply to whoever spoke last.
    bool LooksLikeBroadcast(const std::string& loweredText)
    {
        static const char* words[] = {
            "hi", "hey", "hello", "greetings", "howdy", "yo", "sup",
            "everyone", "everybody", "anyone", "anybody", "yall",
            "folks", "guys", "lads", "people", "team", "guildies", "friends"
        };

        for (const char* word : words)
        {
            if (ContainsWord(loweredText, word))
                return true;
        }

        static const char* phrases[] = {
            "y'all", "you all", "you two", "you lot", "who wants", "who needs",
            "does any", "is any", "can any", "lfg", "lfm", "wtb", "wts"
        };

        for (const char* phrase : phrases)
        {
            if (loweredText.find(phrase) != std::string::npos)
                return true;
        }

        return false;
    }

    uint32_t ChanceForScope(ChatScope scope)
    {
        switch (scope)
        {
            case ChatScope::Say:     return g_ReplyChanceSay;
            case ChatScope::Party:   return g_ReplyChanceParty;
            case ChatScope::Guild:   return g_ReplyChanceGuild;
            case ChatScope::Channel: return g_ReplyChanceChannel;
            case ChatScope::Whisper: return g_ReplyChanceWhisper;
        }
        return 0;
    }

    bool ScopeEnabled(ChatScope scope)
    {
        switch (scope)
        {
            case ChatScope::Say:     return g_HandleSay != 0;
            case ChatScope::Party:   return g_HandleParty != 0;
            case ChatScope::Guild:   return g_HandleGuild != 0;
            case ChatScope::Channel: return g_HandleChannel != 0;
            case ChatScope::Whisper: return g_HandleWhispers != 0;
        }
        return false;
    }

    bool CanHear(Player* listener, Player* speaker, ChatScope scope, const std::string& channelName)
    {
        if (!listener || !speaker || listener == speaker || !listener->IsInWorld())
            return false;

        switch (scope)
        {
            case ChatScope::Say:
                return listener->GetMapId() == speaker->GetMapId()
                    && listener->GetDistance(speaker) <= g_SayDistance;

            case ChatScope::Party:
                return listener->GetGroup() && listener->GetGroup() == speaker->GetGroup();

            case ChatScope::Guild:
                return listener->GetGuildId() != 0 && listener->GetGuildId() == speaker->GetGuildId();

            case ChatScope::Whisper:
                return false;   // handled directly by the caller

            case ChatScope::Channel:
            {
                if (listener->GetTeamId() != speaker->GetTeamId())
                    return false;
                ChannelMgr* manager = ChannelMgr::forTeam(listener->GetTeamId());
                if (!manager)
                    return false;
                Channel* channel = manager->GetChannel(channelName, listener);
                return channel && listener->IsInChannel(channel);
            }
        }

        return false;
    }

    std::vector<Player*> GatherListeningBots(Player* speaker, ChatScope scope, const std::string& channelName)
    {
        std::vector<Player*> bots;

        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* candidate = pair.second;
            if (!candidate || !IsBot(candidate))
                continue;
            if (candidate->IsBeingTeleported())
                continue;
            if (g_DisableRepliesInCombat && candidate->IsInCombat())
                continue;
            if (!CanHear(candidate, speaker, scope, channelName))
                continue;

            bots.push_back(candidate);
        }

        return bots;
    }

    // Bots only talk to each other where a real player can see it happen.
    bool RealPlayerCanHear(Player* speaker, ChatScope scope, const std::string& channelName)
    {
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* candidate = pair.second;
            if (!candidate || IsBot(candidate))
                continue;
            if (CanHear(candidate, speaker, scope, channelName))
                return true;
        }
        return false;
    }

    bool Dispatch(Player* bot, Player* other, TurnKind kind, const ScopeKey& key,
                  const std::string& trigger, const std::string& channelName,
                  uint32_t chainDepth, bool forced)
    {
        TurnRequest request;
        request.bot         = bot;
        request.other       = other;
        request.kind        = kind;
        request.key         = key;
        request.trigger     = trigger;
        request.channelName = channelName;
        request.chainDepth  = chainDepth;

        return RequestBotTurn(request, forced);
    }
}

void OnPlayerLine(Player* speaker, const std::string& text, ChatScope scope, const ScopeKey& key,
                  const std::string& channelName, Player* whisperTarget)
{
    if (!g_Enable || !speaker || text.empty() || !ScopeEnabled(scope))
        return;

    RecordChatLine(key, speaker->GetGUID().GetRawValue(), speaker->GetName(), text);

    // A whisper has exactly one recipient, so there is nothing to resolve.
    if (scope == ChatScope::Whisper)
    {
        if (whisperTarget && IsBot(whisperTarget))
            Dispatch(whisperTarget, speaker, TurnKind::DirectReply, key, text, channelName, 0, true);
        return;
    }

    std::vector<Player*> candidates = GatherListeningBots(speaker, scope, channelName);
    if (candidates.empty())
        return;

    const std::string lowered = ToLower(text);

    // 1. Named directly: that bot answers, and only that bot.
    Player* mentioned    = nullptr;
    size_t  mentionedPos = std::string::npos;
    for (Player* bot : candidates)
    {
        size_t pos = NameMentionPos(lowered, bot->GetName());
        if (pos < mentionedPos)
        {
            mentioned    = bot;
            mentionedPos = pos;
        }
    }

    if (mentioned)
    {
        if (g_DebugEnabled)
            LOG_INFO("server.loading", "[BotMinds] {} was named by {}; answering directly.",
                     mentioned->GetName(), speaker->GetName());
        Dispatch(mentioned, speaker, TurnKind::DirectReply, key, text, channelName, 0, true);
        return;
    }

    const bool broadcast = LooksLikeBroadcast(lowered);

    // 2. Otherwise the bot you were already talking to owns the reply. Looked up
    //    by GUID rather than searched for among the listeners: a bot that has
    //    drifted a few yards out of say range mid-conversation still owes you an
    //    answer, and dropping it here is what made follow-ups vanish.
    Player* primary = nullptr;
    if (!broadcast)
    {
        FloorHolder floor = GetConversationFloor(key, speaker->GetGUID().GetRawValue());
        if (floor.guid != 0
            && (static_cast<uint32_t>(time(nullptr)) - floor.atSec) <= g_FloorWindowSec)
        {
            Player* holder = ObjectAccessor::FindPlayer(ObjectGuid(floor.guid));

            // Overheard scopes still need the bot in talking distance, but it gets
            // the wider conversational radius rather than strict say range, so a
            // few yards of drift mid-exchange does not cost you the answer.
            const bool closeEnough = (scope != ChatScope::Say && scope != ChatScope::Channel)
                || (holder && holder->IsWithinDistInMap(speaker, g_ProximityRadius));

            if (holder && IsBot(holder) && holder->IsInWorld() && !holder->IsBeingTeleported()
                && holder->GetMapId() == speaker->GetMapId() && closeEnough
                && !(g_DisableRepliesInCombat && holder->IsInCombat()))
            {
                primary = holder;
            }
            else if (g_DebugEnabled)
            {
                LOG_INFO("server.loading", "[BotMinds] Floor holder {} for {} is unavailable; falling back.",
                         floor.guid, speaker->GetName());
            }
        }

        // 3. Small group with nobody holding the floor: someone still has to answer,
        //    otherwise a two-man party sits there ignoring you.
        if (!primary && candidates.size() <= g_SmallGroupSize)
            primary = candidates[urand(0, candidates.size() - 1)];
    }

    uint32_t spoken = 0;

    if (primary)
    {
        if (g_DebugEnabled)
            LOG_INFO("server.loading", "[BotMinds] {} holds the floor in {}; answering {}.",
                     primary->GetName(), ScopeName(scope), speaker->GetName());

        if (!Dispatch(primary, speaker, TurnKind::DirectReply, key, text, channelName, 0, true)
            && g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[BotMinds] {} owed {} an answer but was gated (combat, cap or no provider).",
                     primary->GetName(), speaker->GetName());
        }
        ++spoken;

        // Anyone else only chips in occasionally, and may still decide not to.
        for (Player* bot : candidates)
        {
            if (bot == primary || spoken >= g_MaxBotsToPick)
                continue;
            if (urand(0, 99) >= g_InterjectChance)
                continue;
            Dispatch(bot, speaker, TurnKind::Interjection, key, text, channelName, 0, false);
            ++spoken;
        }

        return;
    }

    // 4. Open floor: roll for each listener. A broadcast is fair game for several
    //    bots; anything else is offered as an interjection the bot can decline.
    const uint32_t chance = ChanceForScope(scope);
    const TurnKind kind   = broadcast ? TurnKind::DirectReply : TurnKind::Interjection;

    std::shuffle(candidates.begin(), candidates.end(), RandomEngine::Instance());

    for (Player* bot : candidates)
    {
        if (spoken >= g_MaxBotsToPick)
            break;
        if (urand(0, 99) >= chance)
            continue;
        Dispatch(bot, speaker, kind, key, text, channelName, 0, false);
        ++spoken;
    }

    if (g_DebugEnabled && spoken == 0)
        LOG_INFO("server.loading", "[BotMinds] Nobody picked up '{}' from {} in {} ({} could hear).",
                 text, speaker->GetName(), ScopeName(scope), candidates.size());
}

void OnLineSpoken(uint64_t speakerGuid, const std::string& /*speakerName*/, const std::string& text,
                  const ScopeKey& key, const std::string& channelName, uint32_t chainDepth)
{
    if (!g_Enable || text.empty())
        return;

    if (chainDepth + 1 > g_MaxBotChainDepth)
        return;

    Player* speaker = ObjectAccessor::FindPlayer(ObjectGuid(speakerGuid));
    if (!speaker || !speaker->IsInWorld())
        return;

    if (!ScopeEnabled(key.scope) || key.scope == ChatScope::Whisper)
        return;

    if (!RealPlayerCanHear(speaker, key.scope, channelName))
        return;

    std::vector<Player*> candidates = GatherListeningBots(speaker, key.scope, channelName);
    if (candidates.empty())
        return;

    std::shuffle(candidates.begin(), candidates.end(), RandomEngine::Instance());

    for (Player* bot : candidates)
    {
        if (urand(0, 99) >= g_ReplyChanceBotToBot)
            continue;
        Dispatch(bot, speaker, TurnKind::Interjection, key, text, channelName, chainDepth + 1, false);
        break;   // at most one bot picks up another bot's line
    }
}
