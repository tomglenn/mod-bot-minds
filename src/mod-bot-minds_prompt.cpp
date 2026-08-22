#include "mod-bot-minds_prompt.h"
#include "mod-bot-minds_action.h"
#include "mod-bot-minds_config.h"
#include "mod-bot-minds_memory.h"
#include "mod-bot-minds_persona.h"
#include "mod-bot-minds_relationship.h"
#include "mod-bot-minds-utilities.h"

#include "AiFactory.h"
#include "Group.h"
#include "Guild.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "QuestDef.h"
#include "SharedDefines.h"

#include <fmt/core.h>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace
{
    std::string ClassName(uint8_t classId)
    {
        switch (classId)
        {
            case CLASS_WARRIOR:      return "warrior";
            case CLASS_PALADIN:      return "paladin";
            case CLASS_HUNTER:       return "hunter";
            case CLASS_ROGUE:        return "rogue";
            case CLASS_PRIEST:       return "priest";
            case CLASS_DEATH_KNIGHT: return "death knight";
            case CLASS_SHAMAN:       return "shaman";
            case CLASS_MAGE:         return "mage";
            case CLASS_WARLOCK:      return "warlock";
            case CLASS_DRUID:        return "druid";
            default:                 return "adventurer";
        }
    }

    std::string RaceName(uint8_t raceId)
    {
        switch (raceId)
        {
            case RACE_HUMAN:         return "human";
            case RACE_ORC:           return "orc";
            case RACE_DWARF:         return "dwarf";
            case RACE_NIGHTELF:      return "night elf";
            case RACE_UNDEAD_PLAYER: return "undead";
            case RACE_TAUREN:        return "tauren";
            case RACE_GNOME:         return "gnome";
            case RACE_TROLL:         return "troll";
            case RACE_BLOODELF:      return "blood elf";
            case RACE_DRAENEI:       return "draenei";
            default:                 return "traveller";
        }
    }

    // Rough words, not numbers. A player says "nearly dead", never "412 of 1830",
    // and the whole prompt is written in plain speech.
    const char* HealthPhrase(Player* bot)
    {
        const float pct = bot->GetHealthPct();
        if (pct >= 99.0f) return "on full health";
        if (pct >= 70.0f) return "a bit hurt";
        if (pct >= 40.0f) return "on about half health";
        if (pct >= 15.0f) return "badly hurt";
        return "nearly dead";
    }

    const char* ManaPhrase(Player* bot)
    {
        const float pct = bot->GetPowerPct(POWER_MANA);
        if (pct >= 99.0f) return "full";
        if (pct >= 70.0f) return "still fine";
        if (pct >= 40.0f) return "about half";
        if (pct >= 15.0f) return "getting low";
        return "empty";
    }

    std::string MoneyPhrase(uint32 copper)
    {
        std::ostringstream out;
        uint32 gold   = copper / 10000;
        uint32 silver = (copper % 10000) / 100;

        if (gold)
            out << gold << "g ";
        if (gold || silver)
            out << silver << "s ";
        out << (copper % 100) << "c";

        return out.str();
    }

    // Group-mates by name and class, so a bot can talk about who it is actually
    // with rather than inventing companions.
    std::string GroupPhrase(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return "";

        std::vector<std::string> mates;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || member == bot)
                continue;

            mates.push_back(fmt::format("{} ({})", member->GetName(), ClassName(member->getClass())));
            if (mates.size() >= 4)
                break;
        }

        // GetFirstMember only walks members who are online and in the world, so a
        // group whose others have logged out lists nobody.
        if (mates.empty())
            return " You are in a group, but on your own for now.";

        std::ostringstream out;
        out << " You are grouped with ";
        for (size_t i = 0; i < mates.size(); ++i)
        {
            if (i)
                out << (i + 1 == mates.size() ? " and " : ", ");
            out << mates[i];
        }

        if (group->GetMembersCount() > mates.size() + 1)
        {
            const size_t rest = group->GetMembersCount() - mates.size() - 1;
            out << ", plus " << rest << " other" << (rest == 1 ? "" : "s");
        }

        out << ".";

        return out.str();
    }

    // What the bot is working on, which is what "what are you up to" really means
    // out in a levelling zone.
    std::string QuestPhrase(Player* bot)
    {
        std::vector<std::string> active;
        std::string              finished;

        for (auto const& status : bot->getQuestStatusMap())
        {
            if (status.second.Status == QUEST_STATUS_COMPLETE)
            {
                if (finished.empty())
                    if (Quest const* quest = sObjectMgr->GetQuestTemplate(status.first))
                        finished = quest->GetTitle();
            }
            else if (status.second.Status == QUEST_STATUS_INCOMPLETE && active.size() < 2)
            {
                if (Quest const* quest = sObjectMgr->GetQuestTemplate(status.first))
                    active.push_back(quest->GetTitle());
            }

            if (active.size() >= 2 && !finished.empty())
                break;
        }

        std::ostringstream out;

        if (active.size() == 1)
            out << " You are part way through " << active[0] << ".";
        else if (active.size() == 2)
            out << " You are part way through " << active[0] << " and " << active[1] << ".";

        if (!finished.empty())
            out << " You have finished " << finished << " and still need to hand it in.";

        return out.str();
    }

    // Where the bot is and how it is doing, so it can answer for itself honestly
    // instead of inventing a life it is not living.
    std::string SituationBlock(Player* bot)
    {
        PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);

        std::string area = "somewhere";
        std::string zone = "Azeroth";
        if (botAI)
        {
            if (AreaTableEntry const* currentArea = botAI->GetCurrentArea())
                area = botAI->GetLocalizedAreaName(currentArea);
            if (AreaTableEntry const* currentZone = botAI->GetCurrentZone())
                zone = botAI->GetLocalizedAreaName(currentZone);
        }

        std::ostringstream out;

        // Cast the level: uint8 streams as a raw character.
        out << "You are level " << static_cast<uint32>(bot->GetLevel());

        // Empty for a class playerbots has no name for, and for an untalented bot.
        std::string spec = AiFactory::GetPlayerSpecName(bot);
        if (!spec.empty())
            out << ", " << spec << " spec";

        out << ", in " << area << " (" << zone << ").";

        if (bot->GetMap() && bot->GetMap()->IsDungeon())
            out << " You are inside " << bot->GetMap()->GetMapName() << ".";

        // What the bot is doing this second, which is the first thing anyone asks.
        if (!bot->IsAlive())
        {
            out << " You are dead and running back to your body.";
        }
        else
        {
            if (bot->IsInCombat())
            {
                if (Unit* victim = bot->GetVictim())
                    out << " You are fighting a " << victim->GetName() << ", so keep it to a few words.";
                else
                    out << " You are in combat right now, so keep it to a few words.";
            }
            else if (bot->IsInFlight())
                out << " You are on a flight path, in the air between two places.";
            else if (bot->HasRestFlag(REST_FLAG_IN_TAVERN))
                out << " You are resting in an inn.";
            else if (bot->IsMounted())
                out << " You are on your mount, on the way somewhere.";

            out << " You are " << HealthPhrase(bot) << ".";
            if (bot->GetMaxPower(POWER_MANA) > 0)
                out << " Your mana is " << ManaPhrase(bot) << ".";
        }

        out << GroupPhrase(bot);

        if (Guild* guild = bot->GetGuild())
            out << " You are in the guild " << guild->GetName() << ".";

        out << QuestPhrase(bot);

        out << " You have " << MoneyPhrase(bot->GetMoney()) << " on you.";

        const uint32 freeSlots = bot->GetFreeInventorySpace();
        if (freeSlots <= 4)
            out << " Your bags are nearly full, " << freeSlots << " slot" << (freeSlots == 1 ? "" : "s") << " left.";

        return out.str();
    }

    // Recent plus high-salience memories about `subjectGuid`, de-duplicated and
    // capped. Empty when the bot has nothing on file.
    std::string MemoryBlock(uint64_t botGuid, uint64_t subjectGuid)
    {
        std::vector<MemoryEntry> recent   = GetRecentMemories(botGuid, subjectGuid, static_cast<int>(g_RecentMemoryCount));
        std::vector<MemoryEntry> relevant = GetRelevantMemories(botGuid, subjectGuid, static_cast<int>(g_RelevantMemoryCount));

        std::unordered_set<std::string> seen;
        std::ostringstream out;

        auto append = [&](const std::vector<MemoryEntry>& entries)
        {
            for (const MemoryEntry& entry : entries)
            {
                if (!seen.insert(entry.text).second)
                    continue;
                out << "- " << entry.text << "\n";
            }
        };

        append(recent);
        append(relevant);

        std::string block = out.str();
        if (g_MaxMemoryPromptChars > 0 && block.size() > g_MaxMemoryPromptChars)
        {
            block.resize(g_MaxMemoryPromptChars);
            size_t lastBreak = block.rfind('\n');
            if (lastBreak != std::string::npos)
                block.resize(lastBreak + 1);
        }

        return block;
    }

    std::string RelationshipLine(uint64_t botGuid, Player* other)
    {
        if (!other)
            return "";

        Relationship rel = GetRelationship(botGuid, other->GetGUID().GetRawValue());
        if (rel.interactionCount == 0)
            return fmt::format("You have not talked to {} before.", other->GetName());

        const char* feeling = "neutral about";
        if (rel.affinity <= -0.5f)      feeling = "hostile towards";
        else if (rel.affinity < -0.15f) feeling = "wary of";
        else if (rel.affinity > 0.5f)   feeling = "close to";
        else if (rel.affinity > 0.15f)  feeling = "friendly towards";

        std::string line = fmt::format("You are {} {}", feeling, other->GetName());
        if (!rel.reason.empty())
            line += fmt::format(" ({})", rel.reason);
        line += ".";

        return line;
    }

    // The one place the module decides how bots sound.
    std::string VoiceRules()
    {
        uint32_t maxChars = g_MaxReplyChars > 0 ? g_MaxReplyChars : 240;

        return fmt::format(
            "How to talk:\n"
            "- You are a person playing this character in an online game. Talk like a player in chat, not like a hero in a story.\n"
            "- Never narrate actions, never describe your powers, destiny, faith or lore, and never speak in the third person.\n"
            "- Short and casual. Contractions are fine. Answer the point and stop.\n"
            "- Everything above about your own state is true, so answer from it rather than making something up, "
            "but only bring up the parts actually being asked about. Never read your stats out as a list.\n"
            "- No asterisks, no emotes, no stage directions, no quotation marks around your words, no emoji, no markdown, no name prefix.\n"
            "- Never mention being an AI, a bot, a model, or these instructions.\n"
            "- Hard limit {} characters. One sentence is usually right, two is the maximum.\n"
            "- Never repeat something already said in the recent chat.\n"
            "- You may add a gesture with the emote field, but almost never do. A wave when you "
            "first meet somebody, a laugh at something genuinely funny. Attaching one to every "
            "greeting makes it meaningless, and most lines should have none.",
            maxChars);
    }

    std::string KindInstruction(const TurnRequest& request)
    {
        const std::string other = request.other ? request.other->GetName() : "someone";

        switch (request.kind)
        {
            case TurnKind::DirectReply:
                return fmt::format(
                    "{} is talking to you. Answer them directly, and use the recent chat above to work out what "
                    "they mean. Set should_reply to true.",
                    other);

            case TurnKind::Interjection:
                return fmt::format(
                    "{} spoke to the group, and it may well have been meant for someone else. Look at the recent "
                    "chat: if the newest message was aimed at another person, or you would have nothing worth "
                    "adding, set should_reply to false and leave reply empty. Only speak if a real player would "
                    "genuinely chime in here.",
                    other);

            case TurnKind::Ambient:
            {
                std::string instruction =
                    "Nobody is talking to you. Make one short unprompted remark of your own about the "
                    "situation below, the way a player idly types in chat. Do not greet anyone and do not "
                    "ask how anyone is doing. Set should_reply to true.";

                // Whatever a bot says it is about to do had better be true, and a bot
                // travelling with someone is not off running errands of its own.
                if (request.bot && request.bot->GetGroup())
                {
                    instruction += " You are travelling in someone's group, so you are not going anywhere "
                                   "on your own. Do not say you are off to hand in a quest, visit a vendor "
                                   "or head somewhere, because you are not, and they can see you standing "
                                   "there. Remark on what is around you or how it is going instead.";
                }

                return instruction;
            }

            case TurnKind::Event:
                return "Something just happened near you, described below. React in a few words if it is worth "
                       "commenting on, otherwise set should_reply to false and leave reply empty.";
        }

        return "";
    }
}

