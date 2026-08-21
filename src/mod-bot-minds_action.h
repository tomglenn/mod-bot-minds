#ifndef MOD_BOT_MINDS_ACTION_H
#define MOD_BOT_MINDS_ACTION_H

#include <cstdint>
#include <string>
#include <vector>

class Player;

// --------------------------------------------
// Actions: the point where a bot stops talking and does something.
//
// Two halves. BuildActionMenu works out what this bot can genuinely do for this
// person right now, and that menu goes into the prompt so the bot can only offer
// real things. Whatever the model picks is then validated back against the same
// menu, so it cannot invent a spell, a target or an amount.
//
// Execution never happens on the thread that talked to the API. Actions are
// queued and run from the world tick, which is the only place it is safe to cast
// spells, open trade windows or send mail.
// --------------------------------------------

enum class ActionKind : uint8_t
{
    None = 0,
    Buff,
    Heal,
    GiveGold,
    Follow,
    Stay
};

// What a bot may do for one person on one turn.
struct ActionMenu
{
    std::vector<std::string> buffs;          // castable and the target does not have it
    std::vector<std::string> refreshable;    // castable, but already up; only on request
    std::vector<std::string> heals;
    std::vector<std::string> alreadyHave;    // human-readable "X, 12 minutes left"
    uint32_t                 maxCopper = 0;  // 0 means no money is on offer
    bool                     giftByMail = false;
    bool                     canTakeOrders = false;
    std::string              goldRefusal;    // why not, in plain words, for the prompt

    // Anything a bot would walk up and offer. Refreshable buffs deliberately do
    // not count: nobody wants a stranger recasting a buff they already have.
    bool NothingToVolunteer() const
    {
        return buffs.empty() && heals.empty();
    }

    bool Empty() const
    {
        return buffs.empty() && refreshable.empty() && heals.empty()
            && maxCopper == 0 && !canTakeOrders;
    }
};

// A decided action, carrying only values so it can cross a thread boundary.
struct BotAction
{
    ActionKind  kind = ActionKind::None;
    uint64_t    botGuid = 0;
    uint64_t    targetGuid = 0;
    std::string spellName;         // Buff / Heal
    std::string command;           // Follow / Stay
    uint32_t    copper = 0;        // GiveGold
    bool        viaMail = false;   // GiveGold delivery
    bool        promised = false;  // the bot said it would; worth apologising if it cannot
    bool        tradeStarted = false;  // gold: the window is open, the coin still needs putting in
    bool        goldPlaced = false;    // gold: the coin is in, the bot still needs to accept
    bool        mentionedPost = false; // the spoken line already told them to check their mail
    uint8_t     attempt = 0;
    uint32_t    readyInMs = 0;     // retry backoff
};

// Map the tool's kind string onto the enum. Unknown names become None.
ActionKind ActionKindFromName(const std::string& name);

// What this bot can do for `other` right now. Empty when there is nothing to offer.
// `unprompted` means the bot is considering volunteering rather than answering,
// which is the only case where it should hold back a heal from someone at full
// health: if you ask for one, you get one.
ActionMenu BuildActionMenu(Player* bot, Player* other, bool unprompted = false);

// The menu rendered for the prompt. Empty string when the menu is empty.
std::string DescribeActionMenu(const ActionMenu& menu, const std::string& otherName);

// Check a model-chosen action against the menu it was offered, clamping the
// amount and rejecting anything that was not on it. Returns false to drop it.
bool ValidateAction(const ActionMenu& menu, BotAction& action);

// Safe from any thread. Runs on the next world tick.
void SubmitBotAction(const BotAction& action);

// Drained from BotMindsConfigWorldScript::OnUpdate, on the world thread.
void RunPendingActions(uint32_t diff);

// Note that a bot just spoke to somebody, so it should stand still and face them
// for a moment rather than wandering off mid-sentence. Safe from any thread.
void HoldStillForConversation(uint64_t botGuid, uint64_t targetGuid);

// Keeps held bots planted. Also from the world tick.
void RunConversationHolds(uint32_t diff);

// Counters for `.botminds status`.
uint32_t ActionsPerformed();
uint32_t ActionsFailed();

#endif // MOD_BOT_MINDS_ACTION_H
