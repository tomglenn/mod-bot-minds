#include "mod-bot-minds_action.h"
#include "mod-bot-minds_config.h"
#include "mod-bot-minds_memory.h"
#include "mod-bot-minds_relationship.h"
#include "mod-bot-minds-utilities.h"

#include "AiObjectContext.h"
#include "DatabaseEnv.h"
#include "EmoteAction.h"
#include "GenericBuffUtils.h"
#include "Group.h"
#include "Log.h"
#include "Mail.h"
#include "ObjectAccessor.h"
#include "ObjectDefines.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "SharedDefines.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>
#include <deque>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace
{
    std::atomic<uint32_t> g_ActionsPerformed{0};
    std::atomic<uint32_t> g_ActionsFailed{0};

    std::deque<BotAction> g_Pending;
    std::mutex            g_PendingMutex;

    // Bots currently mid-conversation, and when they are free to get on with
    // their day again.
    struct ConversationHold
    {
        uint64_t targetGuid = 0;
        uint32_t remainingMs = 0;
    };

    std::unordered_map<uint64_t, ConversationHold> g_Holds;
    std::mutex                                    g_HoldMutex;

    // When each bot last emoted, so restraint is enforced here rather than trusted
    // to the model. An emote every other sentence stops meaning anything.
    std::unordered_map<uint64_t, time_t> g_LastEmoteAt;
    std::mutex                           g_EmoteMutex;

    // A deliberately short list. These are the gestures that read as conversation
    // rather than performance, and keeping it closed means the model cannot make a
    // bot dance at somebody.
    uint32_t EmoteIdFromName(const std::string& name)
    {
        static const std::unordered_map<std::string, uint32_t> allowed = {
            { "wave",   TEXT_EMOTE_WAVE },
            { "laugh",  TEXT_EMOTE_LAUGH },
            { "nod",    TEXT_EMOTE_NOD },
            { "shrug",  TEXT_EMOTE_SHRUG },
            { "thank",  TEXT_EMOTE_THANK },
            { "cheer",  TEXT_EMOTE_CHEER },
            { "salute", TEXT_EMOTE_SALUTE },
            { "bow",    TEXT_EMOTE_BOW },
            { "sigh",   TEXT_EMOTE_SIGH }
        };

        auto it = allowed.find(name);
        return it == allowed.end() ? 0 : it->second;
    }

    PlayerbotAI* BotAIFor(Player* player)
    {
        PlayerbotAI* ai = PlayerbotsMgr::instance().GetPlayerbotAI(player);
        return (ai && ai->IsBotAI()) ? ai : nullptr;
    }

    std::string Lower(const std::string& text)
    {
        std::string out = text;
        for (char& c : out)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return out;
    }

    // Our own table, because mod-playerbots keeps buff names scattered across
    // per-class strategy registrations with nothing enumerable. Being generous is
    // safe: HasSpell filters it to what this bot actually knows, and the name
    // resolves to the highest rank it has.
    const std::vector<std::string>& BuffsForClass(uint8 cls)
    {
        static const std::vector<std::string> none;
        static const std::vector<std::string> priest  = {
            "power word: fortitude", "divine spirit", "shadow protection", "power word: shield", "fear ward" };
        static const std::vector<std::string> mage    = {
            "arcane intellect", "dampen magic", "amplify magic", "slow fall" };
        static const std::vector<std::string> druid   = { "mark of the wild", "thorns" };
        static const std::vector<std::string> paladin = {
            "blessing of might", "blessing of wisdom", "blessing of kings", "blessing of sanctuary" };
        static const std::vector<std::string> shaman  = { "water walking", "water breathing" };
        static const std::vector<std::string> warlock = { "unending breath" };

        switch (cls)
        {
            case CLASS_PRIEST:  return priest;
            case CLASS_MAGE:    return mage;
            case CLASS_DRUID:   return druid;
            case CLASS_PALADIN: return paladin;
            case CLASS_SHAMAN:  return shaman;
            case CLASS_WARLOCK: return warlock;
            default:            return none;
        }
    }

    const std::vector<std::string>& HealsForClass(uint8 cls)
    {
        static const std::vector<std::string> none;
        static const std::vector<std::string> priest  = {
            "flash heal", "greater heal", "heal", "lesser heal", "renew" };
        static const std::vector<std::string> paladin = { "flash of light", "holy light" };
        static const std::vector<std::string> druid   = { "healing touch", "rejuvenation" };
        static const std::vector<std::string> shaman  = { "healing wave", "lesser healing wave" };

        switch (cls)
        {
            case CLASS_PRIEST:  return priest;
            case CLASS_PALADIN: return paladin;
            case CLASS_DRUID:   return druid;
            case CLASS_SHAMAN:  return shaman;
            default:            return none;
        }
    }

    uint32 ResolveSpellId(PlayerbotAI* botAI, const std::string& name)
    {
        if (!botAI || !botAI->GetAiObjectContext())
            return 0;
        return botAI->GetAiObjectContext()->GetValue<uint32>("spell id", name)->Get();
    }

    // CanCastSpell deliberately ignores power cost, so a bot would otherwise offer
    // buffs it cannot pay for. This closes that gap.
    bool CanAfford(Player* bot, uint32 spellId)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            return false;

        int32 cost = info->CalcPowerCost(bot, info->GetSchoolMask());
        if (cost <= 0)
            return true;

        return bot->GetPower(Powers(info->PowerType)) >= static_cast<uint32>(cost);
    }

    // Deliberately not PlayerbotAI::CanCastSpell. That returns false whenever the
    // bot is moving and the spell has a cast time, and party bots are almost always
    // following you, so every heal silently vanished from every menu. We ask
    // instead whether the bot could cast this standing still, and make it stand
    // still at execution time.
    bool CastableOn(Player* bot, PlayerbotAI* botAI, const std::string& name, Player* target)
    {
        if (!botAI->HasSpell(name))
            return false;

        uint32 spellId = ResolveSpellId(botAI, name);
        if (!spellId)
            return false;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info)
            return false;

        if (ai::spell::HasSpellOrCategoryCooldown(bot, spellId))
            return false;
        if (!ai::buff::HasRequiredReagents(bot, spellId))
            return false;
        if (!CanAfford(bot, spellId))
            return false;

        if (bot != target)
        {
            float range = info->GetMaxRange(true, bot);
            if (range <= 0.0f)
                return false;   // self only
            if (!bot->IsWithinDistInMap(target, range))
                return false;
        }

        return true;
    }

    // "power word: fortitude, 12 minutes left"
    std::string DescribeRemaining(const std::string& name, Aura* aura)
    {
        int32 ms = aura->GetDuration();
        if (ms < 0)
            return name + ", no time limit";

        uint32 minutes = static_cast<uint32>(ms) / 60000u;
        if (minutes == 0)
            return name + ", about to run out";

        return SafeFormat("{}, {} minute{} left", name, minutes, minutes == 1 ? "" : "s");
    }

    std::string FormatMoney(uint32 copper)
    {
        std::ostringstream out;
        uint32 gold   = copper / 10000;
        uint32 silver = (copper % 10000) / 100;
        uint32 rest   = copper % 100;

        if (gold)
            out << gold << "g ";
        if (gold || silver)
            out << silver << "s ";
        out << rest << "c";

        return out.str();
    }

    bool SharesGroup(Player* bot, Player* other)
    {
        return bot->GetGroup() && bot->GetGroup()->IsMember(other->GetGUID());
    }

    // A trade can only complete when playerbots' own trust check will let the bot
    // accept it back (TradeStatusAction.cpp:30), and the core will only open the
    // window at all within TRADE_DISTANCE. Everyone else gets mail.
    bool TradeCanComplete(Player* bot, PlayerbotAI* botAI, Player* other)
    {
        if (sPlayerbotAIConfig.enableRandomBotTrading == 0)
            return false;
        if (!bot->IsWithinDistInMap(other, TRADE_DISTANCE, false))
            return false;
        return botAI->GetMaster() == other || SharesGroup(bot, other);
    }

    void WhisperApology(Player* bot, Player* target, const std::string& text)
    {
        if (bot && target)
            bot->Whisper(text, LANG_UNIVERSAL, target);
    }
}

