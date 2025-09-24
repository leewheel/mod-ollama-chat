#include "mod-ollama-chat_personality.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include "Log.h"
#include "mod-ollama-chat_config.h"
#include <random>

// Internal personality map
std::string GetBotPersonality(Player* bot)
{
    uint64_t botGuid = bot->GetGUID().GetRawValue();

    // If personality already assigned, return it
    auto it = g_BotPersonalityList.find(botGuid);
    if (it != g_BotPersonalityList.end())
    {
        if(g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] Using existing personality '{}' for bot {}", it->second, bot->GetName());
        }
        return it->second;
    }

    // RP personalities disabled or config not loaded
    if (!g_EnableRPPersonalities || g_PersonalityKeys.empty())
    {
        g_BotPersonalityList[botGuid] = "default";
        return "default";
    }

    // Try to load from database if you have persistence
    if (g_BotPersonalityList.find(botGuid) != g_BotPersonalityList.end())
    {
        // DB stores string keys now
        std::string dbPersonality = g_BotPersonalityList[botGuid];

        if (dbPersonality.empty())
        {
            dbPersonality = "default";
        }

        g_BotPersonalityList[botGuid] = dbPersonality;

        if(g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] Using database personality '{}' for bot {}", dbPersonality, bot->GetName());
        }
        return dbPersonality;
    }

    // Otherwise, assign randomly from config
    uint32 newIdx = urand(0, g_PersonalityKeys.size() - 1);
    std::string chosenPersonality = g_PersonalityKeys[newIdx];
    g_BotPersonalityList[botGuid] = chosenPersonality;

    // Save to database if schema supports string (recommend TEXT or VARCHAR column for personality)
    QueryResult tableExists = CharacterDatabase.Query(
        "SELECT * FROM information_schema.tables WHERE table_schema = 'acore_characters' AND table_name = 'mod_ollama_chat_personality' LIMIT 1;");
    if (!tableExists)
    {
        LOG_INFO("server.loading", "[Ollama Chat] Please source the required database table first");
    }
    else
    {
        CharacterDatabase.Execute("INSERT INTO mod_ollama_chat_personality (guid, personality) VALUES ({}, '{}')", botGuid, chosenPersonality);
    }

    if(g_DebugEnabled)
    {
        LOG_INFO("server.loading", "[Ollama Chat] Assigned new personality '{}' to bot {}", chosenPersonality, bot->GetName());
    }
    return chosenPersonality;
}


std::string GetPersonalityPromptAddition(const std::string& personality)
{
    auto it = g_PersonalityPrompts.find(personality);
    if (it != g_PersonalityPrompts.end())
        return it->second;
    return g_DefaultPersonalityPrompt;
}
