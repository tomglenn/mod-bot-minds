#ifndef MOD_BOT_MINDS_EVENTS_H
#define MOD_BOT_MINDS_EVENTS_H

#include "ScriptMgr.h"
#include "Player.h"

#include <string>

// --------------------------------------------
// World events bots react to. Each hook describes what happened in plain words
// and hands it to the same pipeline that serves conversation, so an event
// reaction sounds like the same person who was talking a minute ago.
// --------------------------------------------
namespace BotMindsEvents
{
    // `description` reads like a sentence: "Tom reached level 32".
    // `chance` is the roll for the event being worth commenting on at all.
    // A non-zero `guildId` sends the reaction to that guild's chat instead of to
    // whoever is standing nearby.
    void Dispatch(Player* actor, const std::string& description, uint32_t chance, uint32 guildId = 0);
}

class ChatOnKill : public PlayerScript
{
public:
    ChatOnKill();
    void OnPlayerCreatureKill(Player* killer, Creature* victim) override;
    void OnPlayerPVPKill(Player* killer, Player* killed) override;
    void OnPlayerCreatureKilledByPet(Player* owner, Creature* victim) override;
};

class ChatOnLoot : public PlayerScript
{
public:
    ChatOnLoot();
    void OnPlayerStoreNewItem(Player* player, Item* item, uint32 count) override;
};

class ChatOnDeath : public PlayerScript
{
public:
    ChatOnDeath();
    void OnPlayerJustDied(Player* player) override;
};

class ChatOnQuest : public PlayerScript
{
public:
    ChatOnQuest();
    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override;
};

class ChatOnLearn : public PlayerScript
{
public:
    ChatOnLearn();
    void OnPlayerLearnSpell(Player* player, uint32 spellID) override;
};

class ChatOnDuel : public PlayerScript
{
public:
    ChatOnDuel();
    void OnPlayerDuelRequest(Player* target, Player* challenger) override;
    void OnPlayerDuelStart(Player* player1, Player* player2) override;
    void OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType type) override;
};

class ChatOnLevelUp : public PlayerScript
{
public:
    ChatOnLevelUp();
    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override;
};

class ChatOnGuildChange : public GuildScript
{
public:
    ChatOnGuildChange();
    void OnAddMember(Guild* guild, Player* player, uint8& plRank) override;
    void OnRemoveMember(Guild* guild, Player* player, bool isDisbanding, bool isKicked) override;
};

#endif // MOD_BOT_MINDS_EVENTS_H
