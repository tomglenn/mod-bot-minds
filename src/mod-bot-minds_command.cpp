#include "mod-bot-minds_command.h"
#include "mod-bot-minds_config.h"
#include "mod-bot-minds_governor.h"
#include "mod-bot-minds_llmclient.h"
#include "mod-bot-minds_memory.h"
#include "mod-bot-minds_persona.h"
#include "mod-bot-minds_relationship.h"
#include "mod-bot-minds-utilities.h"

#include "Chat.h"
#include "CharacterCache.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Field.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include "QueryResult.h"

#include <fmt/core.h>

using namespace Acore::ChatCommands;

namespace
{
    // Bots are looked up by name. Offline characters resolve through the cache so
    // stored memories can be inspected without logging the bot in.
    uint64_t ResolveGuid(ChatHandler* handler, const std::string& name, std::string& resolvedName)
    {
        if (Player* online = ObjectAccessor::FindPlayerByName(name))
        {
            resolvedName = online->GetName();
            return online->GetGUID().GetRawValue();
        }

        ObjectGuid guid = sCharacterCache->GetCharacterGuidByName(name);
        if (!guid.IsEmpty())
        {
            resolvedName = name;
            return guid.GetRawValue();
        }

        handler->SendSysMessage(fmt::format("BotMinds: no character named '{}'.", name));
        return 0;
    }

    std::string NameForGuid(uint64_t rawGuid)
    {
        ObjectGuid guid(rawGuid);
        std::string name;
        if (sCharacterCache->GetCharacterNameByGuid(guid, name))
            return name;
        return std::to_string(rawGuid);
    }
}

BotMindsConfigCommand::BotMindsConfigCommand() : CommandScript("BotMindsConfigCommand") { }

ChatCommandTable BotMindsConfigCommand::GetCommands() const
{
    static ChatCommandTable botMindsTable =
    {
        { "status",   HandleStatus,        SEC_ADMINISTRATOR, Console::Yes },
        { "reload",   HandleReload,        SEC_ADMINISTRATOR, Console::Yes },
        { "persona",  HandlePersona,       SEC_ADMINISTRATOR, Console::Yes },
        { "memory",   HandleMemory,        SEC_ADMINISTRATOR, Console::Yes },
        { "feelings", HandleRelationships, SEC_ADMINISTRATOR, Console::Yes },
        { "forget",   HandleForget,        SEC_ADMINISTRATOR, Console::Yes }
    };

    static ChatCommandTable commandTable =
    {
        { "botminds", botMindsTable }
    };

    return commandTable;
}

bool BotMindsConfigCommand::HandleStatus(ChatHandler* handler)
{
    handler->SendSysMessage(fmt::format("BotMinds: {}, provider {} model {}.",
                                        g_Enable ? "enabled" : "disabled", g_CloudProvider, g_CloudModel));

    if (!GetProvider())
        handler->SendSysMessage("BotMinds: no usable provider, bots are silent. Check BotMinds.ApiKey.");

    handler->SendSysMessage(fmt::format("BotMinds: reply limit {} chars, up to {} bots per line, "
                                        "{}s per-bot cooldown, {} calls per {}s.",
                                        g_MaxReplyChars, g_MaxBotsToPick, g_PerBotCooldownSec,
                                        g_HardCapCallsPerInterval, g_HardCapIntervalSec));

    handler->SendSysMessage(fmt::format("BotMinds: {} API calls since startup, {} in flight.",
                                        BotMindsGovernor::CallsSinceStartup(),
                                        BotMindsGovernor::CallsInFlight()));

    QueryResult counts = CharacterDatabase.Query(
        "SELECT (SELECT COUNT(*) FROM mod_bot_minds_memory), "
        "(SELECT COUNT(*) FROM mod_bot_minds_persona), "
        "(SELECT COUNT(*) FROM mod_bot_minds_relationship)");

    if (counts)
    {
        Field* fields = counts->Fetch();
        handler->SendSysMessage(fmt::format("BotMinds: {} memories, {} personas, {} relationships stored.",
                                            fields[0].Get<uint64_t>(), fields[1].Get<uint64_t>(),
                                            fields[2].Get<uint64_t>()));
    }

    return true;
}

bool BotMindsConfigCommand::HandleReload(ChatHandler* handler)
{
    sConfigMgr->Reload();
    LoadBotMindsConfig();
    InitLLMProviders();
    handler->SendSysMessage("BotMinds: configuration reloaded.");
    return true;
}

