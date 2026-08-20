#include "mod-bot-minds_memory.h"
#include "mod-bot-minds-utilities.h"
#include "Log.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "Field.h"
#include <fmt/core.h>
#include <algorithm>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <vector>

// Config globals (defined in _config.cpp).
extern bool     g_DebugEnabled;
extern uint32_t g_RecentMemoryCount;
extern uint32_t g_RelevantMemoryCount;
extern uint32_t g_MaxMemoriesPerSubject;

// --------------------------------------------
// In-memory cache, mirrored to the database on every write.
//
// Memories are grouped per bot, then per subject (0 == general), newest last.
// Rows are written the moment they are created rather than batched: a memory
// that only exists in RAM is lost if the server is killed, and the whole point
// of the table is that bots remember across restarts.
// --------------------------------------------
namespace
{
    struct MemoryRow
    {
        uint64_t    botGuid = 0;
        uint64_t    subjectGuid = 0;   // 0 == general
        std::string kind;
        std::string text;
        float       salience = 0.5f;
    };

    // botGuid -> subjectGuid -> ordered rows (oldest first).
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, std::deque<MemoryRow>>> g_Memories;
    std::mutex g_MemoryMutex;

    MemoryEntry ToEntry(const MemoryRow& row)
    {
        MemoryEntry entry;
        entry.subjectGuid = row.subjectGuid;
        entry.kind        = row.kind;
        entry.text        = row.text;
        entry.salience    = row.salience;
        return entry;
    }

    std::string SubjectClause(uint64_t subjectGuid)
    {
        return subjectGuid == 0 ? "subject_guid IS NULL"
                                : SafeFormat("subject_guid = {}", subjectGuid);
    }

    // Caller holds g_MemoryMutex. Drops the lowest-salience, then oldest,
    // non-summary rows until at most `cap` remain. Rows are matched in the
    // database by their text, which is what identifies a memory in practice.
    void PruneSubjectBucket(std::deque<MemoryRow>& bucket, size_t cap)
    {
        if (bucket.size() <= cap)
            return;

        std::vector<size_t> prunable;
        prunable.reserve(bucket.size());
        for (size_t i = 0; i < bucket.size(); ++i)
        {
            if (bucket[i].kind != "summary")
                prunable.push_back(i);
        }

        std::sort(prunable.begin(), prunable.end(),
            [&bucket](size_t a, size_t b)
            {
                if (bucket[a].salience != bucket[b].salience)
                    return bucket[a].salience < bucket[b].salience;
                return a < b;
            });

        size_t toRemove = bucket.size() - cap;
        std::vector<size_t> removeIdx(prunable.begin(),
                                      prunable.begin() + std::min(toRemove, prunable.size()));
        // Highest index first so the earlier indices stay valid.
        std::sort(removeIdx.begin(), removeIdx.end(), std::greater<size_t>());

        for (size_t idx : removeIdx)
        {
            const MemoryRow& row = bucket[idx];

            std::string escText = row.text;
            CharacterDatabase.EscapeString(escText);

            CharacterDatabase.Execute(SafeFormat(
                "DELETE FROM mod_bot_minds_memory WHERE bot_guid = {} AND {} AND text = '{}' LIMIT 1",
                row.botGuid, SubjectClause(row.subjectGuid), escText));

            bucket.erase(bucket.begin() + idx);
        }
    }

    void InsertRow(const MemoryRow& row)
    {
        std::string escText = row.text;
        std::string escKind = row.kind;
        CharacterDatabase.EscapeString(escText);
        CharacterDatabase.EscapeString(escKind);

        if (row.subjectGuid == 0)
        {
            CharacterDatabase.Execute(SafeFormat(
                "INSERT INTO mod_bot_minds_memory (bot_guid, subject_guid, kind, text, salience) "
                "VALUES ({}, NULL, '{}', '{}', {:.3f})",
                row.botGuid, escKind, escText, row.salience));
        }
        else
        {
            CharacterDatabase.Execute(SafeFormat(
                "INSERT INTO mod_bot_minds_memory (bot_guid, subject_guid, kind, text, salience) "
                "VALUES ({}, {}, '{}', '{}', {:.3f})",
                row.botGuid, row.subjectGuid, escKind, escText, row.salience));
        }
    }
}

