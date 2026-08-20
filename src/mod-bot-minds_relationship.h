#ifndef MOD_BOT_MINDS_RELATIONSHIP_H
#define MOD_BOT_MINDS_RELATIONSHIP_H

#include <string>
#include <cstdint>

// --------------------------------------------
// Relationship: how a bot feels about another actor (player or bot).
// affinity is clamped to [-1, 1] (-1 = hostile, 0 = neutral, 1 = close).
// --------------------------------------------
struct Relationship
{
    float       affinity = 0.0f;
    std::string reason;
    uint32_t    interactionCount = 0;
};

// Return the relationship a bot has toward another actor. If none exists,
// returns a default (neutral) Relationship.
Relationship GetRelationship(uint64_t botGuid, uint64_t otherGuid);

// Apply a change to a relationship: adjust affinity (clamped to [-1, 1]),
// set the reason, increment the interaction count, and upsert the row.
void ApplyRelationshipDelta(uint64_t botGuid, uint64_t otherGuid, bool otherIsBot,
                            float affinityChange, const std::string& reason);

// Load all relationships from the database into the in-memory cache.
void LoadRelationshipsFromDB();

// Upsert all dirty relationships back to the database.
void FlushRelationshipsToDB();

#endif // MOD_BOT_MINDS_RELATIONSHIP_H
