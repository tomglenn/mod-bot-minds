#ifndef MOD_BOT_MINDS_MEMORY_H
#define MOD_BOT_MINDS_MEMORY_H

#include <string>
#include <cstdint>
#include <vector>

// --------------------------------------------
// Memory: what a bot keeps about someone or something. A memory may be tied to a
// subject (another player or bot) or be general (subject_guid NULL). Newer and
// higher-salience memories are preferred when building prompts.
//
// Writes go straight to the database, so a memory survives a hard restart.
// --------------------------------------------
struct MemoryEntry
{
    uint64_t    subjectGuid = 0;   // 0 == general (no subject)
    std::string kind;              // "event" | "fact" | "summary"
    std::string text;
    float       salience = 0.5f;
};

// Up to n most recent memories a bot holds about a subject, newest first.
// subjectGuid == 0 returns general memories.
std::vector<MemoryEntry> GetRecentMemories(uint64_t botGuid, uint64_t subjectGuid, int n);

// Up to n memories ranked by salience, including general ones.
std::vector<MemoryEntry> GetRelevantMemories(uint64_t botGuid, uint64_t subjectGuid, int n);

// Record a new memory and write it to the database immediately.
void AddMemory(uint64_t botGuid, uint64_t subjectGuid, const std::string& kind,
               const std::string& text, float salience);

// Trim (bot, subject) down to g_MaxMemoriesPerSubject, dropping the least
// salient and oldest first.
void DistillOldMemories(uint64_t botGuid, uint64_t subjectGuid);

// Load every stored memory into the cache. Called once at startup.
void LoadMemoriesFromDB();

#endif // MOD_BOT_MINDS_MEMORY_H
