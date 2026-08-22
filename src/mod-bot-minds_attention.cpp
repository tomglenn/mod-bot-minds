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
#include <unordered_set>
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

    // Position of `needle` as a whole word in `loweredText`, or npos. Word-bounded
    // so "Al" does not match "always".
    size_t WholeWordPos(const std::string& loweredText, const std::string& needle)
    {
        if (needle.empty())
            return std::string::npos;

        size_t pos = 0;
        while ((pos = loweredText.find(needle, pos)) != std::string::npos)
        {
            bool startsClean = (pos == 0) || !std::isalnum(static_cast<unsigned char>(loweredText[pos - 1]));
            size_t end = pos + needle.size();
            bool endsClean = (end >= loweredText.size()) || !std::isalnum(static_cast<unsigned char>(loweredText[end]));
            if (startsClean && endsClean)
                return pos;
            ++pos;
        }
        return std::string::npos;
    }

    // Short words that are far more likely to be English than somebody shortening
    // a name. Without this a bot called Andrea answers every sentence containing
    // "and", and one called Sual answers "sure".
    bool TooCommonForANickname(const std::string& word)
    {
        static const std::unordered_set<std::string> common = {
            "the", "and", "you", "for", "are", "was", "his", "her", "its", "our", "one", "all",
            "out", "get", "got", "any", "now", "way", "day", "let", "put", "say", "see", "who",
            "why", "how", "hey", "yes", "not", "but", "can", "did", "has", "had", "him", "she",
            "they", "them", "this", "that", "with", "from", "just", "what", "when", "sure",
            "mate", "then", "than", "here", "there", "your", "mine", "some", "want", "need",
            "give", "take", "come", "wait", "stay", "gold", "help", "good", "nice", "well"
        };

        return common.count(word) != 0;
    }

    // Where a bot's name appears, allowing the shortenings people actually use in
    // chat: "Hey Gas, how's it going?" is aimed at Gascard.
    //
    // A nickname only counts when it is a prefix of the name, at least three
    // characters, and not a common English word. Ambiguity is resolved by the
    // caller, which ignores a nickname matching more than one bot present.
    size_t NameMentionPos(const std::string& loweredText, const std::string& botName, bool& viaNickname)
    {
        viaNickname = false;

        const std::string loweredName = ToLower(botName);

        size_t pos = WholeWordPos(loweredText, loweredName);
        if (pos != std::string::npos)
            return pos;

        // Walk the words of the message rather than every prefix of the name: a
        // message has few words, and this way the longest sensible match wins.
        size_t best = std::string::npos;
        size_t start = 0;

        while (start < loweredText.size())
        {
            while (start < loweredText.size() && !std::isalnum(static_cast<unsigned char>(loweredText[start])))
                ++start;

            size_t end = start;
            while (end < loweredText.size() && std::isalnum(static_cast<unsigned char>(loweredText[end])))
                ++end;

            if (end == start)
                break;

            const std::string word = loweredText.substr(start, end - start);
            if (word.size() >= 3 && word.size() < loweredName.size()
                && loweredName.compare(0, word.size(), word) == 0
                && !TooCommonForANickname(word))
            {
                if (start < best)
                {
                    best = start;
                    viaNickname = true;
                }
            }

            start = end;
        }

        return best;
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
                  uint32_t chainDepth, bool forced, bool namedDirectly = false)
    {
        TurnRequest request;
        request.bot         = bot;
        request.other       = other;
        request.kind        = kind;
        request.key         = key;
        request.trigger     = trigger;
        request.channelName = channelName;
        request.chainDepth    = chainDepth;
        request.namedDirectly = namedDirectly;

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

    // 1. Named directly: that bot answers, and only that bot. A full name always
    //    wins over somebody else's shortening, and an ambiguous shortening counts
    //    for nobody, since "Hey Gas" among two bots starting Gas names neither.
    Player* mentioned      = nullptr;
    size_t  mentionedPos   = std::string::npos;
    bool    mentionedShort = false;
    uint32_t shortMatches  = 0;

    for (Player* bot : candidates)
    {
        bool viaNickname = false;
        size_t pos = NameMentionPos(lowered, bot->GetName(), viaNickname);
        if (pos == std::string::npos)
            continue;

        if (viaNickname)
            ++shortMatches;

        // Prefer a full name, then whichever came first in the sentence.
        const bool better = (mentioned == nullptr)
            || (mentionedShort && !viaNickname)
            || (mentionedShort == viaNickname && pos < mentionedPos);

        if (better)
        {
            mentioned      = bot;
            mentionedPos   = pos;
            mentionedShort = viaNickname;
        }
    }

    if (mentioned && mentionedShort && shortMatches > 1)
    {
        if (g_DebugEnabled)
            LOG_INFO("server.loading", "[BotMinds] A shortened name from {} fitted {} bots, so nobody claimed it.",
                     speaker->GetName(), shortMatches);
        mentioned = nullptr;
    }

    if (mentioned)
    {
        if (g_DebugEnabled)
            LOG_INFO("server.loading", "[BotMinds] {} was named by {}{}; answering directly.",
                     mentioned->GetName(), speaker->GetName(), mentionedShort ? " (shortened)" : "");
        Dispatch(mentioned, speaker, TurnKind::DirectReply, key, text, channelName, 0, true,
                 /*namedDirectly=*/true);
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
