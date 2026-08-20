#include "mod-bot-minds_config.h"
#include "mod-bot-minds_governor.h"
#include "mod-bot-minds_llmclient.h"
#include "mod-bot-minds_memory.h"
#include "mod-bot-minds_persona.h"
#include "mod-bot-minds_relationship.h"
#include "mod-bot-minds_transcript.h"
#include "mod-bot-minds-utilities.h"

#include "Config.h"
#include "Log.h"

#include <ctime>

// --------------------------------------------
// Core
// --------------------------------------------
bool g_Enable              = true;
bool g_DebugEnabled        = false;
bool g_DebugShowFullPrompt = false;

// --------------------------------------------
// Provider
// --------------------------------------------
std::string g_CloudProvider  = "anthropic";
std::string g_CloudApiKey     = "";
std::string g_CloudModel      = "claude-haiku-4-5";
uint32_t    g_CloudMaxTokens  = 512;
uint32_t    g_CloudTimeoutSec = 30;
uint32_t    g_MaxReplyChars   = 200;

// --------------------------------------------
// Routing
// --------------------------------------------
uint32_t g_HandleWhispers = 1;
uint32_t g_HandleSay      = 1;
uint32_t g_HandleParty    = 1;
uint32_t g_HandleGuild    = 1;
uint32_t g_HandleChannel  = 0;

// --------------------------------------------
// Attention
// --------------------------------------------
uint32_t g_ReplyChanceWhisper  = 100;
uint32_t g_ReplyChanceSay      = 70;
uint32_t g_ReplyChanceParty    = 85;
uint32_t g_ReplyChanceGuild    = 50;
uint32_t g_ReplyChanceChannel  = 35;
uint32_t g_ReplyChanceBotToBot = 12;
uint32_t g_InterjectChance     = 12;
uint32_t g_FloorWindowSec      = 60;
uint32_t g_SmallGroupSize      = 3;
uint32_t g_MaxBotsToPick       = 2;
uint32_t g_MaxBotChainDepth    = 2;
float    g_SayDistance         = 30.0f;

// --------------------------------------------
// Transcript
// --------------------------------------------
uint32_t g_TranscriptLines    = 8;
uint32_t g_TranscriptMaxChars = 700;
uint32_t g_TranscriptTtlSec   = 900;

// --------------------------------------------
// Memory
// --------------------------------------------
uint32_t g_RecentMemoryCount     = 6;
uint32_t g_RelevantMemoryCount   = 4;
uint32_t g_MaxMemoriesPerSubject = 30;
uint32_t g_MaxMemoryPromptChars  = 1200;

// --------------------------------------------
// Rate limits
// --------------------------------------------
float    g_ProximityRadius         = 40.0f;
uint32_t g_PerBotCooldownSec       = 12;
uint32_t g_MaxConcurrentCalls      = 3;
uint32_t g_MaxCallsPerMinute       = 60;
bool     g_DisableRepliesInCombat  = true;

// --------------------------------------------
// Ambient chatter
// --------------------------------------------
bool     g_EnableAmbientChatter = true;
uint32_t g_AmbientChance         = 25;
uint32_t g_AmbientMinIntervalSec = 120;
uint32_t g_AmbientMaxIntervalSec = 600;
float    g_AmbientPlayerDistance = 60.0f;

// --------------------------------------------
// Event chatter
// --------------------------------------------
bool     g_EnableEventChatter    = true;
bool     g_EnableGuildChatter    = true;
float    g_EventDistance         = 40.0f;
uint32_t g_EventMaxBots          = 1;
uint32_t g_EventChanceKill       = 1;
uint32_t g_EventChancePvPKill    = 15;
uint32_t g_EventChanceLoot       = 15;
uint32_t g_EventChanceDeath      = 25;
uint32_t g_EventChanceQuest      = 20;
uint32_t g_EventChanceSpell      = 2;
uint32_t g_EventChanceDuel       = 25;
uint32_t g_EventChanceLevelUp    = 60;
uint32_t g_EventChanceGuildEpic  = 80;
uint32_t g_EventChanceGuildRare  = 30;
uint32_t g_EventChanceGuildLevelUp = 50;
uint32_t g_EventChanceGuildMember  = 60;

// --------------------------------------------
// Presentation
// --------------------------------------------
bool     g_EnableTypingSimulation       = false;
uint32_t g_TypingSimulationBaseDelay    = 1000;
uint32_t g_TypingSimulationDelayPerChar = 25;

std::vector<std::string> g_BlacklistCommands;

uint32_t g_SaveIntervalMinutes = 10;

static time_t g_LastSaveTime = 0;