uint32_t ResolveEmote(uint64_t botGuid, const std::string& name)
{
    if (g_EmoteCooldownSec == 0 || name.empty())
        return 0;

    uint32_t emoteId = EmoteIdFromName(Lower(name));
    if (emoteId == 0)
        return 0;

    const time_t now = time(nullptr);

    std::lock_guard<std::mutex> lock(g_EmoteMutex);

    auto it = g_LastEmoteAt.find(botGuid);
    if (it != g_LastEmoteAt.end() && now - it->second < static_cast<time_t>(g_EmoteCooldownSec))
        return 0;

    g_LastEmoteAt[botGuid] = now;
    return emoteId;
}

ActionKind ActionKindFromName(const std::string& name)
{
    const std::string wanted = Lower(name);

    if (wanted == "buff")      return ActionKind::Buff;
    if (wanted == "heal")      return ActionKind::Heal;
    if (wanted == "give_gold") return ActionKind::GiveGold;
    if (wanted == "follow")    return ActionKind::Follow;
    if (wanted == "stay" || wanted == "wait") return ActionKind::Stay;

    return ActionKind::None;
}

ActionMenu BuildActionMenu(Player* bot, Player* other, bool unprompted)
{
    ActionMenu menu;

    if (!g_ActionsEnable || !bot || !other || bot == other)
        return menu;

    PlayerbotAI* botAI = BotAIFor(bot);
    if (!botAI)
        return menu;

    // Only ever offer things to real people. That is the point of the feature, and
    // it also keeps this function off the API thread: bot-to-bot turns are the ones
    // dispatched from there, and they no longer reach the casting checks below.
    if (BotAIFor(other))
        return menu;

    // Nothing is on offer while fighting, dead, or to the other faction.
    if (bot->IsInCombat() || !bot->IsAlive() || !other->IsAlive() || !bot->IsFriendlyTo(other))
        return menu;

    for (const std::string& name : BuffsForClass(bot->getClass()))
    {
        if (!botAI->HasSpell(name))
            continue;

        if (!CastableOn(bot, botAI, name, other))
            continue;

        // A buff that is already up is still worth offering, because a player whose
        // fortitude has two minutes left will ask for a top-up. It just goes on a
        // separate list, so a bot volunteers only what is actually missing but can
        // still recast on request, and can say how long is left.
        if (Aura* active = botAI->GetAura(name, other))
        {
            menu.refreshable.push_back(name);
            menu.alreadyHave.push_back(DescribeRemaining(name, active));
        }
        else
        {
            menu.buffs.push_back(name);
        }
    }

    // Volunteering a heal to someone at full health is odd. Being asked for one
    // and refusing because they look fine is worse.
    if (!unprompted || other->GetHealthPct() < 90.0f)
    {
        for (const std::string& name : HealsForClass(bot->getClass()))
        {
            if (CastableOn(bot, botAI, name, other))
                menu.heals.push_back(name);
        }
    }

    menu.canTakeOrders = (botAI->GetMaster() == other);

    // Money. The cap is a deliberate design number, not the bot's wallet: bots are
    // created holding hundreds of gold, so "what it can spare" would hand a low
    // level player a fortune.
    if (g_GiftMaxCopper > 0)
    {
        Relationship rel = GetRelationship(bot->GetGUID().GetRawValue(), other->GetGUID().GetRawValue());
        uint32 now = static_cast<uint32_t>(time(nullptr));

        if (rel.affinity < g_GiftMinAffinity)
        {
            menu.goldRefusal = SafeFormat("you barely know {}, so handing over money is not something you would do yet",
                                          other->GetName());
        }
        else if (rel.lastGiftAt != 0 && (now - rel.lastGiftAt) < g_GiftCooldownSec)
        {
            menu.goldRefusal = SafeFormat("you already gave {} money recently and would not do it again so soon",
                                          other->GetName());
        }
        else
        {
            uint32 ceiling = std::min<uint32>(g_GiftMaxCopper, bot->GetLevel() * g_GiftCopperPerLevel);
            ceiling = std::min<uint32>(ceiling, bot->GetMoney() / 2);   // never hand over more than half the purse

            uint32 offer = static_cast<uint32>(ceiling * std::min(1.0f, rel.affinity));
            if (offer > 0)
            {
                menu.maxCopper  = offer;
                menu.giftByMail = !TradeCanComplete(bot, botAI, other);
            }
            else
            {
                menu.goldRefusal = "you have nothing to spare";
            }
        }
    }

    return menu;
}