bool BotMindsConfigCommand::HandlePersona(ChatHandler* handler, std::string botName)
{
    std::string resolved;
    uint64_t guid = ResolveGuid(handler, botName, resolved);
    if (guid == 0)
        return true;

    QueryResult result = CharacterDatabase.Query(SafeFormat(
        "SELECT archetype, traits, speech_style, backstory FROM mod_bot_minds_persona WHERE bot_guid = {}", guid));

    if (!result)
    {
        handler->SendSysMessage(fmt::format("BotMinds: {} has no persona yet (one is made the first time they speak).",
                                            resolved));
        return true;
    }

    Field* fields = result->Fetch();
    handler->SendSysMessage(fmt::format("BotMinds: {} ({})", resolved, fields[0].Get<std::string>()));
    handler->SendSysMessage(fmt::format("  traits: {}", fields[1].Get<std::string>()));
    handler->SendSysMessage(fmt::format("  speech: {}", fields[2].Get<std::string>()));

    std::string backstory = fields[3].Get<std::string>();
    if (!backstory.empty())
        handler->SendSysMessage(fmt::format("  backstory: {}", backstory));

    return true;
}

bool BotMindsConfigCommand::HandleMemory(ChatHandler* handler, std::string botName)
{
    std::string resolved;
    uint64_t guid = ResolveGuid(handler, botName, resolved);
    if (guid == 0)
        return true;

    QueryResult result = CharacterDatabase.Query(SafeFormat(
        "SELECT subject_guid, kind, text, salience, created_at FROM mod_bot_minds_memory "
        "WHERE bot_guid = {} ORDER BY id DESC LIMIT 25", guid));

    if (!result)
    {
        handler->SendSysMessage(fmt::format("BotMinds: {} remembers nothing.", resolved));
        return true;
    }

    handler->SendSysMessage(fmt::format("BotMinds: {} remembers (newest first, from the database):", resolved));

    do
    {
        Field* fields = result->Fetch();
        std::string subject = fields[0].IsNull() ? "general" : NameForGuid(fields[0].Get<uint64_t>());
        handler->SendSysMessage(fmt::format("  [{}] {} ({:.2f}) {} - {}",
                                            subject, fields[1].Get<std::string>(), fields[3].Get<float>(),
                                            fields[4].Get<std::string>(), fields[2].Get<std::string>()));
    } while (result->NextRow());

    return true;
}

bool BotMindsConfigCommand::HandleRelationships(ChatHandler* handler, std::string botName)
{
    std::string resolved;
    uint64_t guid = ResolveGuid(handler, botName, resolved);
    if (guid == 0)
        return true;

    QueryResult result = CharacterDatabase.Query(SafeFormat(
        "SELECT other_guid, affinity, reason, interaction_count FROM mod_bot_minds_relationship "
        "WHERE bot_guid = {} ORDER BY ABS(affinity) DESC LIMIT 25", guid));

    if (!result)
    {
        handler->SendSysMessage(fmt::format("BotMinds: {} has no feelings on record.", resolved));
        return true;
    }

    handler->SendSysMessage(fmt::format("BotMinds: how {} feels about people:", resolved));

    do
    {
        Field* fields = result->Fetch();
        handler->SendSysMessage(fmt::format("  {}: {:+.2f} after {} exchanges - {}",
                                            NameForGuid(fields[0].Get<uint64_t>()), fields[1].Get<float>(),
                                            fields[3].Get<uint32_t>(), fields[2].Get<std::string>()));
    } while (result->NextRow());

    return true;
}

bool BotMindsConfigCommand::HandleForget(ChatHandler* handler, std::string botName)
{
    std::string resolved;
    uint64_t guid = ResolveGuid(handler, botName, resolved);
    if (guid == 0)
        return true;

    CharacterDatabase.Execute(SafeFormat("DELETE FROM mod_bot_minds_memory WHERE bot_guid = {}", guid));
    CharacterDatabase.Execute(SafeFormat("DELETE FROM mod_bot_minds_relationship WHERE bot_guid = {}", guid));

    handler->SendSysMessage(fmt::format(
        "BotMinds: wiped {}'s memories and relationships in the database. Restart the world server to clear the cache.",
        resolved));

    return true;
}