std::vector<MemoryEntry> GetRecentMemories(uint64_t botGuid, uint64_t subjectGuid, int n)
{
    std::vector<MemoryEntry> out;
    if (n <= 0)
        return out;

    std::lock_guard<std::mutex> lock(g_MemoryMutex);

    auto botIt = g_Memories.find(botGuid);
    if (botIt == g_Memories.end())
        return out;

    auto subIt = botIt->second.find(subjectGuid);
    if (subIt == botIt->second.end())
        return out;

    const std::deque<MemoryRow>& bucket = subIt->second;
    for (auto it = bucket.rbegin(); it != bucket.rend() && out.size() < static_cast<size_t>(n); ++it)
        out.push_back(ToEntry(*it));

    return out;
}

std::vector<MemoryEntry> GetRelevantMemories(uint64_t botGuid, uint64_t subjectGuid, int n)
{
    std::vector<MemoryEntry> out;
    if (n <= 0)
        return out;

    std::lock_guard<std::mutex> lock(g_MemoryMutex);

    auto botIt = g_Memories.find(botGuid);
    if (botIt == g_Memories.end())
        return out;

    std::vector<const MemoryRow*> candidates;
    auto collect = [&candidates](const std::unordered_map<uint64_t, std::deque<MemoryRow>>& bySubject, uint64_t key)
    {
        auto it = bySubject.find(key);
        if (it != bySubject.end())
            for (const MemoryRow& row : it->second)
                candidates.push_back(&row);
    };

    collect(botIt->second, subjectGuid);
    if (subjectGuid != 0)
        collect(botIt->second, 0);

    std::sort(candidates.begin(), candidates.end(),
        [](const MemoryRow* a, const MemoryRow* b) { return a->salience > b->salience; });

    for (const MemoryRow* row : candidates)
    {
        if (out.size() >= static_cast<size_t>(n))
            break;
        out.push_back(ToEntry(*row));
    }

    return out;
}

void AddMemory(uint64_t botGuid, uint64_t subjectGuid, const std::string& kind,
               const std::string& text, float salience)
{
    if (text.empty())
        return;

    salience = std::max(0.0f, std::min(1.0f, salience));

    std::string safeKind = kind;
    if (safeKind != "event" && safeKind != "fact" && safeKind != "summary")
        safeKind = "event";

    MemoryRow row;
    row.botGuid     = botGuid;
    row.subjectGuid = subjectGuid;
    row.kind        = safeKind;
    row.text        = text;
    row.salience    = salience;

    {
        std::lock_guard<std::mutex> lock(g_MemoryMutex);
        g_Memories[botGuid][subjectGuid].push_back(row);
        InsertRow(row);
    }

    if (g_DebugEnabled)
    {
        LOG_INFO("server.loading",
                 "[BotMinds] Bot {} remembered ({} about subject {}, salience {:.2f}): '{}'",
                 botGuid, safeKind, subjectGuid, salience, text);
    }

    DistillOldMemories(botGuid, subjectGuid);
}

void DistillOldMemories(uint64_t botGuid, uint64_t subjectGuid)
{
    size_t cap = g_MaxMemoriesPerSubject > 0 ? g_MaxMemoriesPerSubject : 50;

    std::lock_guard<std::mutex> lock(g_MemoryMutex);

    auto botIt = g_Memories.find(botGuid);
    if (botIt == g_Memories.end())
        return;

    auto subIt = botIt->second.find(subjectGuid);
    if (subIt == botIt->second.end())
        return;

    PruneSubjectBucket(subIt->second, cap);
}

void LoadMemoriesFromDB()
{
    std::lock_guard<std::mutex> lock(g_MemoryMutex);
    g_Memories.clear();

    QueryResult result = CharacterDatabase.Query(
        "SELECT bot_guid, subject_guid, kind, text, salience "
        "FROM mod_bot_minds_memory ORDER BY id ASC");

    if (!result)
    {
        LOG_INFO("server.loading", "[BotMinds] No stored memories found.");
        return;
    }

    uint32_t count = 0;
    do
    {
        Field* fields = result->Fetch();
        MemoryRow row;
        row.botGuid     = fields[0].Get<uint64_t>();
        row.subjectGuid = fields[1].IsNull() ? 0 : fields[1].Get<uint64_t>();
        row.kind        = fields[2].Get<std::string>();
        row.text        = fields[3].Get<std::string>();
        row.salience    = fields[4].Get<float>();

        g_Memories[row.botGuid][row.subjectGuid].push_back(std::move(row));
        ++count;
    } while (result->NextRow());

    LOG_INFO("server.loading", "[BotMinds] Loaded {} memories.", count);
}
