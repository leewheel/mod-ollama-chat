#include "Log.h"
#include "Player.h"
#include "Chat.h"
#include "Channel.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "Config.h"
#include "Common.h"
#include "Guild.h"
#include "ObjectAccessor.h"
#include "World.h"
#include "AiFactory.h"
#include "ChannelMgr.h"
#include <sstream>
#include <vector>
#include <fmt/core.h>
#include <nlohmann/json.hpp>
#include <thread>
#include <algorithm>
#include <random>
#include <cctype>
#include <chrono>
#include <ctime>
#include "DatabaseEnv.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat-utilities.h"
#include "mod-ollama-chat_sentiment.h"
#include <iomanip>
#include "SpellMgr.h"
#include "SpellInfo.h"
#include "SharedDefines.h"
#include "Group.h"
#include "Creature.h"
#include "GameObject.h"
#include "TravelMgr.h"
#include "TravelNode.h"
#include "ObjectMgr.h"
#include "QuestDef.h"

// For AzerothCore range checks
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Map.h"
#include "GridNotifiers.h"

// Forward declarations for internal helper functions.
static bool IsBotEligibleForChatChannelLocal(Player* bot, Player* player,
                                             ChatChannelSourceLocal source, Channel* channel = nullptr);
static std::string GenerateBotPrompt(Player* bot, std::string playerMessage, Player* player);

const char* ChatChannelSourceLocalStr[] =
{
    "Undefined",
    "Say",
    "Party",
    "Raid",
    "Guild",
    "Yell",
    "General"
};

std::string GetConversationEntryKey(uint64_t botGuid, uint64_t playerGuid, const std::string& playerMessage, const std::string& botReply)
{
    // Use a combination that guarantees uniqueness
    return SafeFormat("{}:{}:{}:{}", botGuid, playerGuid, playerMessage, botReply);
}

std::string rtrim(const std::string& s)
{
    const std::string whitespace = " \t\n\r,.!?;:";
    size_t end = s.find_last_not_of(whitespace);
    return (end == std::string::npos) ? "" : s.substr(0, end + 1);
}

ChatChannelSourceLocal GetChannelSourceLocal(uint32_t type)
{
    switch (type)
    {
        case 1:
            return SRC_SAY_LOCAL;
        case 51:
            return SRC_PARTY_LOCAL;
        case 3:
            return SRC_RAID_LOCAL;
        case 5:
            return SRC_GUILD_LOCAL;
        case 6:
            return SRC_YELL_LOCAL;
        case 17:
            return SRC_GENERAL_LOCAL;
        default:
            return SRC_UNDEFINED_LOCAL;
    }
}

Channel* GetValidChannel(uint32_t teamId, const std::string& channelName, Player* player)
{
    ChannelMgr* cMgr = ChannelMgr::forTeam(static_cast<TeamId>(teamId));
    Channel* channel = cMgr->GetChannel(channelName, player);
    if (!channel)
    {
        if(g_DebugEnabled)
        {
            LOG_ERROR("server.loading", "[Ollama Chat] Channel '{}' not found for team {}", channelName, teamId);
        }
    }
    return channel;
}

void PlayerBotChatHandler::OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg)
{
    if (!g_Enable)
        return;

    ChatChannelSourceLocal sourceLocal = GetChannelSourceLocal(type);
    ProcessChat(player, type, lang, msg, sourceLocal, nullptr);
}

void PlayerBotChatHandler::OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Group* /*group*/)
{
    if (!g_Enable)
        return;

    ChatChannelSourceLocal sourceLocal = GetChannelSourceLocal(type);
    ProcessChat(player, type, lang, msg, sourceLocal, nullptr);
}

void PlayerBotChatHandler::OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel)
{
    if (!g_Enable)
        return;

    ChatChannelSourceLocal sourceLocal = GetChannelSourceLocal(type);
    ProcessChat(player, type, lang, msg, sourceLocal, channel);
}

void AppendBotConversation(uint64_t botGuid, uint64_t playerGuid, const std::string& playerMessage, const std::string& botReply)
{
    std::lock_guard<std::mutex> lock(g_ConversationHistoryMutex);
    auto& playerHistory = g_BotConversationHistory[botGuid][playerGuid];
    playerHistory.push_back({ playerMessage, botReply });
    while (playerHistory.size() > g_MaxConversationHistory)
    {
        playerHistory.pop_front();
    }

}