std::string DescribeActionMenu(const ActionMenu& menu, const std::string& otherName)
{
    if (menu.Empty() && menu.goldRefusal.empty() && menu.alreadyHave.empty())
        return "";

    std::ostringstream out;
    out << "What you can actually do for " << otherName << " right now:\n";

    if (!menu.buffs.empty())
    {
        out << "- Cast on them: ";
        for (size_t i = 0; i < menu.buffs.size(); ++i)
            out << (i ? ", " : "") << menu.buffs[i];
        out << "\n";
    }

    if (!menu.heals.empty())
    {
        out << "- Heal them with: ";
        for (size_t i = 0; i < menu.heals.size(); ++i)
            out << (i ? ", " : "") << menu.heals[i];
        out << "\n";
    }

    if (!menu.alreadyHave.empty())
    {
        out << "- Recast on request: ";
        for (size_t i = 0; i < menu.alreadyHave.size(); ++i)
            out << (i ? "; " : "") << menu.alreadyHave[i];
        out << ". These are up already, so do not push them unasked. If they do ask for one, "
               "cast it again and set the action: a buff they already have is still yours to "
               "recast, and telling them they are fine instead is refusing them.\n";
    }

    if (menu.maxCopper > 0)
    {
        out << "- You would spare up to " << FormatMoney(menu.maxCopper) << " for " << otherName << ".";

        if (menu.giftByMail)
        {
            out << " You are not grouped with them, so you cannot hand it over face to face and"
                   " would have to post it instead. If you do, say so plainly in your reply,"
                   " otherwise they will have no idea to go and check their mailbox.";
        }

        out << "\n";
    }
    else if (!menu.goldRefusal.empty())
    {
        out << "- On money: " << menu.goldRefusal << ".\n";
    }

    if (menu.canTakeOrders)
        out << "- They are in your group. If they tell you to come along, follow them, wait, stay "
               "put, hold on, or anything meaning either of those, set the action to follow or stay. "
               "Saying you will is not the same as doing it.\n";
    else
        out << "- They are not in your group, so you cannot take orders like following or staying put. "
               "If they ask, say you would need to be grouped up first.\n";

    out << "Only offer what is on this list. If they ask for anything else, say no in your own "
           "words, and keep the reason true: use a reason given above if there is one, otherwise "
           "just say you cannot. Never invent an excuse, and never claim someone else already did "
           "it for them.";

    return out.str();
}

