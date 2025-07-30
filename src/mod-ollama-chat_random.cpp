#include "mod-ollama-chat_random.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_handler.h"
#include "mod-ollama-chat_sentiment.h"
#include "Log.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotMgr.h"
#include "ObjectAccessor.h"
#include "Chat.h"
#include "fmt/core.h"
#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_personality.h"
#include "mod-ollama-chat-utilities.h"
#include "GridNotifiersImpl.h"
#include "CellImpl.h"
#include "Map.h"
#include "GridNotifiers.h"
#include <vector>
#include <random>
#include <thread>
#include <ctime>
#include "Item.h"
#include "Bag.h"
#include "SpellMgr.h"
#include "AiFactory.h"
#include "ObjectMgr.h"
#include "QuestDef.h"

OllamaBotRandomChatter::OllamaBotRandomChatter() : WorldScript("OllamaBotRandomChatter") {}

std::unordered_map<uint64_t, time_t> nextRandomChatTime;

void OllamaBotRandomChatter::OnUpdate(uint32 diff)
{
    if (!g_Enable || !g_EnableRandomChatter)
        return;

    if (g_ConversationHistorySaveInterval > 0)
    {
        time_t now = time(nullptr);
        if (difftime(now, g_LastHistorySaveTime) >= g_ConversationHistorySaveInterval * 60)
        {
            SaveBotConversationHistoryToDB();
            g_LastHistorySaveTime = now;
        }
    }

    // Save sentiment data periodically
    if (g_EnableSentimentTracking && g_SentimentSaveInterval > 0)
    {
        time_t now = time(nullptr);
        if (difftime(now, g_LastSentimentSaveTime) >= g_SentimentSaveInterval * 60)
        {
            SaveBotPlayerSentimentsToDB();
            g_LastSentimentSaveTime = now;
        }
    }

    static uint32_t timer = 0;
    if (timer <= diff)
    {
        timer = 30000;
        HandleRandomChatter();
    }
    else
    {
        timer -= diff;
    }
}

