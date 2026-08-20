#ifndef MOD_BOT_MINDS_HANDLER_H
#define MOD_BOT_MINDS_HANDLER_H

#include "ScriptMgr.h"

#include <string>

class Channel;
class Group;
class Guild;
class Player;

// --------------------------------------------
// Chat entry point. Turns AzerothCore's chat hooks into a scope plus a line of
// text and hands them to the attention layer, which decides who answers.
// --------------------------------------------
class PlayerBotChatHandler : public PlayerScript
{
public:
    PlayerBotChatHandler() : PlayerScript("PlayerBotChatHandler", {
        PLAYERHOOK_CAN_PLAYER_USE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GROUP_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_GUILD_CHAT,
        PLAYERHOOK_CAN_PLAYER_USE_CHANNEL_CHAT,
    }) {}

    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg) override;
    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Group* group) override;
    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Guild* guild) override;
    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel) override;
    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Player* receiver) override;
};

#endif // MOD_BOT_MINDS_HANDLER_H
