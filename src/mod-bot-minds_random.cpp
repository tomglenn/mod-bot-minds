#include "mod-bot-minds_random.h"
#include "mod-bot-minds_config.h"
#include "mod-bot-minds_speak.h"
#include "mod-bot-minds_transcript.h"
#include "mod-bot-minds-utilities.h"

#include "Bag.h"
#include "CellImpl.h"
#include "Creature.h"
#include "GameObject.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "QuestDef.h"
#include "Random.h"

#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
    std::unordered_map<uint64_t, time_t> g_NextAmbientTime;

    bool IsBot(Player* player)
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(player);
        return ai && ai->IsBotAI();
    }

    bool RealPlayerWithin(Player* bot, float distance)
    {
        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* candidate = pair.second;
            if (!candidate || IsBot(candidate) || !candidate->IsInWorld())
                continue;
            if (candidate->GetMapId() == bot->GetMapId() && bot->GetDistance(candidate) <= distance)
                return true;
        }
        return false;
    }

    // Nearest interesting thing around the bot, described plainly. The prompt
    // decides what to do with it.
    std::vector<std::string> ObserveSurroundings(Player* bot)
    {
        std::vector<std::string> observations;

        Unit* nearbyUnit = nullptr;
        {
            Acore::AnyUnitInObjectRangeCheck check(bot, g_SayDistance);
            Acore::UnitSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, nearbyUnit, check);
            Cell::VisitObjects(bot, searcher, g_SayDistance);
        }

        if (nearbyUnit && nearbyUnit->GetTypeId() == TYPEID_UNIT)
        {
            Creature* creature = nearbyUnit->ToCreature();
            if (creature->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
                observations.push_back(SafeFormat("{} is selling wares nearby", creature->GetName()));
            else if (creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
                observations.push_back(SafeFormat("{} is standing here with quests to give", creature->GetName()));
            else
                observations.push_back(SafeFormat("a {} is prowling around nearby", creature->GetName()));
        }

        {
            GameObject* nearbyObject = nullptr;
            Acore::GameObjectInRangeCheck check(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), g_SayDistance);
            Acore::GameObjectSearcher<Acore::GameObjectInRangeCheck> searcher(bot, nearbyObject, check);
            Cell::VisitObjects(bot, searcher, g_SayDistance);
            if (nearbyObject)
                observations.push_back(SafeFormat("there is a {} here", nearbyObject->GetName()));
        }

        {
            std::vector<Item*> equipped;
            for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                if (Item* item = bot->GetItemByPos(slot))
                    equipped.push_back(item);

            if (!equipped.empty())
            {
                Item* item = equipped[urand(0, equipped.size() - 1)];
                observations.push_back(SafeFormat("you are using {}", item->GetTemplate()->Name1));
            }
        }

        {
            int freeSlots = 0;
            for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
                if (!bot->GetItemByPos(slot))
                    ++freeSlots;
            for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
                if (Bag* bag = bot->GetBagByPos(slot))
                    freeSlots += bag->GetFreeSlots();

            if (freeSlots <= 4)
                observations.push_back(SafeFormat("your bags are nearly full, {} slots left", freeSlots));
        }

        if (bot->GetMap() && bot->GetMap()->IsDungeon())
            observations.push_back(SafeFormat("you are inside {}", bot->GetMap()->GetMapName()));

        {
            std::vector<std::string> unfinished;
            for (auto const& status : bot->getQuestStatusMap())
            {
                if (status.second.Status != QUEST_STATUS_INCOMPLETE)
                    continue;
                if (Quest const* quest = sObjectMgr->GetQuestTemplate(status.first))
                    unfinished.push_back(quest->GetTitle());
            }

            if (!unfinished.empty())
                observations.push_back(SafeFormat("your quest {} is still unfinished",
                                                  unfinished[urand(0, unfinished.size() - 1)]));
        }

        return observations;
    }

    const char* PickAngle()
    {
        static const char* angles[] = {
            "complain about it",
            "make a dry observation",
            "ask the others what they think",
            "say what you are going to do next",
            "mention how the levelling is going",
            "say something a bit sarcastic"
        };

        return angles[urand(0, sizeof(angles) / sizeof(angles[0]) - 1)];
    }
}

BotMindsAmbientChatter::BotMindsAmbientChatter() : WorldScript("BotMindsAmbientChatter") {}

void BotMindsAmbientChatter::OnUpdate(uint32 diff)
{
    if (!g_Enable || !g_EnableAmbientChatter)
        return;

    static uint32 timer = 0;
    if (timer > diff)
    {
        timer -= diff;
        return;
    }
    timer = 30000;

    const time_t now = time(nullptr);

    for (auto const& pair : ObjectAccessor::GetPlayers())
    {
        Player* bot = pair.second;
        if (!bot || !IsBot(bot) || !bot->IsInWorld() || bot->IsBeingTeleported())
            continue;
        if (g_DisableRepliesInCombat && bot->IsInCombat())
            continue;

        const uint64_t guid = bot->GetGUID().GetRawValue();

        // Cheap gates first. The audience check walks every player, and the
        // surroundings scan hits the grid, so neither runs until this bot is
        // actually due to say something.
        auto scheduled = g_NextAmbientTime.find(guid);
        if (scheduled == g_NextAmbientTime.end())
        {
            g_NextAmbientTime[guid] = now + urand(g_AmbientMinIntervalSec, g_AmbientMaxIntervalSec);
            continue;
        }

        if (now < scheduled->second)
            continue;

        g_NextAmbientTime[guid] = now + urand(g_AmbientMinIntervalSec, g_AmbientMaxIntervalSec);

        if (urand(0, 99) >= g_AmbientChance)
            continue;

        // Idle chatter is only worth paying for within earshot of a person.
        // Guild membership deliberately does not qualify a bot here: that let
        // every guilded bot in the world chatter into guild chat.
        if (!RealPlayerWithin(bot, g_AmbientPlayerDistance))
            continue;

        std::vector<std::string> observations = ObserveSurroundings(bot);

        std::string situation;
        if (observations.empty())
            situation = "nothing much is happening";
        else
            situation = observations[urand(0, observations.size() - 1)];

        // Grouped bots talk to the group, everyone else says it out loud.
        ChatScope scope = bot->GetGroup() ? ChatScope::Party : ChatScope::Say;

        TurnRequest request;
        request.bot     = bot;
        request.kind    = TurnKind::Ambient;
        request.key     = MakeScope(scope, bot);
        request.trigger = SafeFormat("{}. Take this angle: {}.", situation, PickAngle());

        RequestBotTurn(request, /*forced=*/false);
    }
}
