#ifndef MOD_OLLAMA_CHAT_HANDLER_H
#define MOD_OLLAMA_CHAT_HANDLER_H

#include "ScriptMgr.h"
#include <string>

enum ChatChannelSourceLocal
{
    SRC_UNDEFINED_LOCAL  = 0,
    SRC_SAY_LOCAL        = 1,
    SRC_PARTY_LOCAL      = 2,
    SRC_RAID_LOCAL       = 3,
    SRC_GUILD_LOCAL      = 4,
    SRC_OFFICER_LOCAL    = 5,
    SRC_YELL_LOCAL       = 6,
    SRC_WHISPER_LOCAL    = 7,
    SRC_GENERAL_LOCAL    = 17
};

extern const char* ChatChannelSourceLocalStr[];

std::string rtrim(const std::string& s);
ChatChannelSourceLocal GetChannelSourceLocal(uint32_t type);

void SaveBotConversationHistoryToDB();

class PlayerBotChatHandler : public PlayerScript
{
public:
    PlayerBotChatHandler() : PlayerScript("PlayerBotChatHandler", {
        PLAYERHOOK_ON_CHAT,
        PLAYERHOOK_ON_CHAT_WITH_GROUP,
        PLAYERHOOK_ON_CHAT_WITH_GUILD,
        PLAYERHOOK_ON_CHAT_WITH_CHANNEL,
        PLAYERHOOK_ON_CHAT_WITH_RECEIVER,
        PLAYERHOOK_CAN_PLAYER_USE_PRIVATE_CHAT
    }) {}
    bool OnPlayerCanUseChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Player* receiver) override;
    void OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg) override;
    void OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Group* group) override;
    void OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Guild* guild) override;
    void OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Channel* channel) override;
    void OnPlayerChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, Player* receiver) override;

private:
    void ProcessChat(Player* player, uint32_t type, uint32_t lang, std::string& msg, ChatChannelSourceLocal sourceLocal, Channel* channel = nullptr, Player* receiver = nullptr);
};

#endif // MOD_OLLAMA_CHAT_HANDLER_H
