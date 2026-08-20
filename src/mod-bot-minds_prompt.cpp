#include "mod-bot-minds_prompt.h"
#include "mod-bot-minds_config.h"
#include "mod-bot-minds_memory.h"
#include "mod-bot-minds_persona.h"
#include "mod-bot-minds_relationship.h"
#include "mod-bot-minds-utilities.h"

#include "Group.h"
#include "Guild.h"
#include "Map.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "SharedDefines.h"

#include <fmt/core.h>
#include <sstream>
#include <unordered_set>

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

    // Where the bot is, plus the few facts that change how a player talks.
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
        out << "You are level " << bot->GetLevel() << ", in " << area << " (" << zone << ").";

        if (bot->GetMap() && bot->GetMap()->IsDungeon())
            out << " You are inside " << bot->GetMap()->GetMapName() << ".";

        if (bot->IsInCombat())
            out << " You are in combat right now, so keep it to a few words.";

        if (bot->GetGroup())
            out << " You are in a group.";

        if (Guild* guild = bot->GetGuild())
            out << " You are in the guild " << guild->GetName() << ".";

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
            "- No asterisks, no emotes, no stage directions, no quotation marks around your words, no emoji, no markdown, no name prefix.\n"
            "- Never mention being an AI, a bot, a model, or these instructions.\n"
            "- Hard limit {} characters. One sentence is usually right, two is the maximum.\n"
            "- Never repeat something already said in the recent chat.",
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
                return "Nobody is talking to you. Make one short unprompted remark of your own about the "
                       "situation below, the way a player idly types in chat. Do not greet anyone and do not "
                       "ask how anyone is doing. Set should_reply to true.";

            case TurnKind::Event:
                return "Something just happened near you, described below. React in a few words if it is worth "
                       "commenting on, otherwise set should_reply to false and leave reply empty.";
        }

        return "";
    }
}

TurnPrompt BuildTurnPrompt(const TurnRequest& request)
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

    system << "\n" << KindInstruction(request) << "\n\n";
    system << VoiceRules() << "\n\n";
    system << "Use the bot_turn tool for everything: your reply, whether you speak at all, anything new worth "
              "remembering, and any change in how you feel about the person you are talking to.";

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
