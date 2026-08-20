#ifndef MOD_BOT_MINDS_PERSONA_H
#define MOD_BOT_MINDS_PERSONA_H

#include <string>
#include <cstdint>

class Player;

// --------------------------------------------
// Persona: a persistent identity for a bot.
// Each bot has one persona row keyed by its GUID.
// --------------------------------------------
struct Persona
{
    uint64_t    guid = 0;
    std::string name;
    std::string archetype;
    std::string traits;
    std::string speechStyle;
    std::string backstory;
};

// Returns the persona for the given bot. If none exists in cache or DB,
// a template persona is generated deterministically from the bot's
// class/race/name, inserted into the DB, and cached.
const Persona& GetPersona(Player* bot);

// Load all personas from the database into the in-memory cache.
void LoadPersonasFromDB();

// Upsert all dirty (created/modified) personas back to the database.
void FlushPersonasToDB();

#endif // MOD_BOT_MINDS_PERSONA_H