bool ValidateAction(const ActionMenu& menu, BotAction& action)
{
    switch (action.kind)
    {
        case ActionKind::None:
            return false;

        case ActionKind::Buff:
        case ActionKind::Heal:
        {
            const std::string wanted = Lower(action.spellName);

            auto matches = [&](const std::vector<std::string>& offered) -> const std::string*
            {
                for (const std::string& name : offered)
                    if (Lower(name) == wanted)
                        return &name;
                return nullptr;
            };

            const std::string* found = (action.kind == ActionKind::Buff)
                ? (matches(menu.buffs) ? matches(menu.buffs) : matches(menu.refreshable))
                : matches(menu.heals);

            if (!found)
                return false;

            action.spellName = *found;   // canonical spelling
            return true;
        }

        case ActionKind::GiveGold:
            if (menu.maxCopper == 0 || action.copper == 0)
                return false;
            action.copper  = std::min(action.copper, menu.maxCopper);
            action.viaMail = menu.giftByMail;
            return true;

        case ActionKind::Follow:
            if (!menu.canTakeOrders)
                return false;
            action.command = "follow";
            return true;

        case ActionKind::Stay:
            if (!menu.canTakeOrders)
                return false;
            action.command = "stay";
            return true;
    }

    return false;
}

void SubmitBotAction(const BotAction& action)
{
    if (!g_ActionsEnable || action.kind == ActionKind::None)
        return;

    std::lock_guard<std::mutex> lock(g_PendingMutex);
    g_Pending.push_back(action);
}

namespace
{
    // How long to wait between opening the trade window and putting coin in it.
    // TradeData::SetMoney pushes SMSG_TRADE_STATUS_EXTENDED straight away, but the
    // player's client has not opened the window yet at that point and drops the
    // contents on the floor. The money ends up set server side and invisible.
    constexpr uint32_t TRADE_FILL_DELAY_MS = 1200;

    // Only ever called when playerbots will let the bot accept the trade back.
    bool OpenTradeWindow(Player* bot, Player* target)
    {
        if (bot->GetTrader() || target->GetTrader())
            return false;

        WorldPacket initiate(CMSG_INITIATE_TRADE);
        initiate << target->GetGUID();
        bot->GetSession()->HandleInitiateTradeOpcode(initiate);

        return bot->GetTrader() == target;
    }

    // How long to leave the coin visible before the bot accepts, so the player sees
    // the money land rather than a tick appearing over an apparently empty window.
    constexpr uint32_t TRADE_ACCEPT_DELAY_MS = 700;

