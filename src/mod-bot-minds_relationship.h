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
    uint32_t    lastGiftAt = 0;    // epoch seconds of the last gift, 0 == never
};

// Return the relationship a bot has toward another actor. If none exists,
// returns a default (neutral) Relationship.
Relationship GetRelationship(uint64_t botGuid, uint64_t otherGuid);

// Apply a change to a relationship: adjust affinity (clamped to [-1, 1]),
// set the reason, increment the interaction count, and upsert the row.
void ApplyRelationshipDelta(uint64_t botGuid, uint64_t otherGuid, bool otherIsBot,
                            float affinityChange, const std::string& reason);

// Note that a bot has just given this person something, so the gift cooldown
// survives a restart. Stored on the relationship because that is what it is: a
// fact about the two of them.
void RecordGift(uint64_t botGuid, uint64_t otherGuid);

// Load all relationships from the database into the in-memory cache.
void LoadRelationshipsFromDB();

// Upsert all dirty relationships back to the database.
void FlushRelationshipsToDB();

#endif // MOD_BOT_MINDS_RELATIONSHIP_H