void SaveBotConversationHistoryToDB()
{
    std::lock_guard<std::mutex> lock(g_ConversationHistoryMutex);

    for (const auto& [botGuid, playerMap] : g_BotConversationHistory) {
        for (const auto& [playerGuid, history] : playerMap) {
            for (const auto& pair : history) {
                const std::string& playerMessage = pair.first;
                const std::string& botReply = pair.second;

                std::string escPlayerMsg = playerMessage;
                CharacterDatabase.EscapeString(escPlayerMsg);

                std::string escBotReply = botReply;
                CharacterDatabase.EscapeString(escBotReply);

                CharacterDatabase.Execute(SafeFormat(
                    "INSERT IGNORE INTO mod_ollama_chat_history (bot_guid, player_guid, timestamp, player_message, bot_reply) "
                    "VALUES ({}, {}, NOW(), '{}', '{}')",
                    botGuid, playerGuid, escPlayerMsg, escBotReply));
            }
        }
    }

    // Cleanup: keep only the N most recent entries per bot/player pair
    std::string cleanupQuery = R"SQL(
        WITH ranked_history AS (
            SELECT
                bot_guid,
                player_guid,
                timestamp,
                ROW_NUMBER() OVER (
                    PARTITION BY bot_guid, player_guid
                    ORDER BY timestamp DESC
                ) as rn
            FROM mod_ollama_chat_history
        )
        DELETE FROM mod_ollama_chat_history
        WHERE (bot_guid, player_guid, timestamp) IN (
            SELECT bot_guid, player_guid, timestamp
            FROM ranked_history
            WHERE rn > {}
        );
    )SQL";
    CharacterDatabase.Execute(SafeFormat(cleanupQuery, g_MaxConversationHistory));
}


std::string GetBotHistoryPrompt(uint64_t botGuid, uint64_t playerGuid, std::string playerMessage)
{
    if(!g_EnableChatHistory)
    {
        return "";
    }
    
    std::lock_guard<std::mutex> lock(g_ConversationHistoryMutex);

    std::string result;
    const auto botIt = g_BotConversationHistory.find(botGuid);
    if (botIt == g_BotConversationHistory.end())
        return result;
    const auto playerIt = botIt->second.find(playerGuid);
    if (playerIt == botIt->second.end())
        return result;

    Player* player = ObjectAccessor::FindPlayer(ObjectGuid(playerGuid));
    std::string playerName = player ? player->GetName() : "The player";

    result += SafeFormat(g_ChatHistoryHeaderTemplate, fmt::arg("player_name", playerName));

    for (const auto& entry : playerIt->second) {
        result += SafeFormat(g_ChatHistoryLineTemplate,
            fmt::arg("player_name", playerName),
            fmt::arg("player_message", entry.first),
            fmt::arg("bot_reply", entry.second)
        );
    }

    result += SafeFormat(g_ChatHistoryFooterTemplate,
        fmt::arg("player_name", playerName),
        fmt::arg("player_message", playerMessage)
    );

    return result;
}

