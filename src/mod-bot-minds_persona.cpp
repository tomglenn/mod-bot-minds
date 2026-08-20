#include "mod-bot-minds_persona.h"
#include "mod-bot-minds-utilities.h"
#include "Log.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "Field.h"
#include "Player.h"
#include "SharedDefines.h"
#include <fmt/core.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Config globals (defined in _config.cpp).
extern bool g_DebugEnabled;

// --------------------------------------------
// In-memory cache and dirty-set.
// --------------------------------------------
static std::unordered_map<uint64_t, Persona> g_Personas;
static std::unordered_set<uint64_t>          g_PersonaDirty;
static std::mutex                            g_PersonaMutex;

namespace
{
    // Deterministic pick from a table using the seed value.
    const std::string& PickDeterministic(const std::vector<std::string>& table, uint64_t seed)
    {
        return table[seed % table.size()];
    }

    std::string ArchetypeForClass(uint8 cls)
    {
        switch (cls)
        {
            case CLASS_WARRIOR:      return "Warrior";
            case CLASS_PALADIN:      return "Crusader";
            case CLASS_HUNTER:       return "Hunter";
            case CLASS_ROGUE:        return "Rogue";
            case CLASS_PRIEST:       return "Priest";
            case CLASS_DEATH_KNIGHT: return "Death Knight";
            case CLASS_SHAMAN:       return "Shaman";
            case CLASS_MAGE:         return "Mage";
            case CLASS_WARLOCK:      return "Warlock";
            case CLASS_DRUID:        return "Druid";
            default:                 return "Adventurer";
        }
    }

    // Build a deterministic template persona from a bot's class/race/name.
    //
    // Traits and speech style only. Generated personas deliberately carry no
    // backstory: a heroic background is what makes a bot start talking about its
    // destiny instead of its quest log. A hand-written backstory in the database
    // is still used if one is there.
    Persona GenerateTemplatePersona(uint64_t guid, const std::string& name, uint8 cls, uint8 race)
    {
        static const std::vector<std::string> traitTable = {
            "blunt, loyal, impatient",
            "chatty, curious, upbeat",
            "cautious, observant, wry",
            "hot-headed, competitive, proud",
            "calm, helpful, easygoing",
            "greedy about loot and always angling for a deal",
            "friendly and a bit clueless",
            "serious and focused on making progress"
        };
        static const std::vector<std::string> speechTable = {
            "keep it short and blunt",
            "chat a lot and joke around",
            "are dry and sarcastic",
            "are cheerful and a little over-eager",
            "are calm and matter-of-fact",
            "moan about everything, cheerfully",
            "are terse, almost monosyllabic",
            "over-explain things"
        };

        // Seed derived from stable identity fields so results are reproducible.
        uint64_t seed = guid ^ (static_cast<uint64_t>(cls) << 8) ^ (static_cast<uint64_t>(race) << 16);

        Persona p;
        p.guid        = guid;
        p.name        = name;
        p.archetype   = ArchetypeForClass(cls);
        p.traits      = PickDeterministic(traitTable, seed);
        p.speechStyle = PickDeterministic(speechTable, seed >> 3);
        return p;
    }

    // Insert a persona row immediately (used when auto-generating on first access).
    void InsertPersonaRow(const Persona& p)
    {
        std::string escName      = p.name;
        std::string escArchetype = p.archetype;
        std::string escTraits    = p.traits;
        std::string escSpeech    = p.speechStyle;
        std::string escBackstory = p.backstory;
        CharacterDatabase.EscapeString(escName);
        CharacterDatabase.EscapeString(escArchetype);
        CharacterDatabase.EscapeString(escTraits);
        CharacterDatabase.EscapeString(escSpeech);
        CharacterDatabase.EscapeString(escBackstory);

        CharacterDatabase.Execute(SafeFormat(
            "REPLACE INTO mod_bot_minds_persona "
            "(bot_guid, name, archetype, traits, speech_style, backstory) "
            "VALUES ({}, '{}', '{}', '{}', '{}', '{}')",
            p.guid, escName, escArchetype, escTraits, escSpeech, escBackstory));
    }
}

const Persona& GetPersona(Player* bot)
{
    static Persona emptyPersona;

    if (!bot)
        return emptyPersona;

    uint64_t guid = bot->GetGUID().GetRawValue();

    std::lock_guard<std::mutex> lock(g_PersonaMutex);

    auto it = g_Personas.find(guid);
    if (it != g_Personas.end())
        return it->second;

    // No persona yet: generate a deterministic template from class/race/name,
    // persist it, and cache it. An LLM-driven persona path can be added later.
    Persona generated = GenerateTemplatePersona(
        guid, bot->GetName(), bot->getClass(), bot->getRace());

    InsertPersonaRow(generated);

    auto res = g_Personas.emplace(guid, std::move(generated));

    if (g_DebugEnabled)
    {
        LOG_INFO("server.loading",
                 "[BotMinds] Generated template persona '{}' (archetype '{}') for bot {}",
                 res.first->second.name, res.first->second.archetype, bot->GetName());
    }

    return res.first->second;
}

void LoadPersonasFromDB()
{
    std::lock_guard<std::mutex> lock(g_PersonaMutex);
    g_Personas.clear();
    g_PersonaDirty.clear();

    QueryResult result = CharacterDatabase.Query(
        "SELECT bot_guid, name, archetype, traits, speech_style, backstory FROM mod_bot_minds_persona");

    if (!result)
    {
        LOG_INFO("server.loading", "[BotMinds] No existing persona data found in database");
        return;
    }

    uint32_t count = 0;
    do
    {
        Field* fields = result->Fetch();
        Persona p;
        p.guid        = fields[0].Get<uint64_t>();
        p.name        = fields[1].Get<std::string>();
        p.archetype   = fields[2].Get<std::string>();
        p.traits      = fields[3].Get<std::string>();
        p.speechStyle = fields[4].Get<std::string>();
        p.backstory   = fields[5].Get<std::string>();

        g_Personas[p.guid] = std::move(p);
        ++count;
    } while (result->NextRow());

    LOG_INFO("server.loading", "[BotMinds] Loaded {} persona records from database", count);
}

void FlushPersonasToDB()
{
    std::lock_guard<std::mutex> lock(g_PersonaMutex);

    if (g_PersonaDirty.empty())
        return;

    for (uint64_t guid : g_PersonaDirty)
    {
        auto it = g_Personas.find(guid);
        if (it == g_Personas.end())
            continue;

        InsertPersonaRow(it->second);
    }

    if (g_DebugEnabled)
    {
        LOG_INFO("server.loading", "[BotMinds] Flushed {} dirty persona records to database",
                 static_cast<uint32_t>(g_PersonaDirty.size()));
    }

    g_PersonaDirty.clear();
}
