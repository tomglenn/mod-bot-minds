#include "mod-bot-minds_command.h"
#include "mod-bot-minds_config.h"
#include "mod-bot-minds_events.h"
#include "mod-bot-minds_handler.h"
#include "mod-bot-minds_random.h"
#include "Log.h"

void Addmod_bot_mindsScripts()
{
    LOG_INFO("server.loading", "[BotMinds] Registering scripts.");

    new BotMindsConfigWorldScript();
    new PlayerBotChatHandler();
    new BotMindsAmbientChatter();
    new BotMindsConfigCommand();

    new ChatOnKill();
    new ChatOnLoot();
    new ChatOnDeath();
    new ChatOnQuest();
    new ChatOnLearn();
    new ChatOnDuel();
    new ChatOnLevelUp();
    new ChatOnGuildChange();
}