    bool PutGoldInTrade(Player* bot, uint32 copper)
    {
        if (bot->GetMoney() < copper)
            return false;

        WorldPacket gold(CMSG_SET_TRADE_GOLD, 4);
        gold << copper;
        bot->GetSession()->HandleSetTradeGoldOpcode(gold);
        return true;
    }

    // The bot accepts its own side first, deliberately.
    //
    // playerbots' TradeStatusAction::CheckTrade refuses any trade where neither
    // side put an *item* in ("There are no items to trade"), so a random bot will
    // never accept a pure money gift through its own logic. That check only runs
    // when the bot is told the other party accepted, so by accepting first we skip
    // it entirely: the player then just clicks Trade and it completes.
    void AcceptOwnSide(Player* bot)
    {
        WorldPacket accept;
        uint32 status = 0;
        accept << status;
        bot->GetSession()->HandleAcceptTradeOpcode(accept);
    }

    // Strangers cannot complete a trade, so the coin goes in the post. Debit and
    // send in one transaction so the money cannot be duplicated or lost.
    bool MailGold(Player* bot, Player* target, uint32 copper)
    {
        if (bot->GetMoney() < copper)
            return false;

        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        // If the debit fails the money must not be sent, or we would be minting it.
        if (!bot->ModifyMoney(-static_cast<int32>(copper)))
            return false;

        bot->SaveGoldToDB(trans);

        MailDraft(SafeFormat("From {}", bot->GetName()), "Thought you could use this.")
            .AddMoney(copper)
            .SendMailTo(trans, MailReceiver(target), MailSender(bot));

        CharacterDatabase.CommitTransaction(trans);
        return true;
    }

    // Emoting goes through the session, so it queues like everything else.
    void PerformEmote(Player* bot, Player* target, uint32 emoteId)
    {
        WorldPacket data(SMSG_TEXT_EMOTE);
        data << emoteId;
        data << EmoteActionBase::GetNumberOfEmoteVariants(
                    static_cast<TextEmotes>(emoteId), bot->getRace(), bot->getGender());
        data << target->GetGUID();
        bot->GetSession()->HandleTextEmoteOpcode(data);
    }

    // Whether the memories riding on an action get written depends on which of
    // these it ends with, which is why finishing and succeeding are separate.
    enum class Outcome
    {
        Succeeded,   // it happened: commit whatever the bot wanted to remember
        Abandoned,   // it will not happen: drop those memories rather than lie
        Retry,       // transient failure: costs an attempt
        Progressed   // a step of a multi-step action: costs nothing, honours readyInMs
    };

    Outcome Perform(BotAction& action)
    {
        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(action.botGuid));
        Player* target = ObjectAccessor::FindPlayer(ObjectGuid(action.targetGuid));

        if (!bot || !target || !bot->IsInWorld() || !target->IsInWorld())
            return Outcome::Abandoned;   // gone; nothing to apologise for

        PlayerbotAI* botAI = BotAIFor(bot);
        if (!botAI)
            return Outcome::Abandoned;