// --- Helper: Spells ---
std::string ChatHandler_GetBotSpellInfo(Player* bot)
{
    std::ostringstream spellSummary;
    for (const auto& spellPair : bot->GetSpellMap())
    {
        uint32 spellId = spellPair.first;
        const SpellInfo* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo || spellInfo->Attributes & SPELL_ATTR0_PASSIVE)
            continue;
        if (spellInfo->SpellFamilyName == SPELLFAMILY_GENERIC)
            continue;
        if (bot->HasSpellCooldown(spellId))
            continue;
        std::string effectText;
        for (int i = 0; i < MAX_SPELL_EFFECTS; ++i)
        {
            if (!spellInfo->Effects[i].IsEffect())
                continue;
            switch (spellInfo->Effects[i].Effect)
            {
                case SPELL_EFFECT_SCHOOL_DAMAGE: effectText = "Deals damage"; break;
                case SPELL_EFFECT_HEAL: effectText = "Heals the target"; break;
                case SPELL_EFFECT_APPLY_AURA: effectText = "Applies an aura"; break;
                case SPELL_EFFECT_DISPEL: effectText = "Dispels magic"; break;
                case SPELL_EFFECT_THREAT: effectText = "Generates threat"; break;
                default: continue;
            }
            break;
        }
        if (effectText.empty())
            continue;
        const char* name = spellInfo->SpellName[0];
        if (!name || !*name)
            continue;
        std::string costText;
        if (spellInfo->ManaCost || spellInfo->ManaCostPercentage)
        {
            switch (spellInfo->PowerType)
            {
                case POWER_MANA: costText = std::to_string(spellInfo->ManaCost) + " mana"; break;
                case POWER_RAGE: costText = std::to_string(spellInfo->ManaCost) + " rage"; break;
                case POWER_FOCUS: costText = std::to_string(spellInfo->ManaCost) + " focus"; break;
                case POWER_ENERGY: costText = std::to_string(spellInfo->ManaCost) + " energy"; break;
                case POWER_RUNIC_POWER: costText = std::to_string(spellInfo->ManaCost) + " runic power"; break;
                default: costText = std::to_string(spellInfo->ManaCost) + " unknown resource"; break;
            }
        }
        else
        {
            costText = "no cost";
        }
        spellSummary << "**" << name << "** (ID: " << spellId << ") - " << effectText << ", Costs " << costText << ".\n";
    }
    return spellSummary.str();
}

// --- Helper: Group info ---
std::vector<std::string> ChatHandler_GetGroupStatus(Player* bot)
{
    std::vector<std::string> info;
    if (!bot || !bot->GetGroup()) return info;
    Group* group = bot->GetGroup();
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->GetMap()) continue;
        if(bot == member) continue;
        float dist = bot->GetDistance(member);
        std::string beingAttacked = "";
        if (Unit* attacker = member->GetVictim())
        {
            beingAttacked = " [Under Attack by " + attacker->GetName() +
                            ", Level: " + std::to_string(attacker->GetLevel()) + ", HP: " + std::to_string(attacker->GetHealth()) +
                            "/" + std::to_string(attacker->GetMaxHealth()) + ")]";
        }
        PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(member);
        std::string className = ai ? ai->GetChatHelper()->FormatClass(member->getClass()) : "Unknown";
        std::string raceName = ai ? ai->GetChatHelper()->FormatRace(member->getRace()) : "Unknown";
        info.push_back(
            member->GetName() +
            " (Level: " + std::to_string(member->GetLevel()) +
            ", Class: " + className +
            ", Race: " + raceName +
            ", HP: " + std::to_string(member->GetHealth()) + "/" + std::to_string(member->GetMaxHealth()) +
            ", Dist: " + std::to_string(dist) + ")" + beingAttacked
        );

    }
    return info;
}

