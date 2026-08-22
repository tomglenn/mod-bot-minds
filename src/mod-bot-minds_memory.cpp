#include "mod-bot-minds_memory.h"
#include "mod-bot-minds-utilities.h"
#include "Log.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "Field.h"
#include <fmt/core.h>
#include <algorithm>
#include <cmath>
#include <ctime>
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
//
// The one exception is the record of a memory having been used, which is a
// ranking hint rather than content and is flushed on a timer.
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
        time_t      createdAt = 0;
        time_t      lastReferenced = 0;   // 0 == never went into a prompt
        uint32_t    refCount = 0;
    };

    // A use of a memory that has not reached the database yet. It copies the
    // row's identity instead of pointing at it, because pruning can drop the row
    // before the next flush.
    struct PendingRef
    {
        uint64_t    botGuid = 0;
        uint64_t    subjectGuid = 0;
        std::string text;
        time_t      lastReferenced = 0;
        uint32_t    refCount = 0;
    };

    // Tunables. Hard-coded until they earn config entries.
    constexpr double   kHalfLifeSeconds     = 72.0 * 3600.0;   // three days, a couple of play sessions
    constexpr float    kStaleFloor          = 0.5f;            // age can take at most half a memory's score
    constexpr float    kUseFloorBonus       = 0.2f;            // a memory the bot keeps recalling fades slower
    constexpr uint32_t kUseHalfCount        = 4;               // uses that earn half the bonus
    constexpr time_t   kReferenceCooldown   = 300;             // one conversation is not many recollections
    constexpr time_t   kReferenceFlushSec   = 300;             // how often uses reach the database
    constexpr size_t   kProtectedRecent     = 5;               // newest rows survive however dull they are
    constexpr float    kSummaryMaxSalience  = 0.5f;            // a summary is a middling memory, never a headline
    constexpr size_t   kSummaryMaxChars     = 300;
    constexpr size_t   kFragmentMaxChars    = 60;

    const std::string kSummaryPrefix = "Older, hazy memories: ";

    // botGuid -> subjectGuid -> ordered rows (oldest first).
    std::unordered_map<uint64_t, std::unordered_map<uint64_t, std::deque<MemoryRow>>> g_Memories;
    std::vector<PendingRef> g_PendingRefs;
    time_t                  g_LastRefFlush = 0;
    std::mutex              g_MemoryMutex;

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

    // What a memory is worth right now:
    //
    //     weight = keep + (1 - keep) * 2^(-age / halfLife)
    //     score  = salience * weight
    //
    // Age runs from the last time the memory was used rather than from when it
    // was made, because recalling something keeps it fresh. `keep` floors the
    // weight, so age can never take more than half a memory's score and anything
    // worth more than twice as much as a rival wins however old it is: "she
    // pulled me out of Stonewatch" (0.9) still beats "I healed Azelus" (0.3) a
    // month later, while two dull lines are separated by which happened lately.
    // Repeated use lifts the floor, so the things a bot keeps coming back to
    // fade slower still.
    float EffectiveScore(const MemoryRow& row, time_t now)
    {
        time_t anchor = std::max(row.createdAt, row.lastReferenced);
        double age    = (anchor > 0 && now > anchor) ? static_cast<double>(now - anchor) : 0.0;

        // Saturating, so a busy bucket cannot push the floor up to 1 and stop
        // anything ever ageing.
        float used = static_cast<float>(row.refCount) / static_cast<float>(row.refCount + kUseHalfCount);
        float keep = kStaleFloor + kUseFloorBonus * used;

        float weight = keep + (1.0f - keep) * static_cast<float>(std::pow(0.5, age / kHalfLifeSeconds));
        return row.salience * weight;
    }

    // Caller holds g_MemoryMutex.
    void WritePendingRefs()
    {
        for (const PendingRef& ref : g_PendingRefs)
        {
            std::string escText = ref.text;
            CharacterDatabase.EscapeString(escText);

            CharacterDatabase.Execute(SafeFormat(
                "UPDATE mod_bot_minds_memory SET last_referenced = FROM_UNIXTIME({}), ref_count = {} "
                "WHERE bot_guid = {} AND {} AND text = '{}'",
                static_cast<uint64_t>(ref.lastReferenced), ref.refCount,
                ref.botGuid, SubjectClause(ref.subjectGuid), escText));
        }

        g_PendingRefs.clear();
    }

    // Caller holds g_MemoryMutex. The getters run every time a prompt is built,
    // so a read must not become a write. Uses are batched and written at most
    // once per interval; losing a few minutes of them to a hard restart costs
    // nothing, since they only nudge ranking.
    void FlushRefsIfDue(time_t now)
    {
        if (g_PendingRefs.empty())
            return;
        if (g_LastRefFlush != 0 && now - g_LastRefFlush < kReferenceFlushSec)
            return;

        WritePendingRefs();
        g_LastRefFlush = now;
    }

    // Caller holds g_MemoryMutex. Counting every prompt would let one
    // conversation inflate what a memory is worth, so a row counts once per
    // cooldown however often it is used inside it.
    void MarkReferenced(MemoryRow& row, time_t now)
    {
        if (row.lastReferenced != 0 && now - row.lastReferenced < kReferenceCooldown)
            return;

        row.lastReferenced = now;
        ++row.refCount;

        PendingRef ref;
        ref.botGuid        = row.botGuid;
        ref.subjectGuid    = row.subjectGuid;
        ref.text           = row.text;
        ref.lastReferenced = row.lastReferenced;
        ref.refCount       = row.refCount;
        g_PendingRefs.push_back(std::move(ref));
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

    // Caller holds g_MemoryMutex. Returns true when the bot already holds this
    // memory word for word, in which case it is refreshed and moved to the back
    // rather than stored again: bots retell the same small event constantly, and
    // thirty copies of one line would push out everything else. Keeping the text
    // unique also keeps the database updates unambiguous, since text is what
    // identifies a row here.
    bool RefreshExisting(std::deque<MemoryRow>& bucket, const std::string& text, float salience, time_t now)
    {
        for (size_t i = 0; i < bucket.size(); ++i)
        {
            if (bucket[i].kind == "summary" || bucket[i].text != text)
                continue;

            MemoryRow row = bucket[i];
            row.salience  = std::max(row.salience, salience);
            row.createdAt = now;

            std::string escText = row.text;
            CharacterDatabase.EscapeString(escText);

            CharacterDatabase.Execute(SafeFormat(
                "UPDATE mod_bot_minds_memory SET salience = {:.3f}, created_at = FROM_UNIXTIME({}) "
                "WHERE bot_guid = {} AND {} AND text = '{}'",
                row.salience, static_cast<uint64_t>(now),
                row.botGuid, SubjectClause(row.subjectGuid), escText));

            bucket.erase(bucket.begin() + i);
            bucket.push_back(std::move(row));
            return true;
        }

        return false;
    }

    // The opening words of a memory, enough to recognise it by later. The cut can
    // land inside a multi-byte character, so the result is sanitised.
    std::string Fragment(const std::string& text)
    {
        if (text.size() <= kFragmentMaxChars)
            return text;

        std::string cut = text.substr(0, kFragmentMaxChars);
        size_t lastSpace = cut.rfind(' ');
        if (lastSpace != std::string::npos && lastSpace > kFragmentMaxChars / 2)
            cut.resize(lastSpace);

        return SanitizeUTF8(cut);
    }

    // Appends fragments to a summary body, oldest first, dropping whole fragments
    // off the front once it would get too long. Bounded so a bucket that has been
    // trimmed a hundred times still holds one readable line.
    std::string MergeSummaryBody(const std::string& existing, const std::vector<std::string>& fragments)
    {
        std::string body = existing;
        for (const std::string& fragment : fragments)
        {
            if (fragment.empty())
                continue;
            if (!body.empty())
                body += "; ";
            body += fragment;
        }

        while (body.size() > kSummaryMaxChars)
        {
            size_t next = body.find("; ");
            if (next == std::string::npos)
            {
                body = SanitizeUTF8(body.substr(0, kSummaryMaxChars));
                break;
            }
            body.erase(0, next + 2);
        }

        return body;
    }

    // Caller holds g_MemoryMutex. Keeps the gist of memories that are about to be
    // dropped by folding their opening words into one summary row per
    // (bot, subject), so a long-lived bucket thins out instead of forgetting
    // wholesale. Deterministic on purpose: this runs on whichever thread wrote
    // the memory, which has no provider to call and no business blocking on one.
    void FoldIntoSummary(std::deque<MemoryRow>& bucket, uint64_t botGuid, uint64_t subjectGuid,
                         const std::vector<std::string>& fragments, float salience, time_t now)
    {
        if (fragments.empty())
            return;

        for (MemoryRow& row : bucket)
        {
            if (row.kind != "summary")
                continue;

            std::string body = row.text.compare(0, kSummaryPrefix.size(), kSummaryPrefix) == 0
                ? row.text.substr(kSummaryPrefix.size())
                : row.text;

            std::string escOld = row.text;
            CharacterDatabase.EscapeString(escOld);

            row.text     = kSummaryPrefix + MergeSummaryBody(body, fragments);
            row.salience = std::min(std::max(row.salience, salience), kSummaryMaxSalience);

            std::string escNew = row.text;
            CharacterDatabase.EscapeString(escNew);

            // created_at is left alone deliberately. A summary that renewed
            // itself on every trim would never age and would sit in every prompt
            // for ever.
            CharacterDatabase.Execute(SafeFormat(
                "UPDATE mod_bot_minds_memory SET text = '{}', salience = {:.3f} "
                "WHERE bot_guid = {} AND {} AND kind = 'summary' AND text = '{}' LIMIT 1",
                escNew, row.salience, botGuid, SubjectClause(subjectGuid), escOld));

            return;
        }

        MemoryRow row;
        row.botGuid     = botGuid;
        row.subjectGuid = subjectGuid;
        row.kind        = "summary";
        row.text        = kSummaryPrefix + MergeSummaryBody("", fragments);
        row.salience    = std::min(salience, kSummaryMaxSalience);
        row.createdAt   = now;

        InsertRow(row);

        // Summaries sit at the front of a bucket, which is the oldest end, so
        // they never crowd out what just happened.
        bucket.push_front(std::move(row));
    }

    // Caller holds g_MemoryMutex. Drops the rows worth least until at most `cap`
    // remain, by effective score rather than raw salience, so a bucket that has
    // been running for weeks keeps the memories that matter instead of the last
    // thirty pieces of trivia. Summaries are never dropped, and neither are the
    // newest few rows, which are what the bot needs to talk about right now.
    // Rows are matched in the database by their text, which is what identifies a
    // memory in practice.
    void PruneSubjectBucket(std::deque<MemoryRow>& bucket, uint64_t botGuid, uint64_t subjectGuid, size_t cap)
    {
        if (cap == 0 || bucket.size() <= cap)
            return;

        time_t now = time(nullptr);

        bool hasSummary = std::any_of(bucket.begin(), bucket.end(),
                                      [](const MemoryRow& row) { return row.kind == "summary"; });

        // A summary that does not exist yet needs a slot of its own, otherwise
        // the bucket would sit one row over cap and be pruned again on the very
        // next memory.
        size_t target = (!hasSummary && cap >= 2) ? cap - 1 : cap;
        if (bucket.size() <= target)
            return;

        // Scaled to the cap so a small cap does not end up protecting the whole
        // bucket and pruning nothing.
        size_t protect       = std::min(kProtectedRecent, cap / 4);
        size_t protectedFrom = bucket.size() > protect ? bucket.size() - protect : 0;

        std::vector<size_t> prunable;
        prunable.reserve(bucket.size());
        for (size_t i = 0; i < protectedFrom; ++i)
        {
            if (bucket[i].kind != "summary")
                prunable.push_back(i);
        }

        std::sort(prunable.begin(), prunable.end(),
            [&bucket, now](size_t a, size_t b)
            {
                float scoreA = EffectiveScore(bucket[a], now);
                float scoreB = EffectiveScore(bucket[b], now);
                if (scoreA != scoreB)
                    return scoreA < scoreB;
                return a < b;
            });

        size_t toRemove = std::min(bucket.size() - target, prunable.size());
        if (toRemove == 0)
            return;

        std::vector<size_t> removeIdx(prunable.begin(), prunable.begin() + toRemove);
        // Chronological, so the summary reads in the order things happened.
        std::sort(removeIdx.begin(), removeIdx.end());

        std::vector<std::string> fragments;
        fragments.reserve(removeIdx.size());
        float droppedSalience = 0.0f;
        for (size_t idx : removeIdx)
        {
            fragments.push_back(Fragment(bucket[idx].text));
            droppedSalience = std::max(droppedSalience, bucket[idx].salience);
        }

        // Highest index first so the earlier indices stay valid.
        for (auto it = removeIdx.rbegin(); it != removeIdx.rend(); ++it)
        {
            size_t idx = *it;
            const MemoryRow& row = bucket[idx];

            std::string escText = row.text;
            CharacterDatabase.EscapeString(escText);

            CharacterDatabase.Execute(SafeFormat(
                "DELETE FROM mod_bot_minds_memory WHERE bot_guid = {} AND {} AND text = '{}' LIMIT 1",
                row.botGuid, SubjectClause(row.subjectGuid), escText));

            bucket.erase(bucket.begin() + idx);
        }

        FoldIntoSummary(bucket, botGuid, subjectGuid, fragments, droppedSalience, now);
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

    time_t now = time(nullptr);

    std::deque<MemoryRow>& bucket = subIt->second;
    for (auto it = bucket.rbegin(); it != bucket.rend() && out.size() < static_cast<size_t>(n); ++it)
    {
        MarkReferenced(*it, now);
        out.push_back(ToEntry(*it));
    }

    FlushRefsIfDue(now);

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

    time_t now = time(nullptr);

    std::vector<MemoryRow*> candidates;
    auto collect = [&candidates](std::unordered_map<uint64_t, std::deque<MemoryRow>>& bySubject, uint64_t key)
    {
        auto it = bySubject.find(key);
        if (it != bySubject.end())
            for (MemoryRow& row : it->second)
                candidates.push_back(&row);
    };

    collect(botIt->second, subjectGuid);
    if (subjectGuid != 0)
        collect(botIt->second, 0);

    std::sort(candidates.begin(), candidates.end(),
        [now](const MemoryRow* a, const MemoryRow* b)
        {
            return EffectiveScore(*a, now) > EffectiveScore(*b, now);
        });

    for (MemoryRow* row : candidates)
    {
        if (out.size() >= static_cast<size_t>(n))
            break;
        MarkReferenced(*row, now);
        out.push_back(ToEntry(*row));
    }

    FlushRefsIfDue(now);

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

    time_t now = time(nullptr);

    MemoryRow row;
    row.botGuid     = botGuid;
    row.subjectGuid = subjectGuid;
    row.kind        = safeKind;
    row.text        = text;
    row.salience    = salience;
    row.createdAt   = now;

    {
        std::lock_guard<std::mutex> lock(g_MemoryMutex);

        std::deque<MemoryRow>& bucket = g_Memories[botGuid][subjectGuid];
        if (!RefreshExisting(bucket, row.text, row.salience, now))
        {
            bucket.push_back(row);
            InsertRow(row);
        }
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

    PruneSubjectBucket(subIt->second, botGuid, subjectGuid, cap);
}

void FlushMemoryReferences()
{
    std::lock_guard<std::mutex> lock(g_MemoryMutex);

    WritePendingRefs();
    g_LastRefFlush = time(nullptr);
}

void LoadMemoriesFromDB()
{
    std::lock_guard<std::mutex> lock(g_MemoryMutex);
    g_Memories.clear();
    g_PendingRefs.clear();
    g_LastRefFlush = time(nullptr);

    QueryResult result = CharacterDatabase.Query(
        "SELECT bot_guid, subject_guid, kind, text, salience, UNIX_TIMESTAMP(created_at), "
        "UNIX_TIMESTAMP(last_referenced), ref_count "
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
        row.botGuid        = fields[0].Get<uint64_t>();
        row.subjectGuid    = fields[1].IsNull() ? 0 : fields[1].Get<uint64_t>();
        row.kind           = fields[2].Get<std::string>();
        row.text           = fields[3].Get<std::string>();
        row.salience       = fields[4].Get<float>();
        row.createdAt      = fields[5].IsNull() ? 0 : static_cast<time_t>(fields[5].Get<uint64_t>());
        row.lastReferenced = fields[6].IsNull() ? 0 : static_cast<time_t>(fields[6].Get<uint64_t>());
        row.refCount       = fields[7].Get<uint32_t>();

        g_Memories[row.botGuid][row.subjectGuid].push_back(std::move(row));
        ++count;
    } while (result->NextRow());

    // Rows come back in insertion order, which puts a summary wherever it
    // happened to be written. Move summaries to the front of each bucket so a
    // restart does not turn them into the bot's most recent thought.
    for (auto& botEntry : g_Memories)
    {
        for (auto& subjectEntry : botEntry.second)
        {
            std::stable_partition(subjectEntry.second.begin(), subjectEntry.second.end(),
                                  [](const MemoryRow& row) { return row.kind == "summary"; });
        }
    }

    LOG_INFO("server.loading", "[BotMinds] Loaded {} memories.", count);
}