        switch (action.kind)
        {
            case ActionKind::Buff:
            case ActionKind::Heal:
            {
                if (!bot->IsAlive() || bot->IsInCombat())
                    return Outcome::Abandoned;
                if (bot->GetMapId() != target->GetMapId())
                    return Outcome::Abandoned;

                // CastSpell refuses while the bot is moving, and a following bot
                // is always moving, so plant it before trying.
                if (bot->isMoving())
                    bot->StopMoving();

                if (botAI->CastSpell(action.spellName, target))
                {
                    ++g_ActionsPerformed;
                    if (g_DebugEnabled)
                        LOG_INFO("server.loading", "[BotMinds] {} cast {} on {}.",
                                 bot->GetName(), action.spellName, target->GetName());
                    return Outcome::Succeeded;
                }

                return Outcome::Retry;   // moving, standing up, or on cooldown
            }

            case ActionKind::GiveGold:
            {
                if (action.viaMail)
                {
                    if (!MailGold(bot, target, action.copper))
                        return Outcome::Retry;

                    RecordGift(action.botGuid, action.targetGuid);
                    ++g_ActionsPerformed;

                    // Money arriving silently in the post is just confusing. The bot
                    // was asked to mention it and usually does; this covers the times
                    // it does not.
                    if (!action.mentionedPost)
                    {
                        bot->Whisper(SafeFormat("sent you {} in the post, worth a look at your mailbox",
                                                FormatMoney(action.copper)),
                                     LANG_UNIVERSAL, target);
                    }

                    if (g_DebugEnabled)
                        LOG_INFO("server.loading", "[BotMinds] {} posted {} to {}{}.",
                                 bot->GetName(), FormatMoney(action.copper), target->GetName(),
                                 action.mentionedPost ? "" : " (and had to spell it out)");
                    return Outcome::Succeeded;
                }

                // Open the window first, then come back once the client has it up.
                if (!action.tradeStarted)
                {
                    if (!OpenTradeWindow(bot, target))
                        return Outcome::Retry;

                    action.tradeStarted = true;
                    action.readyInMs    = TRADE_FILL_DELAY_MS;
                    return Outcome::Progressed;
                }

                // They may have closed it or wandered off in the meantime.
                if (bot->GetTrader() != target)
                {
                    if (g_DebugEnabled)
                        LOG_INFO("server.loading", "[BotMinds] {}'s trade with {} went away before the coin went in.",
                                 bot->GetName(), target->GetName());
                    return Outcome::Abandoned;
                }

                // Put the coin in, then come back to accept. Setting the money
                // clears both accept flags, so accepting has to come after it.
                if (!action.goldPlaced)
                {
                    if (!PutGoldInTrade(bot, action.copper))
                        return Outcome::Retry;

                    action.goldPlaced = true;
                    action.readyInMs  = TRADE_ACCEPT_DELAY_MS;

                    if (g_DebugEnabled)
                        LOG_INFO("server.loading", "[BotMinds] {} put {} in the trade window for {}.",
                                 bot->GetName(), FormatMoney(action.copper), target->GetName());
                    return Outcome::Progressed;
                }

                AcceptOwnSide(bot);

                RecordGift(action.botGuid, action.targetGuid);
                ++g_ActionsPerformed;
                if (g_DebugEnabled)
                    LOG_INFO("server.loading", "[BotMinds] {} accepted its side of the trade with {}.",
                             bot->GetName(), target->GetName());
                return Outcome::Succeeded;
            }

            case ActionKind::Follow:
            case ActionKind::Stay:
            {
                // HandleCommand drops anything missing the configured prefix
                // (PlayerbotAI.cpp:969), so synthesised commands have to carry it.
                const std::string command = sPlayerbotAIConfig.commandPrefix + action.command;

                // Telling the party to hold up means the party, not whichever bot
                // happened to answer. One of them speaks, all of them obey.
                uint32_t told = 0;
                if (action.wholeGroup && target->GetGroup())
                {
                    for (GroupReference* ref = target->GetGroup()->GetFirstMember(); ref; ref = ref->next())
                    {
                        Player* member = ref->GetSource();
                        if (!member || member == target)
                            continue;

                        PlayerbotAI* memberAI = BotAIFor(member);
                        if (!memberAI || memberAI->GetMaster() != target)
                            continue;   // not theirs to order about

                        memberAI->HandleCommand(CHAT_MSG_WHISPER, command, target);
                        ++told;
                    }
                }

                if (told == 0)
                {
                    botAI->HandleCommand(CHAT_MSG_WHISPER, command, target);
                    told = 1;
                }

                ++g_ActionsPerformed;
                if (g_DebugEnabled)
                    LOG_INFO("server.loading", "[BotMinds] '{}' sent to {} bot(s) on behalf of {}.",
                             command, told, target->GetName());
                return Outcome::Succeeded;
            }

            case ActionKind::Emote:
                if (bot->IsInCombat() || !bot->IsAlive())
                    return Outcome::Abandoned;

                PerformEmote(bot, target, action.emoteId);
                if (g_DebugEnabled)
                    LOG_INFO("server.loading", "[BotMinds] {} emoted {} at {}.",
                             bot->GetName(), action.emoteId, target->GetName());
                return Outcome::Succeeded;

            case ActionKind::None:
                return Outcome::Abandoned;
        }

        return Outcome::Abandoned;
    }

    // Only now is it true, so only now is it remembered.
    void CommitMemories(const BotAction& action)
    {
        for (const PendingMemory& memory : action.memories)
            AddMemory(action.botGuid, action.targetGuid, memory.kind, memory.text, memory.salience);

        if (action.hasRelationshipChange && action.targetGuid != 0)
        {
            ApplyRelationshipDelta(action.botGuid, action.targetGuid, action.otherIsBot,
                                   action.affinityChange, action.affinityReason);
        }
    }
}