void OllamaBotRandomChatter::HandleRandomChatter()
{
    auto const& allPlayers = ObjectAccessor::GetPlayers();

    std::vector<Player*> realPlayers;
    for (auto const& itr : allPlayers)
    {
        Player* player = itr.second;
        if (!player->IsInWorld()) continue;
        if (!sPlayerbotsMgr->GetPlayerbotAI(player))
            realPlayers.push_back(player);
    }

    std::unordered_set<uint64_t> processedBotsThisTick;

    for (Player* realPlayer : realPlayers)
    {
        std::vector<Player*> botsInRange;
        for (auto const& itr : allPlayers)
        {
            Player* bot = itr.second;
            PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(bot);
            if (!ai) continue;
            if (!bot->IsInWorld() || bot->IsBeingTeleported()) continue;
            if (bot->GetDistance(realPlayer) > g_RandomChatterRealPlayerDistance) continue;
            if (processedBotsThisTick.count(bot->GetGUID().GetRawValue())) continue;
            botsInRange.push_back(bot);
        }

        std::shuffle(botsInRange.begin(), botsInRange.end(), std::mt19937(std::random_device()()));

        uint32_t botsToProcess = std::min((uint32_t)botsInRange.size(), g_RandomChatterMaxBotsPerPlayer);
        for (uint32_t i = 0; i < botsToProcess; ++i)
        {
            Player* bot = botsInRange[i];
            if (g_DisableRepliesInCombat && bot->IsInCombat())
            {
                continue;
            }

            PlayerbotAI* ai = sPlayerbotsMgr->GetPlayerbotAI(bot);
            uint64_t guid = bot->GetGUID().GetRawValue();

            processedBotsThisTick.insert(guid);

            time_t now = time(nullptr);

            if (nextRandomChatTime.find(guid) == nextRandomChatTime.end())
            {
                nextRandomChatTime[guid] = now + urand(g_MinRandomInterval, g_MaxRandomInterval);
                continue;
            }

            if (now < nextRandomChatTime[guid])
                continue;

            if(urand(0, 99) > g_RandomChatterBotCommentChance)
                continue;

            std::string environmentInfo;
            std::vector<std::string> candidateComments;

            // Creature
            {
                Unit* unitInRange = nullptr;
                Acore::AnyUnitInObjectRangeCheck creatureCheck(bot, g_SayDistance);
                Acore::UnitSearcher<Acore::AnyUnitInObjectRangeCheck> creatureSearcher(bot, unitInRange, creatureCheck);
                Cell::VisitGridObjects(bot, creatureSearcher, g_SayDistance);
                if (unitInRange && unitInRange->GetTypeId() == TYPEID_UNIT)
                    if (!g_EnvCommentCreature.empty()) {
                        uint32_t idx = g_EnvCommentCreature.size() == 1 ? 0 : urand(0, g_EnvCommentCreature.size() - 1);
                        std::string templ = g_EnvCommentCreature[idx];
                        candidateComments.push_back(SafeFormat(templ, fmt::arg("creature_name", unitInRange->ToCreature()->GetName())));
                    }
            }

            // Game Object
            {
                Acore::GameObjectInRangeCheck goCheck(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), g_SayDistance);
                GameObject* goInRange = nullptr;
                Acore::GameObjectSearcher<Acore::GameObjectInRangeCheck> goSearcher(bot, goInRange, goCheck);
                Cell::VisitGridObjects(bot, goSearcher, g_SayDistance);
                if (goInRange)
                {
                    if (!g_EnvCommentGameObject.empty()) {
                        uint32_t idx = g_EnvCommentGameObject.size() == 1 ? 0 : urand(0, g_EnvCommentGameObject.size() - 1);
                        std::string templ = g_EnvCommentGameObject[idx];
                        std::string gameObjectName = goInRange->GetName();
                        candidateComments.push_back(SafeFormat(templ, fmt::arg("object_name", gameObjectName)));
                    }
                }
            }

            // Equipped Item
            {
                std::vector<Item*> equippedItems;
                for (uint8_t slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
                    if (Item* item = bot->GetItemByPos(slot))
                        equippedItems.push_back(item);

                if (!equippedItems.empty())
                {
                    uint32_t eqIdx = equippedItems.size() == 1 ? 0 : urand(0, equippedItems.size() - 1);
                    Item* randomEquipped = equippedItems[eqIdx];
                    if (!g_EnvCommentEquippedItem.empty()) {
                        uint32_t tempIdx = g_EnvCommentEquippedItem.size() == 1 ? 0 : urand(0, g_EnvCommentEquippedItem.size() - 1);
                        std::string templ = g_EnvCommentEquippedItem[tempIdx];
                        candidateComments.push_back(SafeFormat(templ, fmt::arg("item_name", randomEquipped->GetTemplate()->Name1)));
                    }
                }
            }

            // Bag Item
            {
                std::vector<Item*> bagItems;
                for (uint32_t bagSlot = 0; bagSlot < 5; ++bagSlot)
                {
                    if (Bag* bag = bot->GetBagByPos(bagSlot))
                    {
                        for (uint32_t i = 0; i < bag->GetBagSize(); ++i)
                            if (Item* bagItem = bag->GetItemByPos(i))
                                bagItems.push_back(bagItem);
                    }
                }
                if (!bagItems.empty())
                {
                    uint32_t bagIdx = bagItems.size() == 1 ? 0 : urand(0, bagItems.size() - 1);
                    Item* randomBagItem = bagItems[bagIdx];

                    if (!g_EnvCommentBagItem.empty()) {
                        uint32_t tempIdx = g_EnvCommentBagItem.size() == 1 ? 0 : urand(0, g_EnvCommentBagItem.size() - 1);
                        std::string templ = g_EnvCommentBagItem[tempIdx];
                        candidateComments.push_back(SafeFormat(templ, fmt::arg("item_name", randomBagItem->GetTemplate()->Name1)));
                    }

                    if (!g_EnvCommentBagItemSell.empty()) {
                        uint32_t tempIdx = g_EnvCommentBagItemSell.size() == 1 ? 0 : urand(0, g_EnvCommentBagItemSell.size() - 1);
                        std::string templ = g_EnvCommentBagItemSell[tempIdx];
                        candidateComments.push_back(SafeFormat(templ,
                            fmt::arg("item_count", randomBagItem->GetCount()),
                            fmt::arg("item_name", randomBagItem->GetTemplate()->Name1)
                        ));
                    }
                }
            }

            // Spell
            {
                struct NamedSpell
                {
                    uint32 id;
                    std::string name;
                    std::string effect;
                    std::string cost;
                };
                std::vector<NamedSpell> validSpells;
                for (const auto& spellPair : bot->GetSpellMap())
                {
                    uint32 spellId = spellPair.first;
                    const SpellInfo* spellInfo = sSpellMgr->GetSpellInfo(spellId);
                    if (!spellInfo) continue;
                    if (spellInfo->Attributes & SPELL_ATTR0_PASSIVE)
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
                            case SPELL_EFFECT_APPLY_AURA: effectText = "Applies an effect"; break;
                            case SPELL_EFFECT_DISPEL: effectText = "Dispels magic"; break;
                            case SPELL_EFFECT_THREAT: effectText = "Generates threat"; break;
                            default: continue;
                        }
                        if (!effectText.empty())
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

                    validSpells.push_back({spellId, name, effectText, costText});
                }

                if (!validSpells.empty())
                {
                    uint32_t spellIdx = validSpells.size() == 1 ? 0 : urand(0, validSpells.size() - 1);
                    const NamedSpell& randomSpell = validSpells[spellIdx];
                    if (!g_EnvCommentSpell.empty())
                    {
                        uint32_t tempIdx = g_EnvCommentSpell.size() == 1 ? 0 : urand(0, g_EnvCommentSpell.size() - 1);
                        std::string templ = g_EnvCommentSpell[tempIdx];
                        candidateComments.push_back(SafeFormat(
                            templ,
                            fmt::arg("spell_name", randomSpell.name),
                            fmt::arg("spell_effect", randomSpell.effect),
                            fmt::arg("spell_cost", randomSpell.cost)
                        ));
                    }
                }
            }

            // Quest Area
            {
                std::vector<std::string> questAreas;
                for (auto const& qkv : sObjectMgr->GetQuestTemplates())
                {
                    Quest const* qt = qkv.second;
                    if (!qt) continue;
                    int32 qlevel = qt->GetQuestLevel();
                    int32 plevel = bot->GetLevel();
                    if (qlevel < plevel - 2 || qlevel > plevel + 2)
                        continue;
                    uint32 zone = qt->GetZoneOrSort();
                    if (zone == 0) continue;
                    if (auto const* area = sAreaTableStore.LookupEntry(zone))
                    {
                        if (!g_EnvCommentQuestArea.empty()) {
                            uint32_t idx = g_EnvCommentQuestArea.size() == 1 ? 0 : urand(0, g_EnvCommentQuestArea.size() - 1);
                            std::string templ = g_EnvCommentQuestArea[idx];
                            questAreas.push_back(SafeFormat(templ, fmt::arg("quest_area", area->area_name[LocaleConstant::LOCALE_enUS])));
                        }
                    }
                }
                if (!questAreas.empty())
                {
                    uint32_t qIdx = questAreas.size() == 1 ? 0 : urand(0, questAreas.size() - 1);
                    candidateComments.push_back(questAreas[qIdx]);
                }
            }

            // Vendor
            {
                Unit* unit = nullptr;
                Acore::AnyUnitInObjectRangeCheck check(bot, g_SayDistance);
                Acore::UnitSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, unit, check);
                Cell::VisitGridObjects(bot, searcher, g_SayDistance);

                if (unit && unit->GetTypeId() == TYPEID_UNIT)
                {
                    Creature* vendor = unit->ToCreature();
                    if (vendor->HasNpcFlag(UNIT_NPC_FLAG_VENDOR))
                    {
                        if (!g_EnvCommentVendor.empty()) {
                            uint32_t idx = g_EnvCommentVendor.size() == 1 ? 0 : urand(0, g_EnvCommentVendor.size() - 1);
                            std::string templ = g_EnvCommentVendor[idx];
                            candidateComments.push_back(SafeFormat(templ, fmt::arg("vendor_name", vendor->GetName())));
                        }
                    }
                }
            }

            // Questgiver
            {
                Unit* unit = nullptr;
                Acore::AnyUnitInObjectRangeCheck check(bot, g_SayDistance);
                Acore::UnitSearcher<Acore::AnyUnitInObjectRangeCheck> searcher(bot, unit, check);
                Cell::VisitGridObjects(bot, searcher, g_SayDistance);

                if (unit && unit->GetTypeId() == TYPEID_UNIT)
                {
                    Creature* giver = unit->ToCreature();
                    if (giver->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER))
                    {
                        auto bounds = sObjectMgr->GetCreatureQuestRelationBounds(giver->GetEntry());
                        int n       = std::distance(bounds.first, bounds.second);
                        if (!g_EnvCommentQuestgiver.empty()) {
                            uint32_t idx = g_EnvCommentQuestgiver.size() == 1 ? 0 : urand(0, g_EnvCommentQuestgiver.size() - 1);
                            std::string templ = g_EnvCommentQuestgiver[idx];
                            candidateComments.push_back(SafeFormat(templ,
                                fmt::arg("questgiver_name", giver->GetName()),
                                fmt::arg("quest_count", n)
                            ));
                        }
                    }
                }
            }

            // Free bag slots
            {
                int freeSlots = 0;
                for (uint8 i = INVENTORY_SLOT_ITEM_START; i < INVENTORY_SLOT_ITEM_END; ++i)
                    if (!bot->GetItemByPos(i))
                        ++freeSlots;
                for (uint8 b = INVENTORY_SLOT_BAG_START; b < INVENTORY_SLOT_BAG_END; ++b)
                    if (Bag* bag = bot->GetBagByPos(b))
                        freeSlots += bag->GetFreeSlots();

                if (!g_EnvCommentBagSlots.empty()) {
                    uint32_t idx = g_EnvCommentBagSlots.size() == 1 ? 0 : urand(0, g_EnvCommentBagSlots.size() - 1);
                    std::string templ = g_EnvCommentBagSlots[idx];
                    candidateComments.push_back(SafeFormat(templ, fmt::arg("bag_slots", freeSlots)));
                }
            }

            // Dungeon
            {
                if (bot->GetMap() && bot->GetMap()->IsDungeon())
                {
                    std::string name = bot->GetMap()->GetMapName();
                    if (!g_EnvCommentDungeon.empty()) {
                        uint32_t idx = g_EnvCommentDungeon.size() == 1 ? 0 : urand(0, g_EnvCommentDungeon.size() - 1);
                        std::string templ = g_EnvCommentDungeon[idx];
                        candidateComments.push_back(SafeFormat(templ, fmt::arg("dungeon_name", name)));
                    }
                }
            }

            // Unfinished Quest
            {
                std::vector<std::string> unfinished;
                for (auto const& qs : bot->getQuestStatusMap())
                {
                    if (qs.second.Status == QUEST_STATUS_INCOMPLETE)
                    {
                        if (auto* qt = sObjectMgr->GetQuestTemplate(qs.first))
                            if (!g_EnvCommentUnfinishedQuest.empty()) {
                                uint32_t idx = g_EnvCommentUnfinishedQuest.size() == 1 ? 0 : urand(0, g_EnvCommentUnfinishedQuest.size() - 1);
                                std::string templ = g_EnvCommentUnfinishedQuest[idx];
                                unfinished.push_back(SafeFormat(templ, fmt::arg("quest_name", qt->GetTitle())));
                            }
                    }
                }
                if (!unfinished.empty())
                {
                    uint32_t uIdx = unfinished.size() == 1 ? 0 : urand(0, unfinished.size() - 1);
                    candidateComments.push_back(unfinished[uIdx]);
                }
            }

            if (!candidateComments.empty())
            {
                uint32_t index = candidateComments.size() == 1 ? 0 : urand(0, candidateComments.size() - 1);
                environmentInfo = candidateComments[index];
            }
            else
            {
                environmentInfo = "";
            }


            auto prompt = [bot, &environmentInfo]()
            {
                PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(bot);
                if (!botAI)
                    return std::string("Error, no bot AI");

                std::string personality         = GetBotPersonality(bot);
                std::string personalityPrompt   = GetPersonalityPromptAddition(personality);
                std::string botName             = bot->GetName();
                uint32_t botLevel               = bot->GetLevel();
                std::string botClass            = botAI->GetChatHelper()->FormatClass(bot->getClass());
                std::string botRace             = botAI->GetChatHelper()->FormatRace(bot->getRace());
                std::string botRole             = ChatHelper::FormatClass(bot, AiFactory::GetPlayerSpecTab(bot));
                std::string botGender           = (bot->getGender() == 0 ? "Male" : "Female");
                std::string botFaction          = (bot->GetTeamId() == TEAM_ALLIANCE ? "Alliance" : "Horde");

                AreaTableEntry const* botCurrentArea = botAI->GetCurrentArea();
                AreaTableEntry const* botCurrentZone = botAI->GetCurrentZone();
                std::string botAreaName = botCurrentArea ? botAI->GetLocalizedAreaName(botCurrentArea) : "UnknownArea";
                std::string botZoneName = botCurrentZone ? botAI->GetLocalizedAreaName(botCurrentZone) : "UnknownZone";
                std::string botMapName  = bot->GetMap() ? bot->GetMap()->GetMapName() : "UnknownMap";

                std::string prompt = SafeFormat(
                    g_RandomChatterPromptTemplate,
                    fmt::arg("bot_name", botName),
                    fmt::arg("bot_level", botLevel),
                    fmt::arg("bot_class", botClass),
                    fmt::arg("bot_race", botRace),
                    fmt::arg("bot_gender", botGender),
                    fmt::arg("bot_role", botRole),
                    fmt::arg("bot_faction", botFaction),
                    fmt::arg("bot_area", botAreaName),
                    fmt::arg("bot_zone", botZoneName),
                    fmt::arg("bot_map", botMapName),
                    fmt::arg("bot_personality", personalityPrompt),
                    fmt::arg("environment_info", environmentInfo)
                );

                return prompt;

            }();

            if(g_DebugEnabled)
            {
                LOG_INFO("server.loading", "[Ollama Chat] Random Message Prompt: {} ", prompt);
            }

            uint64_t botGuid = bot->GetGUID().GetRawValue();

            std::thread([botGuid, prompt]() {
                try {
                    Player* botPtr = ObjectAccessor::FindPlayer(ObjectGuid(botGuid));
                    if (!botPtr) return;
                    std::string response = QueryOllamaAPI(prompt);
                    if (response.empty()) return;
                    botPtr = ObjectAccessor::FindPlayer(ObjectGuid(botGuid));
                    if (!botPtr) return;
                    PlayerbotAI* botAI = sPlayerbotsMgr->GetPlayerbotAI(botPtr);
                    if (!botAI) return;
                    if (botPtr->GetGroup())
                        botAI->SayToParty(response);
                    else {
                        std::vector<std::string> channels = {"General", "Say"};
                        std::random_device rd;
                        std::mt19937 gen(rd());
                        std::uniform_int_distribution<size_t> dist(0, channels.size() - 1);
                        std::string selectedChannel = channels[dist(gen)];
                        if (selectedChannel == "Say") {
                            if (g_DebugEnabled)
                                LOG_INFO("server.loading", "[Ollama Chat] Bot Random Chatter Say: {}", response);
                            botAI->Say(response);
                        } else if (selectedChannel == "General") {
                            if (g_DebugEnabled)
                                LOG_INFO("server.loading", "[Ollama Chat] Bot Random Chatter General: {}", response);
                            botAI->SayToChannel(response, ChatChannelId::GENERAL);
                        }
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("server.loading", "[Ollama Chat] Exception in random chatter thread: {}", e.what());
                } catch (...) {
                    LOG_ERROR("server.loading", "[Ollama Chat] Unknown exception in random chatter thread");
                }
            }).detach();


            nextRandomChatTime[guid] = now + urand(g_MinRandomInterval, g_MaxRandomInterval);
        }
    }
}
