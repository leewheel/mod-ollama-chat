#include "mod-ollama-chat_personality.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include "Log.h"
#include "mod-ollama-chat_config.h"
#include <random>

// Internal personality map
static std::unordered_map<uint64_t, std::string> botPersonalities;

extern bool g_EnableRPPersonalities;

std::string GetBotPersonality(Player* bot)
{
    uint64_t botGuid = bot->GetGUID().GetRawValue();

    // If personality already assigned, return it
    auto it = botPersonalities.find(botGuid);
    if (it != botPersonalities.end())
    {
        if(g_DebugEnabled)
        {
            LOG_INFO("server.loading", "Using existing personality '{}' for bot {}", it->second, bot->GetName());
        }
        return it->second;
    }

    // RP personalities disabled or config not loaded
    if (!g_EnableRPPersonalities || g_PersonalityKeys.empty())
    {
        botPersonalities[botGuid] = "default";
        return "default";
    }

    // Try to load from database if you have persistence
    if (botPersonalityList.find(botGuid) != botPersonalityList.end())
    {
        // DB stores string keys now
        uint32_t dbIdx = botPersonalityList[botGuid];
        std::string dbPersonality;
        if (dbIdx < g_PersonalityKeys.size())
            dbPersonality = g_PersonalityKeys[dbIdx];
        else
            dbPersonality = "default";

        botPersonalities[botGuid] = dbPersonality;
        if(g_DebugEnabled)
        {
            LOG_INFO("server.loading", "Using database personality '{}' for bot {}", dbPersonality, bot->GetName());
        }
        return dbPersonality;
    }

    // Otherwise, assign randomly from config
    uint32 newIdx = urand(0, g_PersonalityKeys.size() - 1);
    std::string chosenPersonality = g_PersonalityKeys[newIdx];
    botPersonalities[botGuid] = chosenPersonality;

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
        LOG_INFO("server.loading", "Assigned new personality '{}' to bot {}", chosenPersonality, bot->GetName());
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
