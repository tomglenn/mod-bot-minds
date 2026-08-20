# mod-bot-minds

Playerbots that talk like people. Each bot has a persona, remembers what happened to it, keeps track of how it feels about you, and follows the thread of a conversation instead of shouting over it.

> [!IMPORTANT]
> **This only works on AzerothCore with Playerbots.** It needs [liyunfan1223/azerothcore-wotlk](https://github.com/liyunfan1223/azerothcore-wotlk) with [mod-playerbots](https://github.com/liyunfan1223/mod-playerbots) enabled, and it calls into the Playerbots API directly. It will not build against upstream AzerothCore, because without bots there is nobody for it to give a mind to.

> [!NOTE]
> **Heavily inspired by [mod-ollama-chat](https://github.com/DustinHendrickson/mod-ollama-chat) by Dustin Hendrickson**, which is where this started and which deserves the credit for the idea of giving playerbots an LLM voice. The chat pipeline here has since been rewritten around personas, persistent memory and a single shared prompt path; the HTTP client, the config plumbing and the event hook surface are the parts that still trace back to that project. If you want bots chatting through a local model, go and use mod-ollama-chat: it does that, and this does not.

> [!CAUTION]
> **This costs money.** Every line a bot speaks is an API call to a hosted model. There is no local option. See [Cost](#cost) for the arithmetic and the settings that cap it.

## What it does

**One voice.** Every line a bot speaks, whether it is answering you, muttering about its bags, or reacting to a level-up, is built by the same prompt builder and sent to the same model. There is one place that decides how bots sound, so a bot cannot drift between a casual player voice and a heroic one.

**A conversational floor.** Name a bot and it answers. Address the room ("hey folks", "does anyone need this?") and a couple of bots may pick it up. Say anything else and the reply belongs to the bot you were already talking to, so "yeah good thanks, you?" goes back to whoever asked rather than to three bystanders. In a small group somebody always answers you. Bots that were not addressed are offered the turn and can decline it.

**Memory that survives restarts.** What a bot learns is written to the database as it happens, not batched, so killing the server does not erase the last ten minutes. Recent and high-salience memories about whoever it is talking to go into every prompt. Read it back with `.botminds memory <bot>`.

**Personas.** One row per bot: traits and a manner of speaking, generated deterministically from class, race and GUID so a bot is the same character every session. Write a `backstory` into the table by hand and that gets used too.

**Relationships.** Each exchange can nudge how a bot feels about you, which is stored and fed back into later prompts. `.botminds feelings <bot>` shows the ledger.

**Ambient chatter and event reactions.** Bots comment on their surroundings and on things that happen (kills, loot, deaths, quests, duels, level-ups, guild changes), only within earshot of a real player, and always through the same pipeline, so those lines carry the persona and land in memory as well.

## Requirements

- AzerothCore (liyunfan1223 fork) with mod-playerbots.
- An Anthropic or OpenAI API key. Both are driven with tool calling, which the memory system depends on.
- fmt (comes with AzerothCore), OpenSSL for HTTPS. nlohmann/json and cpp-httplib are bundled in this repo, so there is nothing to install for those.

## Installing

```bash
cd /path/to/azerothcore/modules
git clone https://github.com/tomglenn/mod-bot-minds.git
```

Rebuild the worldserver:

```bash
cd /path/to/azerothcore/build
cmake ..
make -j$(nproc)
```

Copy the config template and edit it:

```bash
cp /path/to/azerothcore/modules/mod-bot-minds/conf/mod_bot_minds.conf.dist \
   /path/to/azerothcore/env/dist/etc/modules/mod_bot_minds.conf
```

Set `BotMinds.ApiKey` in that copy. Everything else has a working default. The module loads without a key and simply keeps every bot silent, so an empty key is a safe way to switch it off.

Start the server. The module's SQL creates its tables on import.

> [!TIP]
> Keep your real `mod_bot_minds.conf` out of version control. This repo's `.gitignore` already excludes `conf/*.conf` while keeping the `.dist` template, so the file you edit will not be committed by accident.

Finally, turn off playerbots' own canned chatter so it does not talk over the module. In `playerbots.conf`:

```
AiPlayerbot.EnableBroadcasts = 0
AiPlayerbot.RandomBotTalk = 0
AiPlayerbot.RandomBotEmote = 0
AiPlayerbot.RandomBotSuggestDungeons = 0
AiPlayerbot.EnableGreet = 0
AiPlayerbot.GuildFeedback = 0
AiPlayerbot.RandomBotSayWithoutMaster = 0
```

## Configuration

`conf/mod_bot_minds.conf.dist` is the annotated template with every setting and its default. The ones worth knowing:

| Setting | Default | What it does |
| --- | --- | --- |
| `BotMinds.ApiKey` | empty | Your API key. Empty means silence. |
| `BotMinds.Provider` | `anthropic` | `anthropic` or `openai`. |
| `BotMinds.Model` | `claude-haiku-4-5` | Any tool-calling model from that provider. |
| `BotMinds.MaxReplyChars` | 200 | Hard limit on a spoken line. The model is told this number. |
| `BotMinds.Attention.FloorWindowSec` | 60 | How long the bot you are talking to keeps the right to answer. |
| `BotMinds.Attention.MaxBotsToPick` | 2 | Most bots that may answer one line. |
| `BotMinds.Route.HandleChannel` | 0 | General and Trade. Off because a busy channel burns calls. |
| `BotMinds.Ambient.Chance` | 25 | How talkative bots are when nothing is happening. |
| `BotMinds.Limits.HardCapCallsPerInterval` | 60 | Hard ceiling on calls per interval. |

## Commands

All require SEC_ADMINISTRATOR and work from the server console too.

| Command | What it shows |
| --- | --- |
| `.botminds status` | Provider, model, limits, calls made since startup, and how much is stored |
| `.botminds reload` | Re-read the config and rebuild the provider |
| `.botminds persona <bot>` | That bot's traits, speech style and backstory |
| `.botminds memory <bot>` | The 25 newest memories, read from the database |
| `.botminds feelings <bot>` | Affinity towards everyone it has an opinion about |
| `.botminds forget <bot>` | Delete that bot's memories and relationships |

## Cost

A turn is roughly 1000 to 1400 input tokens (persona, world state, relationship, memories, recent chat, the tool schema) and 80 to 150 output. On Claude Haiku 4.5 that is about $0.002 per line a bot speaks. Ordinary play is a few calls a minute, so single-digit cents an hour.

Calls only happen where a real player can hear the result: say and yell are limited to bots within `SayDistance`, ambient chatter needs someone within `Ambient.PlayerDistance`, and bots only answer each other where a person is watching. Party and guild chat have no distance limit, because those channels do not either.

`Limits.HardCapCallsPerInterval` is a hard ceiling, so the worst case is bounded no matter what happens in game. `.botminds status` reports the running total, which beats guessing.

## How a line gets spoken

1. **Chat hook** turns an incoming message into a scope (say, party, guild, channel, whisper) and records it in that scope's transcript. Playerbot commands are filtered out.
2. **Attention** decides who answers: a named bot, the bot holding the floor, one member of a small group, or a chance roll per listener. This is the only place reply probability lives.
3. **Governor** decides who may: provider available, hard cap and concurrency free, per-bot cooldown, proximity. A direct answer to someone who addressed the bot skips the cooldown and proximity.
4. **Prompt** is assembled from persona, world state, relationship, memory and the recent transcript, plus one instruction for why this bot is speaking and the shared voice rules.
5. **Provider** returns a `bot_turn` tool call: whether to speak, the words, new memories, relationship change.
6. **Speak** cleans the line up (no emotes, no quoting, trimmed to the character limit on a word boundary), puts it in the right channel, records it, and writes the memories.

## Database

Three tables in the characters database, created by the module's SQL:

- `mod_bot_minds_persona` — one row per bot.
- `mod_bot_minds_memory` — what bots remember, with a subject and a salience.
- `mod_bot_minds_relationship` — affinity per bot per person.

## Debugging

`BotMinds.DebugEnabled = 1` logs who was picked to answer, who stayed quiet, and why. `BotMinds.DebugShowFullPrompt = 1` adds the full prompt for every line, which is very noisy but useful when the voice is wrong.

## License

GNU AGPL v3, consistent with AzerothCore.
