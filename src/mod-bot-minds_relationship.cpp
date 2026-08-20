#include "mod-bot-minds_relationship.h"
#include "mod-bot-minds-utilities.h"
#include "Log.h"
#include "DatabaseEnv.h"
#include "QueryResult.h"
#include "Field.h"
#include <fmt/core.h>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// Config globals (defined elsewhere by the integrator).
extern bool g_DebugEnabled;

// --------------------------------------------
// In-memory cache and dirty-set.
// Keyed by (botGuid, otherGuid) matching the DB unique key.
// --------------------------------------------
namespace
{
    struct RelationshipRow
    {
        uint64_t    botGuid = 0;
        uint64_t    otherGuid = 0;
        bool        otherIsBot = false;
        float       affinity = 0.0f;
        std::string reason;
        uint32_t    interactionCount = 0;
    };

    struct PairHash
    {
        std::size_t operator()(const std::pair<uint64_t, uint64_t>& p) const
        {
            return std::hash<uint64_t>()(p.first) ^ (std::hash<uint64_t>()(p.second) << 1);
        }
    };

    using Key = std::pair<uint64_t, uint64_t>;

    std::unordered_map<Key, RelationshipRow, PairHash> g_Relationships;
    std::unordered_set<Key, PairHash>                  g_RelationshipDirty;
    std::mutex                                         g_RelationshipMutex;
}

Relationship GetRelationship(uint64_t botGuid, uint64_t otherGuid)
{
    std::lock_guard<std::mutex> lock(g_RelationshipMutex);

    auto it = g_Relationships.find(Key(botGuid, otherGuid));
    if (it == g_Relationships.end())
        return Relationship{};

    Relationship r;
    r.affinity         = it->second.affinity;
    r.reason           = it->second.reason;
    r.interactionCount = it->second.interactionCount;
    return r;
}

void ApplyRelationshipDelta(uint64_t botGuid, uint64_t otherGuid, bool otherIsBot,
                            float affinityChange, const std::string& reason)
{
    std::lock_guard<std::mutex> lock(g_RelationshipMutex);

    Key key(botGuid, otherGuid);
    auto it = g_Relationships.find(key);
    if (it == g_Relationships.end())
    {
        RelationshipRow row;
        row.botGuid    = botGuid;
        row.otherGuid  = otherGuid;
        row.otherIsBot = otherIsBot;
        it = g_Relationships.emplace(key, std::move(row)).first;
    }

    RelationshipRow& row = it->second;
    row.otherIsBot = otherIsBot;
    row.affinity   = std::max(-1.0f, std::min(1.0f, row.affinity + affinityChange));
    if (!reason.empty())
        row.reason = reason;
    ++row.interactionCount;

    g_RelationshipDirty.insert(key);

    if (g_DebugEnabled)
    {
        LOG_INFO("server.loading",
                 "[BotMinds] Relationship bot {} -> {} affinity {:+.2f} => {:.2f} (reason '{}', count {})",
                 botGuid, otherGuid, affinityChange, row.affinity, row.reason, row.interactionCount);
    }
}

void LoadRelationshipsFromDB()
{
    std::lock_guard<std::mutex> lock(g_RelationshipMutex);
    g_Relationships.clear();
    g_RelationshipDirty.clear();

    QueryResult result = CharacterDatabase.Query(
        "SELECT bot_guid, other_guid, other_is_bot, affinity, reason, interaction_count "
        "FROM mod_bot_minds_relationship");

    if (!result)
    {
        LOG_INFO("server.loading", "[BotMinds] No existing relationship data found in database");
        return;
    }

    uint32_t count = 0;
    do
    {
        Field* fields = result->Fetch();
        RelationshipRow row;
        row.botGuid          = fields[0].Get<uint64_t>();
        row.otherGuid        = fields[1].Get<uint64_t>();
        row.otherIsBot       = fields[2].Get<uint8>() != 0;
        row.affinity         = fields[3].Get<float>();
        row.reason           = fields[4].IsNull() ? "" : fields[4].Get<std::string>();
        row.interactionCount = fields[5].Get<uint32_t>();

        g_Relationships[Key(row.botGuid, row.otherGuid)] = std::move(row);
        ++count;
    } while (result->NextRow());

    LOG_INFO("server.loading", "[BotMinds] Loaded {} relationship records from database", count);
}

void FlushRelationshipsToDB()
{
    std::lock_guard<std::mutex> lock(g_RelationshipMutex);

    if (g_RelationshipDirty.empty())
        return;

    for (const Key& key : g_RelationshipDirty)
    {
        auto it = g_Relationships.find(key);
        if (it == g_Relationships.end())
            continue;

        const RelationshipRow& row = it->second;

        std::string escReason = row.reason;
        CharacterDatabase.EscapeString(escReason);

        CharacterDatabase.Execute(SafeFormat(
            "INSERT INTO mod_bot_minds_relationship "
            "(bot_guid, other_guid, other_is_bot, affinity, reason, interaction_count) "
            "VALUES ({}, {}, {}, {:.3f}, '{}', {}) "
            "ON DUPLICATE KEY UPDATE "
            "other_is_bot = VALUES(other_is_bot), affinity = VALUES(affinity), "
            "reason = VALUES(reason), interaction_count = VALUES(interaction_count)",
            row.botGuid, row.otherGuid, row.otherIsBot ? 1 : 0,
            row.affinity, escReason, row.interactionCount));
    }

    if (g_DebugEnabled)
    {
        LOG_INFO("server.loading", "[BotMinds] Flushed {} dirty relationship records to database",
                 static_cast<uint32_t>(g_RelationshipDirty.size()));
    }

    g_RelationshipDirty.clear();
}