void RunPendingActions(uint32_t diff)
{
    std::deque<BotAction> due;

    {
        std::lock_guard<std::mutex> lock(g_PendingMutex);

        for (auto it = g_Pending.begin(); it != g_Pending.end(); )
        {
            if (it->readyInMs > diff)
            {
                it->readyInMs -= diff;
                ++it;
                continue;
            }

            due.push_back(*it);
            it = g_Pending.erase(it);
        }
    }

    for (BotAction& action : due)
    {
        Outcome outcome = Outcome::Abandoned;

        try
        {
            outcome = Perform(action);
        }
        catch (const std::exception& ex)
        {
            LOG_ERROR("server.loading", "[BotMinds] Exception performing action: {}", ex.what());
            outcome = Outcome::Abandoned;
        }

        if (outcome == Outcome::Succeeded)
        {
            CommitMemories(action);
            continue;
        }

        if (outcome == Outcome::Abandoned)
        {
            if (g_DebugEnabled && !action.memories.empty())
                LOG_INFO("server.loading",
                         "[BotMinds] Action never happened, so {} thing(s) it would have remembered were dropped.",
                         static_cast<uint32_t>(action.memories.size()));
            continue;
        }

        // A step of a multi-step action is progress, not failure: it keeps its own
        // delay and does not eat into the retry budget.
        if (outcome == Outcome::Progressed)
        {
            std::lock_guard<std::mutex> lock(g_PendingMutex);
            g_Pending.push_back(action);
            continue;
        }

        // Transient failure. Give the bot a moment to stand still and try again.
        if (++action.attempt >= g_ActionMaxAttempts)
        {
            ++g_ActionsFailed;

            if (action.promised)
            {
                Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(action.botGuid));
                Player* target = ObjectAccessor::FindPlayer(ObjectGuid(action.targetGuid));
                if (bot && target)
                    WhisperApology(bot, target, "sorry, couldn't manage that just now");
            }

            if (g_DebugEnabled)
                LOG_INFO("server.loading", "[BotMinds] Gave up on an action after {} attempts, "
                                           "so nothing about it was remembered.",
                         static_cast<uint32_t>(action.attempt));
            continue;
        }

        action.readyInMs = 600;

        std::lock_guard<std::mutex> lock(g_PendingMutex);
        g_Pending.push_back(action);
    }
}

void HoldStillForConversation(uint64_t botGuid, uint64_t targetGuid)
{
    if (g_ConversationHoldSec == 0 || botGuid == 0 || targetGuid == 0)
        return;

    std::lock_guard<std::mutex> lock(g_HoldMutex);

    ConversationHold& hold = g_Holds[botGuid];
    hold.targetGuid  = targetGuid;
    hold.remainingMs = g_ConversationHoldSec * 1000;   // each new line refreshes it
}

void RunConversationHolds(uint32_t diff)
{
    std::lock_guard<std::mutex> lock(g_HoldMutex);

    for (auto it = g_Holds.begin(); it != g_Holds.end(); )
    {
        if (it->second.remainingMs <= diff)
        {
            it = g_Holds.erase(it);
            continue;
        }

        it->second.remainingMs -= diff;

        Player* bot = ObjectAccessor::FindPlayer(ObjectGuid(it->first));
        Player* target = ObjectAccessor::FindPlayer(ObjectGuid(it->second.targetGuid));

        // Anything that makes holding wrong also ends the hold: combat always
        // wins, and there is nothing to stand still for if they have gone.
        if (!bot || !target || !bot->IsInWorld() || !target->IsInWorld() || !bot->IsAlive()
            || bot->IsInCombat() || bot->GetMapId() != target->GetMapId()
            || !bot->IsWithinDistInMap(target, g_SayDistance * 2.0f))
        {
            it = g_Holds.erase(it);
            continue;
        }

        // Re-asserted every tick because whatever the bot was doing will simply
        // move it again otherwise. Nothing is modified that needs putting back, so
        // if we stop refreshing, or the server dies, the bot just carries on.
        bot->StopMovingOnCurrentPos();
        bot->SetFacingToObject(target);

        ++it;
    }
}

uint32_t ActionsPerformed()
{
    return g_ActionsPerformed.load();
}

uint32_t ActionsFailed()
{
    return g_ActionsFailed.load();
}
