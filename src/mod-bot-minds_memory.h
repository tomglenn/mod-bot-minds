#ifndef MOD_BOT_MINDS_MEMORY_H
#define MOD_BOT_MINDS_MEMORY_H

#include <string>
#include <cstdint>
#include <vector>

// --------------------------------------------
// Memory: what a bot keeps about someone or something. A memory may be tied to a
// subject (another player or bot) or be general (subject_guid NULL).
//
// A memory is ranked by its salience discounted by how stale it is, so a big
// moment outlives a month of small talk while two equally dull lines are
// separated by which happened lately. Being put into a prompt counts as using a
// memory and keeps it fresh.
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

// Up to n memories ranked by salience and staleness, including general ones.
std::vector<MemoryEntry> GetRelevantMemories(uint64_t botGuid, uint64_t subjectGuid, int n);

// Record a new memory and write it to the database immediately. A memory the bot
// already holds word for word is refreshed rather than stored twice.
void AddMemory(uint64_t botGuid, uint64_t subjectGuid, const std::string& kind,
               const std::string& text, float salience);

// Trim (bot, subject) down to g_MaxMemoriesPerSubject, dropping whatever scores
// lowest and folding its gist into a summary row.
void DistillOldMemories(uint64_t botGuid, uint64_t subjectGuid);

// Write out the memory uses collected since the last flush. The getters call
// this on a timer of their own, so wiring it into a tick is optional; call it on
// shutdown to avoid losing the last few minutes.
void FlushMemoryReferences();

// Load every stored memory into the cache. Called once at startup.
void LoadMemoriesFromDB();

#endif // MOD_BOT_MINDS_MEMORY_H