void LoadBotMindsConfig()
{
    g_Enable              = sConfigMgr->GetOption<bool>("BotMinds.Enable", true);
    g_DebugEnabled        = sConfigMgr->GetOption<bool>("BotMinds.DebugEnabled", false);
    g_DebugShowFullPrompt = sConfigMgr->GetOption<bool>("BotMinds.DebugShowFullPrompt", false);

    g_CloudProvider  = sConfigMgr->GetOption<std::string>("BotMinds.Provider", "anthropic");
    g_CloudApiKey    = sConfigMgr->GetOption<std::string>("BotMinds.ApiKey", "");
    g_CloudModel     = sConfigMgr->GetOption<std::string>("BotMinds.Model", "claude-haiku-4-5");
    g_CloudMaxTokens = sConfigMgr->GetOption<uint32_t>("BotMinds.MaxTokens", 512);
    g_CloudTimeoutSec = sConfigMgr->GetOption<uint32_t>("BotMinds.TimeoutSec", 30);
    g_MaxReplyChars  = sConfigMgr->GetOption<uint32_t>("BotMinds.MaxReplyChars", 200);

    g_HandleWhispers = sConfigMgr->GetOption<uint32_t>("BotMinds.Route.HandleWhispers", 1);
    g_HandleSay      = sConfigMgr->GetOption<uint32_t>("BotMinds.Route.HandleSay", 1);
    g_HandleParty    = sConfigMgr->GetOption<uint32_t>("BotMinds.Route.HandleParty", 1);
    g_HandleGuild    = sConfigMgr->GetOption<uint32_t>("BotMinds.Route.HandleGuild", 1);
    g_HandleChannel  = sConfigMgr->GetOption<uint32_t>("BotMinds.Route.HandleChannel", 0);

    g_ReplyChanceWhisper  = sConfigMgr->GetOption<uint32_t>("BotMinds.ReplyChance.Whisper", 100);
    g_ReplyChanceSay      = sConfigMgr->GetOption<uint32_t>("BotMinds.ReplyChance.Say", 70);
    g_ReplyChanceParty    = sConfigMgr->GetOption<uint32_t>("BotMinds.ReplyChance.Party", 85);
    g_ReplyChanceGuild    = sConfigMgr->GetOption<uint32_t>("BotMinds.ReplyChance.Guild", 50);
    g_ReplyChanceChannel  = sConfigMgr->GetOption<uint32_t>("BotMinds.ReplyChance.Channel", 35);
    g_ReplyChanceBotToBot = sConfigMgr->GetOption<uint32_t>("BotMinds.ReplyChance.BotToBot", 12);
    g_InterjectChance     = sConfigMgr->GetOption<uint32_t>("BotMinds.Attention.InterjectChance", 12);
    g_FloorWindowSec      = sConfigMgr->GetOption<uint32_t>("BotMinds.Attention.FloorWindowSec", 60);
    g_SmallGroupSize      = sConfigMgr->GetOption<uint32_t>("BotMinds.Attention.SmallGroupSize", 3);
    g_MaxBotsToPick       = sConfigMgr->GetOption<uint32_t>("BotMinds.Attention.MaxBotsToPick", 2);
    g_MaxBotChainDepth    = sConfigMgr->GetOption<uint32_t>("BotMinds.Attention.MaxBotChainDepth", 2);
    g_SayDistance         = sConfigMgr->GetOption<float>("BotMinds.SayDistance", 30.0f);

    g_TranscriptLines    = sConfigMgr->GetOption<uint32_t>("BotMinds.Transcript.Lines", 8);
    g_TranscriptMaxChars = sConfigMgr->GetOption<uint32_t>("BotMinds.Transcript.MaxChars", 700);
    g_TranscriptTtlSec   = sConfigMgr->GetOption<uint32_t>("BotMinds.Transcript.TtlSec", 900);

    g_RecentMemoryCount     = sConfigMgr->GetOption<uint32_t>("BotMinds.Memory.RecentCount", 6);
    g_RelevantMemoryCount   = sConfigMgr->GetOption<uint32_t>("BotMinds.Memory.RelevantCount", 4);
    g_MaxMemoriesPerSubject = sConfigMgr->GetOption<uint32_t>("BotMinds.Memory.MaxPerSubject", 30);
    g_MaxMemoryPromptChars  = sConfigMgr->GetOption<uint32_t>("BotMinds.Memory.MaxPromptChars", 1200);

    g_ProximityRadius         = sConfigMgr->GetOption<float>("BotMinds.Limits.ProximityRadius", 40.0f);
    g_PerBotCooldownSec       = sConfigMgr->GetOption<uint32_t>("BotMinds.Limits.PerBotCooldownSec", 12);
    g_MaxConcurrentCalls      = sConfigMgr->GetOption<uint32_t>("BotMinds.Limits.MaxConcurrentCalls", 3);
    g_MaxCallsPerMinute       = sConfigMgr->GetOption<uint32_t>("BotMinds.Limits.MaxCallsPerMinute", 60);
    g_DisableRepliesInCombat  = sConfigMgr->GetOption<bool>("BotMinds.Limits.DisableRepliesInCombat", true);

    g_EnableAmbientChatter  = sConfigMgr->GetOption<bool>("BotMinds.Ambient.Enable", true);
    g_AmbientChance         = sConfigMgr->GetOption<uint32_t>("BotMinds.Ambient.Chance", 25);
    g_AmbientMinIntervalSec = sConfigMgr->GetOption<uint32_t>("BotMinds.Ambient.MinIntervalSec", 120);
    g_AmbientMaxIntervalSec = sConfigMgr->GetOption<uint32_t>("BotMinds.Ambient.MaxIntervalSec", 600);
    g_AmbientPlayerDistance = sConfigMgr->GetOption<float>("BotMinds.Ambient.PlayerDistance", 60.0f);

    g_EnableEventChatter       = sConfigMgr->GetOption<bool>("BotMinds.Events.Enable", true);
    g_EnableGuildChatter       = sConfigMgr->GetOption<bool>("BotMinds.Events.EnableGuild", true);
    g_EventDistance            = sConfigMgr->GetOption<float>("BotMinds.Events.Distance", 40.0f);
    g_EventMaxBots             = sConfigMgr->GetOption<uint32_t>("BotMinds.Events.MaxBots", 1);
    g_EventChanceKill          = sConfigMgr->GetOption<uint32_t>("BotMinds.Events.Chance.Kill", 1);
    g_EventChancePvPKill       = sConfigMgr->GetOption<uint32_t>("BotMinds.Events.Chance.PvPKill", 15);
    g_EventChanceLoot          = sConfigMgr->GetOption<uint32_t>("BotMinds.Events.Chance.Loot", 15);
    g_EventChanceDeath         = sConfigMgr->GetOption<uint32_t>("BotMinds.Events.Chance.Death", 25);
    g_EventChanceQuest         = sConfigMgr->GetOption<uint32_t>("BotMinds.Events.Chance.Quest", 20);
    g_EventChanceSpell         = sConfigMgr->GetOption<uint32_t>("BotMinds.Events.Chance.Spell", 2);
    g_EventChanceDuel          = sConfigMgr->GetOption<uint32_t>("BotMinds.Events.Chance.Duel", 25);
    g_EventChanceLevelUp       = sConfigMgr->GetOption<uint32_t>("BotMinds.Events.Chance.LevelUp", 60);
    g_EventChanceGuildEpic     = sConfigMgr->GetOption<uint32_t>("BotMinds.Events.Chance.GuildEpicGear", 80);
    g_EventChanceGuildRare     = sConfigMgr->GetOption<uint32_t>("BotMinds.Events.Chance.GuildRareGear", 30);
    g_EventChanceGuildLevelUp  = sConfigMgr->GetOption<uint32_t>("BotMinds.Events.Chance.GuildLevelUp", 50);
    g_EventChanceGuildMember   = sConfigMgr->GetOption<uint32_t>("BotMinds.Events.Chance.GuildMember", 60);

    g_EnableTypingSimulation       = sConfigMgr->GetOption<bool>("BotMinds.Typing.Enable", false);
    g_TypingSimulationBaseDelay    = sConfigMgr->GetOption<uint32_t>("BotMinds.Typing.BaseDelayMs", 1000);
    g_TypingSimulationDelayPerChar = sConfigMgr->GetOption<uint32_t>("BotMinds.Typing.DelayPerCharMs", 25);

    g_SaveIntervalMinutes = sConfigMgr->GetOption<uint32_t>("BotMinds.SaveIntervalMinutes", 10);

    g_BlacklistCommands = SplitString(sConfigMgr->GetOption<std::string>("BotMinds.BlacklistCommands", ""), ',');

    if (g_MaxBotsToPick == 0)
        g_MaxBotsToPick = 1;
    if (g_MaxConcurrentCalls == 0)
        g_MaxConcurrentCalls = 1;
    if (g_AmbientMaxIntervalSec < g_AmbientMinIntervalSec)
        g_AmbientMaxIntervalSec = g_AmbientMinIntervalSec;
}

BotMindsConfigWorldScript::BotMindsConfigWorldScript() : WorldScript("BotMindsConfigWorldScript") { }

void BotMindsConfigWorldScript::OnStartup()
{
    LoadBotMindsConfig();

    InitLLMProviders();
    LoadPersonasFromDB();
    LoadMemoriesFromDB();
    LoadRelationshipsFromDB();

    g_LastSaveTime = time(nullptr);
}

void BotMindsConfigWorldScript::OnShutdown()
{
    FlushPersonasToDB();
    FlushRelationshipsToDB();
}

void BotMindsConfigWorldScript::OnUpdate(uint32 diff)
{
    BotMindsGovernor::Tick(diff);

    time_t now = time(nullptr);
    if (g_SaveIntervalMinutes > 0 && now - g_LastSaveTime >= static_cast<time_t>(g_SaveIntervalMinutes * 60))
    {
        FlushPersonasToDB();
        FlushRelationshipsToDB();
        PruneTranscripts();
        g_LastSaveTime = now;
    }
}
