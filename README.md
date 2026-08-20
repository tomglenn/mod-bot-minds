# mod-bot-minds

Playerbots that talk like people. Each bot has a persona, remembers what happened to it, keeps track of how it feels about you, and follows the thread of a conversation instead of shouting over it.

> [!IMPORTANT]
> **This only works on AzerothCore with Playerbots.** It needs [liyunfan1223/azerothcore-wotlk](https://github.com/liyunfan1223/azerothcore-wotlk) with [mod-playerbots](https://github.com/liyunfan1223/mod-playerbots) enabled, and it calls into the Playerbots API directly. It will not build against upstream AzerothCore, because without bots there is nobody for it to give a mind to.

> [!NOTE]
> **Heavily inspired by [mod-ollama-chat](https://github.com/DustinHendrickson/mod-ollama-chat) by Dustin Hendrickson**, which is where this started and which deserves the credit for the idea of giving playerbots an LLM voice. The chat pipeline here has since been rewritten around personas, persistent memory and a single shared prompt path; the HTTP client, the config plumbing and the event hook surface are the parts that still trace back to that project. If you want bots chatting through a local model, go and use mod-ollama-chat: it does that, and this does not.

> [!CAUTION]
> **This costs money.** Every line a bot speaks is an API call to a hosted model. There is no local option. See [Cost](#cost) for the arithmetic and the settings that cap it.

## What it does

**Talk to a bot and the right one answers.** Say a bot's name and that bot replies. Ask the room something and a couple of them chime in. Follow up without naming anyone and the answer comes from the bot you were already talking to, not from three strangers who happened to overhear. In a small party, someone always answers you.

**They sound like players, not heroes.** Bots chat the way people in a game chat: short, casual, off the cuff. No monologues about destiny, no narrating their own actions, no sliding into stagey roleplay halfway through a conversation.

**They remember.** Tell a bot you're working on Brotherhood of Thieves, ask about it an hour later or after a server restart, and it knows. Memories are saved the moment they happen, so nothing is lost when the server goes down. `.botminds memory <bot>` shows you what a bot is carrying.

**Every bot is a different person.** Each one has its own temperament and way of speaking, and keeps it for good. The blunt one stays blunt, the cheerful one stays cheerful. `.botminds persona <bot>`.

**They form opinions of you.** Help a bot out and it warms to you. Be rude and it cools off, and it remembers why. `.botminds feelings <bot>` shows where you stand.

**They talk when you don't.** Bots grumble about full bags, mention what's around them, and react when someone dies, levels up or wins a duel. Only ever within earshot, so the world feels lived-in without running up a bill for conversations nobody hears.

## Requirements

- AzerothCore (liyunfan1223 fork) with mod-playerbots.
- An **Anthropic** API key. There is also an OpenAI provider, but it is untested (see [Providers](#providers)).
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

## Providers

**Anthropic is the tested path.** Everything in this repo was built and run against Claude Haiku 4.5, and that is what the defaults point at.

**The OpenAI provider is untested.** It is wired up, it compiles, it sends the `bot_turn` tool to `/v1/chat/completions` and parses tool calls back out, and as far as the documented API goes the shape is right. But no call has ever been made through it. It sends `max_completion_tokens` and falls back to `max_tokens` if the API rejects that, and any non-200 response is logged with the API's own error text, so if bots go quiet the server log should tell you why. Treat a failure there as a bug worth reporting rather than a wall.

If you get it working, or find it broken, an issue or a PR would be welcome.

## Configuration

`conf/mod_bot_minds.conf.dist` is the template, with every setting annotated and set to its default. There is more in there than is listed below (memory sizing, transcript length, per-event reaction chances, the remaining reply chances and channel routing toggles), but these are the dials most worth reaching for:

| Setting | Default | What it does |
| --- | --- | --- |
| `BotMinds.Enable` | 1 | Master switch for the module. |
| `BotMinds.ApiKey` | empty | Your API key. Empty means silence. |
| `BotMinds.Provider` | `anthropic` | `anthropic` or `openai`. |
| `BotMinds.Model` | `claude-haiku-4-5` | Any tool-calling model from that provider. |
| `BotMinds.MaxReplyChars` | 200 | Hard limit on a spoken line. The model is told this number. |
| `BotMinds.SayDistance` | 30.0 | Earshot for say and yell, in yards. |
| `BotMinds.Route.HandleChannel` | 0 | General, Trade and the rest. Off because a busy channel burns calls. |
| `BotMinds.ReplyChance.Say` | 70 | How likely an uninvolved bystander is to pick up an open remark. Party, Guild, Channel, Whisper and BotToBot have their own. |
| `BotMinds.Attention.FloorWindowSec` | 60 | How long the bot you are talking to keeps the right to answer you. |
| `BotMinds.Attention.MaxBotsToPick` | 2 | Most bots that may answer one line. |
| `BotMinds.Limits.PerBotCooldownSec` | 12 | Quiet time between one bot's unprompted lines. A direct answer ignores it. |
| `BotMinds.Limits.MaxCallsPerMinute` | 60 | Ceiling on API calls in any one minute. The safety rail on your bill. |
| `BotMinds.Ambient.Chance` | 25 | How talkative bots are when nothing is happening. |
| `BotMinds.Ambient.PlayerDistance` | 60.0 | How close you have to be for idle chatter to happen at all. |
| `BotMinds.Typing.Enable` | 0 | Hold a finished line back as though the bot were typing it. |
| `BotMinds.DebugEnabled` | 0 | Log who was picked to answer, who stayed quiet, and why. |

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

`Limits.MaxCallsPerMinute` bounds the worst case no matter what happens in game. At the default of 60 that is 3600 calls an hour, or roughly $7, and you would have to work hard to get near it. `.botminds status` reports the running total, which beats guessing.

## How a line gets spoken

1. **Chat hook** turns an incoming message into a scope (say, party, guild, channel, whisper) and records it in that scope's transcript. Playerbot commands are filtered out.
2. **Attention** decides who answers: a named bot, the bot holding the floor, one member of a small group, or a chance roll per listener. This is the only place reply probability lives.
3. **Governor** decides who may: provider available, hard cap and concurrency free, per-bot cooldown, proximity. A direct answer to someone who addressed the bot skips the cooldown and proximity.
4. **Prompt** is assembled from persona, world state, relationship, memory and the recent transcript, plus one instruction for why this bot is speaking and the shared voice rules.
5. **Provider** returns a `bot_turn` tool call: whether to speak, the words, new memories, relationship change.
6. **Speak** cleans the line up (no emotes, no quoting, trimmed to the character limit on a word boundary), puts it in the right channel, records it, and writes the memories.

## Database

Three tables in the characters database, created by the module's SQL:

- `mod_bot_minds_persona`: one row per bot.
- `mod_bot_minds_memory`: what bots remember, with a subject and a salience.
- `mod_bot_minds_relationship`: affinity per bot per person.

## Debugging

`BotMinds.DebugEnabled = 1` logs who was picked to answer, who stayed quiet, and why. `BotMinds.DebugShowFullPrompt = 1` adds the full prompt for every line, which is very noisy but useful when the voice is wrong.

## License

GNU AGPL v3, consistent with AzerothCore.