// --- Helper: Visible players ---
std::vector<std::string> ChatHandler_GetVisiblePlayers(Player* bot, float radius = 40.0f)
{
    std::vector<std::string> players;
    if (!bot || !bot->GetMap()) return players;
    for (auto const& pair : ObjectAccessor::GetPlayers())
    {
        Player* player = pair.second;
        if (!player || player == bot) continue;
        if (!player->IsInWorld() || player->IsGameMaster()) continue;
        if (player->GetMap() != bot->GetMap()) continue;
        if (!bot->IsWithinDistInMap(player, radius)) continue;
        if (!bot->IsWithinLOS(player->GetPositionX(), player->GetPositionY(), player->GetPositionZ())) continue;
        float dist = bot->GetDistance(player);
        std::string faction = (player->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");
        PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(player);
        std::string className = ai ? ai->GetChatHelper()->FormatClass(player->getClass()) : "Unknown";
        std::string raceName = ai ? ai->GetChatHelper()->FormatRace(player->getRace()) : "Unknown";
        players.push_back(
            "Player: " + player->GetName() +
            " (Level: " + std::to_string(player->GetLevel()) +
            ", Class: " + className +
            ", Race: " + raceName +
            ", Faction: " + faction +
            ", Distance: " + std::to_string(dist) + ")"
        );

    }
    return players;
}

// --- Helper: Visible locations/objects (creatures and gameobjects) ---
std::vector<std::string> ChatHandler_GetVisibleLocations(Player* bot, float radius = 40.0f)
{
    std::vector<std::string> visible;
    if (!bot || !bot->GetMap()) return visible;
    Map* map = bot->GetMap();
    for (auto const& pair : map->GetCreatureBySpawnIdStore())
    {
        Creature* c = pair.second;
        if (!c) continue;
        if (c->GetGUID() == bot->GetGUID()) continue;
        if (!bot->IsWithinDistInMap(c, radius)) continue;
        if (!bot->IsWithinLOS(c->GetPositionX(), c->GetPositionY(), c->GetPositionZ())) continue;
        if (c->IsPet() || c->IsTotem()) continue;
        std::string type;
        if (c->isDead()) type = "DEAD";
        else if (c->IsHostileTo(bot)) type = "ENEMY";
        else if (c->IsFriendlyTo(bot)) type = "FRIENDLY";
        else type = "NEUTRAL";
        float dist = bot->GetDistance(c);
        visible.push_back(
            type + ": " + c->GetName() +
            ", Level: " + std::to_string(c->GetLevel()) +
            ", HP: " + std::to_string(c->GetHealth()) + "/" + std::to_string(c->GetMaxHealth()) +
            ", Distance: " + std::to_string(dist) + ")"
        );
    }
    for (auto const& pair : map->GetGameObjectBySpawnIdStore())
    {
        GameObject* go = pair.second;
        if (!go) continue;
        if (!bot->IsWithinDistInMap(go, radius)) continue;
        if (!bot->IsWithinLOS(go->GetPositionX(), go->GetPositionY(), go->GetPositionZ())) continue;
        float dist = bot->GetDistance(go);
        visible.push_back(
            go->GetName() +
            ", Type: " + std::to_string(go->GetGoType()) +
            ", Distance: " + std::to_string(dist) + ")"
        );
    }
    return visible;
}

// --- Helper: Combat summary ---
std::string ChatHandler_GetCombatSummary(Player* bot)
{
    std::ostringstream oss;
    bool inCombat = bot->IsInCombat();
    Unit* victim = bot->GetVictim();

    // Class-specific resource reporting
    auto classId = bot->getClass();

    auto printResource = [&](std::ostringstream& oss) {
        switch (classId)
        {
            case CLASS_WARRIOR:
                oss << ", Rage: " << bot->GetPower(POWER_RAGE) << "/" << bot->GetMaxPower(POWER_RAGE);
                break;
            case CLASS_ROGUE:
                oss << ", Energy: " << bot->GetPower(POWER_ENERGY) << "/" << bot->GetMaxPower(POWER_ENERGY);
                break;
            case CLASS_DEATH_KNIGHT:
                oss << ", Runic Power: " << bot->GetPower(POWER_RUNIC_POWER) << "/" << bot->GetMaxPower(POWER_RUNIC_POWER);
                break;
            case CLASS_HUNTER:
                oss << ", Focus: " << bot->GetPower(POWER_FOCUS) << "/" << bot->GetMaxPower(POWER_FOCUS);
                break;
            default: // Mana classes
                if (bot->GetMaxPower(POWER_MANA) > 0)
                    oss << ", Mana: " << bot->GetPower(POWER_MANA) << "/" << bot->GetMaxPower(POWER_MANA);
                break;
        }
    };

    if (inCombat)
    {
        oss << "IN COMBAT: ";
        if (victim)
        {
            oss << "Target: " << victim->GetName()
                << ", Level: " << victim->GetLevel()
                << ", HP: " << victim->GetHealth() << "/" << victim->GetMaxHealth();
        }
        else
        {
            oss << "No current target";
        }
        oss << ". ";
        printResource(oss);
    }
    else
    {
        oss << "NOT IN COMBAT. ";
        printResource(oss);
    }
    return oss.str();
}


static std::string GenerateBotGameStateSnapshot(Player* bot)
{
    // Prepare each section
    std::string combat = ChatHandler_GetCombatSummary(bot);

    std::string group;
    std::vector<std::string> groupInfo = ChatHandler_GetGroupStatus(bot);
    if (!groupInfo.empty()) {
        group += "Group members:\n";
        for (const auto& entry : groupInfo) group += " - " + entry + "\n";
    }

    std::string spells = ChatHandler_GetBotSpellInfo(bot);

    std::string quests;
    for (auto const& [questId, qsd] : bot->getQuestStatusMap())
    {
        // look up the template
        Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
        if (!quest)
            continue;

        // get the English title as a fallback
        std::string title = quest->GetTitle();

        // then, if we have a locale record, overwrite it
        if (auto const* locale = sObjectMgr->GetQuestLocale(questId))
        {
            int locIdx = bot->GetSession()->GetSessionDbLocaleIndex();
            if (locIdx >= 0)
                ObjectMgr::GetLocaleString(locale->Title, locIdx, title);
        }

        quests += "Quest \"" + title + "\" status " + std::to_string(qsd.Status) + "\n";
    }

    std::string los;
    std::vector<std::string> losLocs = ChatHandler_GetVisibleLocations(bot);
    if (!losLocs.empty()) {
        for (const auto& entry : losLocs) los += " - " + entry + "\n";
    }

    std::string players;
    std::vector<std::string> nearbyPlayers = ChatHandler_GetVisiblePlayers(bot);
    if (!nearbyPlayers.empty()) {
        for (const auto& entry : nearbyPlayers) players += " - " + entry + "\n";
    }

    // Use template
    return SafeFormat(
        g_ChatBotSnapshotTemplate,
        fmt::arg("combat", combat),
        fmt::arg("group", group),
        fmt::arg("spells", spells),
        fmt::arg("quests", quests),
        fmt::arg("los", los),
        fmt::arg("players", players)
    );
}


void PlayerBotChatHandler::ProcessChat(Player* player, uint32_t /*type*/, uint32_t /*lang*/, std::string& msg, ChatChannelSourceLocal sourceLocal, Channel* channel)
{
    if (player == nullptr) {
        LOG_ERROR("server.loading", "[Ollama Chat] ProcessChat: player is null");
        return;
    }
    if (msg.empty()) {
        return;
    }
    std::string chanName = (channel != nullptr) ? channel->GetName() : "Unknown";
    uint32_t channelId = (channel != nullptr) ? channel->GetChannelId() : 0;
    if(g_DebugEnabled)
    {
        LOG_INFO("server.loading",
                "[Ollama Chat] Player {} sent msg: '{}' | Channel Name: {} | Channel ID: {}",
                player->GetName(), msg, chanName, channelId);
    }

    std::string trimmedMsg = rtrim(msg);
    for (const std::string& blacklist : g_BlacklistCommands)
    {
        if (trimmedMsg.find(blacklist) == 0)
        {
            if(g_DebugEnabled)
            {
                LOG_INFO("server.loading", "[Ollama Chat] Message starts with '{}' (blacklisted). Skipping bot responses.", blacklist);
            }
            return;
        }
    }
             
    PlayerbotAI* senderAI = sPlayerbotsMgr->GetPlayerbotAI(player);
    bool senderIsBot = (senderAI && senderAI->IsBotAI());
    
    std::vector<Player*> eligibleBots;
    if (channel != nullptr)
    {
        ChannelMgr* cMgr = ChannelMgr::forTeam(static_cast<TeamId>(player->GetTeamId()));
        Channel* senderChannel = cMgr->GetChannel(channel->GetName(), player);
        if (senderChannel)
        {
            auto const& allPlayers = ObjectAccessor::GetPlayers();
            for (auto const& itr : allPlayers)
            {
                Player* candidate = itr.second;
                if (!candidate->IsInWorld())
                    continue;
                ChannelMgr* candidateCM = ChannelMgr::forTeam(static_cast<TeamId>(candidate->GetTeamId()));
                Channel* candidateChannel = candidateCM->GetChannel(channel->GetName(), candidate);
                if (candidateChannel && candidateChannel->GetChannelId() == senderChannel->GetChannelId())
                {
                    eligibleBots.push_back(candidate);
                }
            }
        }
    }
    else
    {
        auto const& allPlayers = ObjectAccessor::GetPlayers();
        for (auto const& itr : allPlayers)
        {
            Player* candidate = itr.second;
            if (candidate->IsInWorld())
                eligibleBots.push_back(candidate);
        }
    }
    
    std::vector<Player*> candidateBots;
    for (Player* bot : eligibleBots)
    {
        if (!bot)
        {
            continue;
        }
        if (IsBotEligibleForChatChannelLocal(bot, player, sourceLocal, channel))
            candidateBots.push_back(bot);
    }
    
    uint32_t chance = senderIsBot ? g_BotReplyChance : g_PlayerReplyChance;
    if (senderIsBot)
    {
        bool realPlayerNearby = false;
        for (auto const& itr : ObjectAccessor::GetPlayers())
        {
            Player* candidate = itr.second;
            if (candidate == player)
                continue;
            if (!candidate->IsInWorld())
                continue;
            if (player->GetDistance(candidate) > g_GeneralDistance)
                continue;
            if (!sPlayerbotsMgr->GetPlayerbotAI(candidate))
            {
                realPlayerNearby = true;
                break;
            }
        }
        if (!realPlayerNearby)
            chance = 0;
    }
    
        std::vector<Player*> finalCandidates;
    std::vector<std::pair<size_t, Player*>> mentionedBots;

    for (Player* bot : candidateBots)
    {
        if (!bot)
        {
            continue;
        }
        if (g_DisableRepliesInCombat && bot->IsInCombat())
        {
            continue;
        }
        size_t pos = trimmedMsg.find(bot->GetName());
        if (pos != std::string::npos)
        {
            mentionedBots.emplace_back(pos, bot);
        }
    }

    if (!mentionedBots.empty())
    {
        std::sort(mentionedBots.begin(), mentionedBots.end(),
                  [](auto &a, auto &b) { return a.first < b.first; });
        Player* chosen = mentionedBots.front().second;
        if (!(g_DisableRepliesInCombat && chosen->IsInCombat()))
        {
            finalCandidates.push_back(chosen);
        }
    }
    else
    {
        for (Player* bot : candidateBots)
        {
            if (g_DisableRepliesInCombat && bot->IsInCombat())
            {
                continue;
            }
            if (urand(0, 99) < chance)
            {
                finalCandidates.push_back(bot);
            }
        }
    }

    
    if (finalCandidates.empty())
    {
        if(g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] No eligible bots found to respond to message '{}'.", msg);
        }
        return;
    }
    
    if (finalCandidates.size() > g_MaxBotsToPick)
    {
        std::random_device rd;
        std::mt19937 g(rd());
        std::shuffle(finalCandidates.begin(), finalCandidates.end(), g);
        uint32_t countToPick = urand(1, g_MaxBotsToPick);
        finalCandidates.resize(countToPick);
    }
    
    uint64_t senderGuid = player->GetGUID().GetRawValue();
    
    for (Player* bot : finalCandidates)
    {
        float distance = player->GetDistance(bot);
        if(g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] Bot {} (distance: {}) is set to respond.", bot->GetName(), distance);
        }
        if (bot == nullptr) {
            continue;
        }
        std::string prompt = GenerateBotPrompt(bot, msg, player);
        uint64_t botGuid = bot->GetGUID().GetRawValue();
        
        std::thread([botGuid, senderGuid, prompt, sourceLocal, channelId = (channel ? channel->GetChannelId() : 0), msg]() {
            try {
                // Use the QueryManager to submit the query.
                auto responseFuture = SubmitQuery(prompt);
                if (!responseFuture.valid())
                {
                    return;
                }
                std::string response = responseFuture.get();

                // Reacquire pointers by GUID.
                Player* botPtr = ObjectAccessor::FindPlayer(ObjectGuid(botGuid));
                Player* senderPtr = ObjectAccessor::FindPlayer(ObjectGuid(senderGuid));
                if (!botPtr)
                {
                    if(g_DebugEnabled)
                    {
                        LOG_ERROR("server.loading", "[Ollama Chat] Failed to reacquire bot from GUID {}", botGuid);
                    }
                    return;
                }
                if (!senderPtr)
                {
                    if(g_DebugEnabled)
                    {
                        LOG_ERROR("server.loading", "[Ollama Chat] Failed to reacquire sender from GUID {}", senderGuid);
                    }
                    return;
                }
                if (response.empty())
                {
                    if(g_DebugEnabled)
                    {
                        LOG_ERROR("server.loading", "[Ollama Chat] Bot {} received empty response from Ollama API.", botPtr->GetName());
                    }
                    return;
                }
                PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(botPtr);
                if (!botAI)
                {
                    if(g_DebugEnabled)
                    {
                        LOG_ERROR("server.loading", "[Ollama Chat] No PlayerbotAI found for bot {}", botPtr->GetName());
                    }
                    return;
                }
                // Route the response.
                if (channelId != 0)
                {
                    ChatChannelId chanId = static_cast<ChatChannelId>(channelId);
                    botAI->SayToChannel(response, chanId);
                }
                else
                {
                    switch (sourceLocal)
                    {
                        case SRC_GUILD_LOCAL: botAI->SayToGuild(response); break;
                        case SRC_PARTY_LOCAL: botAI->SayToParty(response); break;
                        case SRC_RAID_LOCAL:  botAI->SayToRaid(response); break;
                        case SRC_SAY_LOCAL:   botAI->Say(response); break;
                        case SRC_YELL_LOCAL:  botAI->Yell(response); break;
                        default:              botAI->Say(response); break;
                    }
                }
                
                // Update sentiment based on the player's message
                UpdateBotPlayerSentiment(botPtr, senderPtr, msg);
                
                AppendBotConversation(botGuid, senderGuid, msg, response);
                float respDistance = senderPtr->GetDistance(botPtr);
                if(g_DebugEnabled)
                {
                    LOG_INFO("server.loading", "[Ollama Chat] Bot {} (distance: {}) responded: {}", botPtr->GetName(), respDistance, response);
                }
            }
            catch (const std::exception& ex)
            {
                if(g_DebugEnabled)
                {
                    LOG_ERROR("server.loading", "[Ollama Chat] Exception in bot response thread: {}", ex.what());
                }
            }
        }).detach();

    }
}