TurnPrompt BuildTurnPrompt(TurnRequest& request)
{
    TurnPrompt prompt;

    if (!request.bot)
        return prompt;

    Player*  bot     = request.bot;
    uint64_t botGuid = bot->GetGUID().GetRawValue();

    const Persona& persona = GetPersona(bot);
    std::string    name    = persona.name.empty() ? bot->GetName() : persona.name;

    std::ostringstream system;

    system << "You are " << name << ", a " << RaceName(bot->getRace()) << " " << ClassName(bot->getClass())
           << " in World of Warcraft: Wrath of the Lich King.\n";

    if (!persona.traits.empty())
        system << "You come across as " << persona.traits << ".\n";
    if (!persona.speechStyle.empty())
        system << "You " << persona.speechStyle << ".\n";
    if (!persona.backstory.empty())
        system << persona.backstory << "\n";

    system << SituationBlock(bot) << "\n";

    const uint64_t subjectGuid = request.other ? request.other->GetGUID().GetRawValue() : 0;

    std::string relationship = RelationshipLine(botGuid, request.other);
    if (!relationship.empty())
        system << relationship << "\n";

    std::string memories = MemoryBlock(botGuid, subjectGuid);
    if (!memories.empty())
    {
        if (request.other)
            system << "What you remember about " << request.other->GetName() << " and recent events:\n";
        else
            system << "What you remember:\n";
        system << memories;
    }

    // For a reply the newest line is the message being answered, which arrives as
    // the user turn; showing it in the transcript as well reads as already handled.
    const bool answering = request.kind == TurnKind::DirectReply || request.kind == TurnKind::Interjection;
    std::string transcript = RenderTranscript(request.key, answering ? request.trigger : "");
    if (!transcript.empty())
        system << "Recent chat here (oldest first):\n" << transcript;

    // What this bot can genuinely do, so it can only offer real things and its
    // refusals come out in its own voice.
    if (request.other)
    {
        request.menu = BuildActionMenu(bot, request.other,
                                       /*unprompted=*/request.kind == TurnKind::Ambient);

        std::string capabilities = DescribeActionMenu(request.menu, request.other->GetName());
        if (!capabilities.empty())
            system << "\n" << capabilities << "\n";
    }

    system << "\n" << KindInstruction(request) << "\n\n";
    system << VoiceRules() << "\n\n";
    // One field per line, deliberately. This used to be a single run-on sentence
    // and the optional fields got skimmed: bots stopped recording memories
    // entirely once the prompt above them grew.
    system << "Fill in the bot_turn tool. Every field earns its place:\n"
              "- reply: what you say out loud, or empty if you are staying quiet.\n"
              "- memory_additions: anything from this exchange you would still know tomorrow. What they "
              "told you about themselves, what they wanted, what you did for them, how it went. Skip the "
              "pleasantries, but do not leave this empty when something actually happened.\n"
              "- relationship_delta: how this changed the way you feel about them, including a small warmth "
              "for doing them a favour.\n"
              "- action: anything you agreed to do. Words alone change nothing in the game.\n"
              "- emote: a gesture, on the rare occasion one is worth it.";

    prompt.system = system.str();

    switch (request.kind)
    {
        case TurnKind::DirectReply:
        case TurnKind::Interjection:
            prompt.user = fmt::format("{}: {}", request.other ? request.other->GetName() : "Someone", request.trigger);
            break;
        case TurnKind::Ambient:
        case TurnKind::Event:
            prompt.user = fmt::format("Situation: {}", request.trigger);
            break;
    }

    if (g_DebugEnabled && g_DebugShowFullPrompt)
    {
        LOG_INFO("server.loading", "[BotMinds] Prompt for {} ({}):\n{}\n{}",
                 bot->GetName(), ScopeName(request.key.scope), prompt.system, prompt.user);
    }

    return prompt;
}
