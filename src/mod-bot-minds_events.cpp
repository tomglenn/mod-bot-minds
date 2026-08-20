#include "mod-bot-minds_events.h"
#include "mod-bot-minds_config.h"
#include "mod-bot-minds_speak.h"
#include "mod-bot-minds_transcript.h"
#include "mod-bot-minds-utilities.h"

#include "Creature.h"
#include "Group.h"
#include "Guild.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "QuestDef.h"
#include "Random.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

#include <algorithm>
#include <vector>

namespace
{
    bool IsBot(Player* player)
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(player);
        return ai && ai->IsBotAI();
    }

    bool RealPlayerNearby(Player* actor)
    {
        if (!IsBot(actor))
            return true;   // the actor is the audience

        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* candidate = pair.second;
            if (!candidate || IsBot(candidate) || !candidate->IsInWorld())
                continue;
            if (candidate->GetMapId() == actor->GetMapId()
                && candidate->GetDistance(actor) <= g_EventDistance)
                return true;
        }
        return false;
    }

    bool RealPlayerInGuild(uint32 guildId)
    {
        if (guildId == 0)
            return false;

        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* candidate = pair.second;
            if (!candidate || IsBot(candidate) || !candidate->IsInWorld())
                continue;
            if (candidate->GetGuildId() == guildId)
                return true;
        }
        return false;
    }
}

void BotMindsEvents::Dispatch(Player* actor, const std::string& description, uint32_t chance, uint32 guildId)
{
    if (!g_Enable || !g_EnableEventChatter || !actor || description.empty())
        return;

    // One roll for the event itself, so a busy moment does not become a chorus.
    if (chance == 0 || urand(0, 99) >= chance)
        return;

    const bool guildEvent = guildId != 0;

    if (guildEvent && (!g_EnableGuildChatter || !RealPlayerInGuild(guildId)))
        return;

    if (!guildEvent && !RealPlayerNearby(actor))
        return;

    std::vector<Player*> candidates;
    for (auto const& pair : ObjectAccessor::GetPlayers())
    {
        Player* bot = pair.second;
        if (!bot || bot == actor || !IsBot(bot) || !bot->IsInWorld() || bot->IsBeingTeleported())
            continue;
        if (g_DisableRepliesInCombat && bot->IsInCombat())
            continue;

        if (guildEvent)
        {
            if (bot->GetGuildId() != guildId)
                continue;
        }
        else
        {
            if (bot->GetMapId() != actor->GetMapId() || bot->GetDistance(actor) > g_EventDistance)
                continue;
        }

        candidates.push_back(bot);
    }

    if (candidates.empty())
        return;

    std::shuffle(candidates.begin(), candidates.end(), RandomEngine::Instance());

    uint32_t spoke = 0;
    for (Player* bot : candidates)
    {
        if (spoke >= g_EventMaxBots)
            break;

        // Guild events go to guild chat, group-mates talk in party, everyone else
        // reacts out loud where the event happened.
        ChatScope scope = ChatScope::Say;
        if (guildEvent)
            scope = ChatScope::Guild;
        else if (bot->GetGroup() && bot->GetGroup() == actor->GetGroup())
            scope = ChatScope::Party;

        TurnRequest request;
        request.bot     = bot;
        request.other   = actor;
        request.kind    = TurnKind::Event;
        request.key     = MakeScope(scope, bot);
        request.trigger = description;

        if (RequestBotTurn(request, /*forced=*/false))
            ++spoke;
    }

    if (g_DebugEnabled && spoke > 0)
        LOG_INFO("server.loading", "[BotMinds] Event '{}' picked up by {} bot(s).", description, spoke);
}

// === Script hooks ===

ChatOnKill::ChatOnKill() : PlayerScript("ChatOnKill") {}

void ChatOnKill::OnPlayerCreatureKill(Player* killer, Creature* victim)
{
    if (!killer || !victim)
        return;

    BotMindsEvents::Dispatch(killer, SafeFormat("{} killed {}", killer->GetName(), victim->GetName()),
                             g_EventChanceKill);
}

void ChatOnKill::OnPlayerPVPKill(Player* killer, Player* killed)
{
    if (!killer || !killed)
        return;

    BotMindsEvents::Dispatch(killer, SafeFormat("{} killed {} in a fight", killer->GetName(), killed->GetName()),
                             g_EventChancePvPKill);
}

void ChatOnKill::OnPlayerCreatureKilledByPet(Player* owner, Creature* victim)
{
    if (!owner || !victim)
        return;

    BotMindsEvents::Dispatch(owner, SafeFormat("{}'s pet killed {}", owner->GetName(), victim->GetName()),
                             g_EventChanceKill);
}

ChatOnLoot::ChatOnLoot() : PlayerScript("ChatOnLoot") {}

