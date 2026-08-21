#ifndef MOD_BOT_MINDS_CONFIG_H
#define MOD_BOT_MINDS_CONFIG_H

#include <cstdint>
#include <string>
#include <vector>

#include "ScriptMgr.h"

// --------------------------------------------
// Core
// --------------------------------------------
extern bool g_Enable;
extern bool g_DebugEnabled;
extern bool g_DebugShowFullPrompt;

// --------------------------------------------
// Provider
// --------------------------------------------
extern std::string g_CloudProvider;   // "anthropic" | "openai"
extern std::string g_CloudApiKey;     // empty keeps every bot silent
extern std::string g_CloudModel;
extern uint32_t    g_CloudMaxTokens;
extern uint32_t    g_CloudTimeoutSec;
extern uint32_t    g_MaxReplyChars;

// --------------------------------------------
// Routing: which channels bots listen to
// --------------------------------------------
extern uint32_t g_HandleWhispers;
extern uint32_t g_HandleSay;
extern uint32_t g_HandleParty;
extern uint32_t g_HandleGuild;
extern uint32_t g_HandleChannel;

// --------------------------------------------
// Attention: who answers, and how often
// --------------------------------------------
extern uint32_t g_ReplyChanceWhisper;
extern uint32_t g_ReplyChanceSay;
extern uint32_t g_ReplyChanceParty;
extern uint32_t g_ReplyChanceGuild;
extern uint32_t g_ReplyChanceChannel;
extern uint32_t g_ReplyChanceBotToBot;
extern uint32_t g_InterjectChance;      // chance a bystander chips in when someone else holds the floor
extern uint32_t g_FloorWindowSec;       // how long the last speaker keeps the reply
extern uint32_t g_SmallGroupSize;       // at or below this many listeners, one always answers
extern uint32_t g_MaxBotsToPick;
extern uint32_t g_MaxBotChainDepth;     // bot-to-bot hops allowed after a human's line
extern float    g_SayDistance;

// --------------------------------------------
// Transcript
// --------------------------------------------
extern uint32_t g_TranscriptLines;
extern uint32_t g_TranscriptMaxChars;
extern uint32_t g_TranscriptTtlSec;

// --------------------------------------------
// Memory
// --------------------------------------------
extern uint32_t g_RecentMemoryCount;
extern uint32_t g_RelevantMemoryCount;
extern uint32_t g_MaxMemoriesPerSubject;
extern uint32_t g_MaxMemoryPromptChars;

// --------------------------------------------
// Rate limits
// --------------------------------------------
extern float    g_ProximityRadius;
extern uint32_t g_PerBotCooldownSec;
extern uint32_t g_MaxConcurrentCalls;
extern uint32_t g_MaxCallsPerMinute;
extern bool     g_DisableRepliesInCombat;

// --------------------------------------------
// Ambient chatter
// --------------------------------------------
extern bool     g_EnableAmbientChatter;
extern uint32_t g_AmbientChance;
extern uint32_t g_AmbientMinIntervalSec;
extern uint32_t g_AmbientMaxIntervalSec;

// --------------------------------------------
// Event chatter
// --------------------------------------------
extern bool     g_EnableEventChatter;
extern bool     g_EnableGuildChatter;
extern float    g_EventDistance;
extern uint32_t g_EventMaxBots;
extern uint32_t g_EventChanceKill;
extern uint32_t g_EventChancePvPKill;
extern uint32_t g_EventChanceLoot;
extern uint32_t g_EventChanceDeath;
extern uint32_t g_EventChanceQuest;
extern uint32_t g_EventChanceSpell;
extern uint32_t g_EventChanceDuel;
extern uint32_t g_EventChanceLevelUp;
extern uint32_t g_EventChanceGuildEpic;
extern uint32_t g_EventChanceGuildRare;
extern uint32_t g_EventChanceGuildLevelUp;
extern uint32_t g_EventChanceGuildMember;

// --------------------------------------------
// Actions: what bots will actually do, not just say
// --------------------------------------------
extern bool     g_ActionsEnable;
extern uint32_t g_ActionMaxAttempts;
extern float    g_GiftMinAffinity;      // how much a bot must like you before it parts with coin
extern uint32_t g_GiftMaxCopper;        // absolute ceiling on one gift; 0 disables gold entirely
extern uint32_t g_GiftCopperPerLevel;   // scales the ceiling with the giver's level
extern uint32_t g_GiftCooldownSec;      // per bot, per person
extern uint32_t g_UnpromptedChance;     // chance an ambient turn is a favour rather than a remark
extern uint32_t g_UnpromptedCooldownSec;

// --------------------------------------------
// Presentation
// --------------------------------------------
extern bool     g_EnableTypingSimulation;
extern uint32_t g_TypingSimulationBaseDelay;
extern uint32_t g_TypingSimulationDelayPerChar;

// --------------------------------------------
// Playerbot commands typed in chat, which are instructions rather than talk.
// --------------------------------------------
extern std::vector<std::string> g_BlacklistCommands;

// --------------------------------------------
// How often persona and relationship changes are written back (minutes).
// Memories are written as they happen and are not affected by this.
// --------------------------------------------
extern uint32_t g_SaveIntervalMinutes;

void LoadBotMindsConfig();

class BotMindsConfigWorldScript : public WorldScript
{
public:
    BotMindsConfigWorldScript();
    void OnStartup() override;
    void OnShutdown() override;
    void OnUpdate(uint32 diff) override;
};

#endif // MOD_BOT_MINDS_CONFIG_H
