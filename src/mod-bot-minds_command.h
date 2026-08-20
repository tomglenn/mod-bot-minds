#ifndef MOD_BOT_MINDS_COMMAND_H
#define MOD_BOT_MINDS_COMMAND_H

#include "ScriptMgr.h"
#include "Chat.h"

// --------------------------------------------
// `.botminds` commands for looking inside a bot's head: its persona, what it
// remembers, and how it feels about people. The memory command reads the
// database rather than the cache, so it also answers "did that actually persist".
// --------------------------------------------
class BotMindsConfigCommand : public CommandScript
{
public:
    BotMindsConfigCommand();
    Acore::ChatCommands::ChatCommandTable GetCommands() const override;

    static bool HandleStatus(ChatHandler* handler);
    static bool HandleReload(ChatHandler* handler);
    static bool HandlePersona(ChatHandler* handler, std::string botName);
    static bool HandleMemory(ChatHandler* handler, std::string botName);
    static bool HandleRelationships(ChatHandler* handler, std::string botName);
    static bool HandleForget(ChatHandler* handler, std::string botName);
};

#endif // MOD_BOT_MINDS_COMMAND_H