static bool IsBotEligibleForChatChannelLocal(Player* bot, Player* player, ChatChannelSourceLocal source, Channel* channel)
{
    if (!bot || !player || bot == player)
        return false;
    if (bot->GetTeamId() != player->GetTeamId())
        return false;
    if (!sPlayerbotsMgr->GetPlayerbotAI(bot))
        return false;
    if (channel && !bot->IsInChannel(channel))
        return false;
    
    bool isInParty = (player->GetGroup() && bot->GetGroup() && (player->GetGroup() == bot->GetGroup()));
    float threshold = 0.0f;
    switch (source)
    {
        case SRC_SAY_LOCAL:    threshold = g_SayDistance;     break;
        case SRC_YELL_LOCAL:   threshold = g_YellDistance;    break;
        case SRC_GENERAL_LOCAL:threshold = g_GeneralDistance; break;
        default:               threshold = 0.0f;              break;
    }
    switch (source)
    {
        case SRC_GUILD_LOCAL:
            return (player->GetGuild() && bot->GetGuildId() == player->GetGuildId());
        case SRC_PARTY_LOCAL:
            return isInParty;
        case SRC_RAID_LOCAL:
            return isInParty;
        case SRC_SAY_LOCAL:
            return (threshold > 0.0f && player->GetDistance(bot) > threshold) ? false : true;
        case SRC_YELL_LOCAL:
            return (threshold > 0.0f && player->GetDistance(bot) > threshold) ? false : true;
        case SRC_GENERAL_LOCAL:
            return (threshold > 0.0f && player->GetDistance(bot) > threshold) ? false : true;
        default:
            return false;
    }
}

