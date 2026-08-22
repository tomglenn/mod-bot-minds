#include "mod-bot-minds_random.h"
#include "mod-bot-minds_action.h"
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

    // One favour per person per cooldown, so an unprompted buff stays a nice
    // surprise rather than a fixture.
    std::unordered_map<uint64_t, time_t> g_LastFavourAt;

    // The nearest real player this bot could actually do something for, or null.
    Player* FavourCandidate(Player* bot, time_t now)
    {
        if (!g_ActionsEnable || urand(0, 99) >= g_UnpromptedChance)
            return nullptr;

        for (auto const& pair : ObjectAccessor::GetPlayers())
        {
            Player* candidate = pair.second;
            if (!candidate || !candidate->IsInWorld())
                continue;
            if (PlayerbotsMgr::instance().GetPlayerbotAI(candidate))
                continue;
            if (candidate->GetMapId() != bot->GetMapId() || bot->GetDistance(candidate) > g_SayDistance)
                continue;

            auto last = g_LastFavourAt.find(candidate->GetGUID().GetRawValue());
            if (last != g_LastFavourAt.end() && now < last->second + (time_t)g_UnpromptedCooldownSec)
                continue;

            return candidate;
        }

        return nullptr;
    }

    bool IsBot(Player* player)
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(player);
        return ai && ai->IsBotAI();
    }

    // A group-mate always has an audience: party chat reaches the whole group
    // however far apart its members are.
    bool RealPlayerInGroup(Player* bot)
    {
        Group* group = bot->GetGroup();
        if (!group)
            return false;

        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (member && member != bot && !IsBot(member))
                return true;
        }

        return false;
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

    // A bot in somebody's group does not decide where it goes, so "say what you are
    // going to do next" invites it to announce an errand it will never run. Its own
    // words then contradict the fact that it is stood there following you.
    const char* PickAngle(bool underOrders)
    {
        static const char* anyone[] = {
            "complain about it",
            "make a dry observation",
            "ask the others what they think",
            "mention how the levelling is going",
            "say something a bit sarcastic"
        };

        static const char* freeToRoam[] = {
            "say what you are going to do next"
        };

        // One combined range, so a free bot keeps the fuller set of angles.
        const size_t extra = underOrders ? 0 : sizeof(freeToRoam) / sizeof(freeToRoam[0]);
        const size_t total = sizeof(anyone) / sizeof(anyone[0]) + extra;
        const size_t pick  = urand(0, total - 1);

        return pick < sizeof(anyone) / sizeof(anyone[0])
            ? anyone[pick]
            : freeToRoam[pick - sizeof(anyone) / sizeof(anyone[0])];
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

        // Idle chatter needs someone to hear it. A group-mate always qualifies,
        // at any distance, the same way it can already answer party chat from
        // across the map. Everyone else has to be inside say range, since that is
        // what "can a person hear this" means for a line spoken out loud.
        //
        // Guild membership deliberately does not qualify a bot: a party is four
        // bots, a guild can be hundreds, and that let every guilded bot in the
        // world chatter into guild chat.
        const bool groupAudience = RealPlayerInGroup(bot);
        if (!groupAudience && !RealPlayerWithin(bot, g_SayDistance))
            continue;

        // Sometimes, rather than remarking on the scenery, do somebody a good turn.
        // Naming the person as the turn's counterpart is what puts the capability
        // list in the prompt, so the bot can pick a real buff and say why.
        Player* favourFor = FavourCandidate(bot, now);

        // A warrior has nothing to offer. Without this check it would still be told
        // to do someone a good turn, and would promise a buff it does not have.
        if (favourFor)
        {
            ActionMenu offer = BuildActionMenu(bot, favourFor, /*unprompted=*/true);
            if (offer.NothingToVolunteer())
                favourFor = nullptr;
        }

        std::string situation;

        if (favourFor)
        {
            situation = SafeFormat("{} is nearby and you could do them a good turn unasked", favourFor->GetName());
        }
        else
        {
            std::vector<std::string> observations = ObserveSurroundings(bot);
            if (observations.empty())
                situation = "nothing much is happening";
            else
                situation = observations[urand(0, observations.size() - 1)];

            situation = SafeFormat("{}. Take this angle: {}.", situation, PickAngle(groupAudience));
        }

        // Talk to the group when a person is in it, otherwise say it out loud.
        // Checking for a person rather than just a group matters: a bot in an
        // all-bot party used to mutter into a party channel nobody was reading.
        ChatScope scope = groupAudience ? ChatScope::Party : ChatScope::Say;

        TurnRequest request;
        request.bot     = bot;
        request.other   = favourFor;
        request.kind    = TurnKind::Ambient;
        request.key     = MakeScope(scope, bot);
        request.trigger = situation;

        if (RequestBotTurn(request, /*forced=*/false) && favourFor)
            g_LastFavourAt[favourFor->GetGUID().GetRawValue()] = now;
    }
}