void ChatOnLoot::OnPlayerStoreNewItem(Player* player, Item* item, uint32 /*count*/)
{
    if (!player || !item || !item->GetTemplate())
        return;

    ItemTemplate const* itemTemplate = item->GetTemplate();

    // Only gear worth talking about. Grey and white drops are noise.
    if (itemTemplate->Quality < ITEM_QUALITY_RARE)
        return;

    const bool isEpic = itemTemplate->Quality >= ITEM_QUALITY_EPIC;

    BotMindsEvents::Dispatch(player, SafeFormat("{} looted {}", player->GetName(), itemTemplate->Name1),
                             g_EventChanceLoot);

    if (player->GetGuildId() != 0 && (itemTemplate->Class == ITEM_CLASS_WEAPON || itemTemplate->Class == ITEM_CLASS_ARMOR))
    {
        BotMindsEvents::Dispatch(player,
                                 SafeFormat("{} got {} gear: {}", player->GetName(),
                                            isEpic ? "epic" : "rare", itemTemplate->Name1),
                                 isEpic ? g_EventChanceGuildEpic : g_EventChanceGuildRare,
                                 player->GetGuildId());
    }
}

ChatOnDeath::ChatOnDeath() : PlayerScript("ChatOnDeath") {}

void ChatOnDeath::OnPlayerJustDied(Player* player)
{
    if (!player)
        return;

    BotMindsEvents::Dispatch(player, SafeFormat("{} died", player->GetName()), g_EventChanceDeath);
}

ChatOnQuest::ChatOnQuest() : PlayerScript("ChatOnQuest") {}

void ChatOnQuest::OnPlayerCompleteQuest(Player* player, Quest const* quest)
{
    if (!player || !quest)
        return;

    BotMindsEvents::Dispatch(player, SafeFormat("{} finished the quest {}", player->GetName(), quest->GetTitle()),
                             g_EventChanceQuest);
}

ChatOnLearn::ChatOnLearn() : PlayerScript("ChatOnLearn") {}

void ChatOnLearn::OnPlayerLearnSpell(Player* player, uint32 spellID)
{
    if (!player)
        return;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellID);
    if (!spellInfo || !spellInfo->SpellName[0] || !*spellInfo->SpellName[0])
        return;

    BotMindsEvents::Dispatch(player, SafeFormat("{} learned {}", player->GetName(), spellInfo->SpellName[0]),
                             g_EventChanceSpell);
}

ChatOnDuel::ChatOnDuel() : PlayerScript("ChatOnDuel") {}

void ChatOnDuel::OnPlayerDuelRequest(Player* target, Player* challenger)
{
    if (!target || !challenger)
        return;

    BotMindsEvents::Dispatch(challenger, SafeFormat("{} challenged {} to a duel", challenger->GetName(), target->GetName()),
                             g_EventChanceDuel);
}

void ChatOnDuel::OnPlayerDuelStart(Player* player1, Player* player2)
{
    if (!player1 || !player2)
        return;

    BotMindsEvents::Dispatch(player1, SafeFormat("{} and {} started duelling", player1->GetName(), player2->GetName()),
                             g_EventChanceDuel);
}

void ChatOnDuel::OnPlayerDuelEnd(Player* winner, Player* loser, DuelCompleteType /*type*/)
{
    if (!winner || !loser)
        return;

    BotMindsEvents::Dispatch(winner, SafeFormat("{} beat {} in a duel", winner->GetName(), loser->GetName()),
                             g_EventChanceDuel);
}

ChatOnLevelUp::ChatOnLevelUp() : PlayerScript("ChatOnLevelUp") {}

void ChatOnLevelUp::OnPlayerLevelChanged(Player* player, uint8 /*oldLevel*/)
{
    if (!player)
        return;

    std::string description = SafeFormat("{} reached level {}", player->GetName(), player->GetLevel());

    BotMindsEvents::Dispatch(player, description, g_EventChanceLevelUp);

    BotMindsEvents::Dispatch(player, description, g_EventChanceGuildLevelUp, player->GetGuildId());
}

ChatOnGuildChange::ChatOnGuildChange() : GuildScript("ChatOnGuildChange") {}

void ChatOnGuildChange::OnAddMember(Guild* guild, Player* player, uint8& /*plRank*/)
{
    if (!guild || !player)
        return;

    BotMindsEvents::Dispatch(player, SafeFormat("{} joined the guild", player->GetName()),
                             g_EventChanceGuildMember, guild->GetId());
}

void ChatOnGuildChange::OnRemoveMember(Guild* guild, Player* player, bool /*isDisbanding*/, bool isKicked)
{
    if (!guild || !player)
        return;

    BotMindsEvents::Dispatch(player,
                             SafeFormat("{} {} the guild", player->GetName(), isKicked ? "was kicked from" : "left"),
                             g_EventChanceGuildMember, guild->GetId());
}