std::string GenerateBotPrompt(Player* bot, std::string playerMessage, Player* player)
{  
    if (!bot || !player) {
        return "";
    }
    PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(bot);
    if (botAI == nullptr) {
        return "";
    }
    ChatHelper* helper = botAI->GetChatHelper();
    if (helper == nullptr) {
        return "";
    }
    if (g_ChatPromptTemplate.empty()) {
        LOG_ERROR("server.loading", "[Ollama Chat] GenerateBotPrompt: template is empty");
        return "";
    }

    AreaTableEntry const* botCurrentArea = botAI->GetCurrentArea();
    AreaTableEntry const* botCurrentZone = botAI->GetCurrentZone();

    uint64_t botGuid                = bot->GetGUID().GetRawValue();
    uint64_t playerGuid             = player->GetGUID().GetRawValue();

    std::string personality         = GetBotPersonality(bot);
    std::string personalityPrompt   = GetPersonalityPromptAddition(personality);
    std::string botName             = bot->GetName();
    uint32_t botLevel               = bot->GetLevel();
    uint8_t botGenderByte           = bot->getGender();
    std::string botAreaName         = botCurrentArea ? botAI->GetLocalizedAreaName(botCurrentArea): "UnknownArea";
    std::string botZoneName         = botCurrentZone ? botAI->GetLocalizedAreaName(botCurrentZone): "UnknownZone";
    std::string botMapName          = bot->GetMap() ? bot->GetMap()->GetMapName() : "UnknownMap";
    std::string botClass            = botAI->GetChatHelper()->FormatClass(bot->getClass());
    std::string botRace             = botAI->GetChatHelper()->FormatRace(bot->getRace());
    std::string botRole             = ChatHelper::FormatClass(bot, AiFactory::GetPlayerSpecTab(bot));
    std::string botGender           = (botGenderByte == 0 ? "Male" : "Female");
    std::string botFaction          = (bot->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");
    std::string botGuild            = (bot->GetGuild() ? bot->GetGuild()->GetName() : "No Guild");
    std::string botGroupStatus      = (bot->GetGroup() ? "In a group" : "Solo");
    uint32_t botGold                = bot->GetMoney() / 10000;

    std::string playerName          = player->GetName();
    uint32_t playerLevel            = player->GetLevel();
    std::string playerClass         = botAI->GetChatHelper()->FormatClass(player->getClass());
    std::string playerRace          = botAI->GetChatHelper()->FormatRace(player->getRace());
    std::string playerRole          = ChatHelper::FormatClass(player, AiFactory::GetPlayerSpecTab(player));
    uint8_t playerGenderByte        = player->getGender();
    std::string playerGender        = (playerGenderByte == 0 ? "Male" : "Female");
    std::string playerFaction       = (player->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");
    std::string playerGuild         = (player->GetGuild() ? player->GetGuild()->GetName() : "No Guild");
    std::string playerGroupStatus   = (player->GetGroup() ? "In a group" : "Solo");
    uint32_t playerGold             = player->GetMoney() / 10000;
    float playerDistance            = player->IsInWorld() && bot->IsInWorld() ? player->GetDistance(bot) : -1.0f;

    std::string chatHistory         = GetBotHistoryPrompt(botGuid, playerGuid, playerMessage);
    std::string sentimentInfo       = GetSentimentPromptAddition(bot, player);

    std::string extraInfo = SafeFormat(
        g_ChatExtraInfoTemplate,
        fmt::arg("bot_race", botRace),
        fmt::arg("bot_gender", botGender),
        fmt::arg("bot_role", botRole),
        fmt::arg("bot_faction", botFaction),
        fmt::arg("bot_guild", botGuild),
        fmt::arg("bot_group_status", botGroupStatus),
        fmt::arg("bot_gold", botGold),
        fmt::arg("player_race", playerRace),
        fmt::arg("player_gender", playerGender),
        fmt::arg("player_role", playerRole),
        fmt::arg("player_faction", playerFaction),
        fmt::arg("player_guild", playerGuild),
        fmt::arg("player_group_status", playerGroupStatus),
        fmt::arg("player_gold", playerGold),
        fmt::arg("player_distance", playerDistance),
        fmt::arg("bot_area", botAreaName),
        fmt::arg("bot_zone", botZoneName),
        fmt::arg("bot_map", botMapName)
    );
    
    std::string prompt = SafeFormat(
        g_ChatPromptTemplate,
        fmt::arg("bot_name", botName),
        fmt::arg("bot_level", botLevel),
        fmt::arg("bot_class", botClass),
        fmt::arg("bot_personality", personalityPrompt),
        fmt::arg("player_level", playerLevel),
        fmt::arg("player_class", playerClass),
        fmt::arg("player_name", playerName),
        fmt::arg("player_message", playerMessage),
        fmt::arg("extra_info", extraInfo),
        fmt::arg("chat_history", chatHistory),
        fmt::arg("sentiment_info", sentimentInfo)
    );

    if(g_EnableChatBotSnapshotTemplate)
    {
        prompt += GenerateBotGameStateSnapshot(bot);
    }

    return prompt;
}