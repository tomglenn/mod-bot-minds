#ifndef MOD_BOT_MINDS_RANDOM_H
#define MOD_BOT_MINDS_RANDOM_H

#include "ScriptMgr.h"

// --------------------------------------------
// Unprompted chatter. Bots near a real player occasionally say something about
// what is around them, through the same pipeline as everything else.
// --------------------------------------------
class BotMindsAmbientChatter : public WorldScript
{
public:
    BotMindsAmbientChatter();
    void OnUpdate(uint32 diff) override;
};

#endif // MOD_BOT_MINDS_RANDOM_H
