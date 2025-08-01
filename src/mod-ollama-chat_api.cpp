#include "mod-ollama-chat_api.h"
#include "mod-ollama-chat_config.h"
#include "mod-ollama-chat_httpclient.h"
#include "Log.h"
#include <sstream>
#include <nlohmann/json.hpp>
#include <fmt/core.h>
#include <thread>
#include <mutex>
#include <queue>
#include <future>

std::string ExtractTextBetweenDoubleQuotes(const std::string& response)
{
    size_t first = response.find('"');
    size_t second = response.find('"', first + 1);
    if (first != std::string::npos && second != std::string::npos) {
        return response.substr(first + 1, second - first - 1);
    }
    return response;
}

// Function to perform the API call.
std::string QueryOllamaAPI(const std::string& prompt)
{
    // Initialize our custom HTTP client
    static OllamaHttpClient httpClient;
    
    if (!httpClient.IsAvailable())
    {
        if(g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] HTTP client not available.");
        }
        return "Hmm... I'm lost in thought.";
    }

    std::string url   = g_OllamaUrl;
    std::string model = g_OllamaModel;

    nlohmann::json requestData = {
        {"model",  model},
        {"prompt", prompt}
    };
    // Only include if set (do not send defaults if user did not set them)
    if (g_OllamaNumPredict > 0)          requestData["num_predict"]     = g_OllamaNumPredict;
    if (g_OllamaTemperature != 0.8f)     requestData["temperature"]     = g_OllamaTemperature;
    if (g_OllamaTopP != 0.95f)           requestData["top_p"]           = g_OllamaTopP;
    if (g_OllamaRepeatPenalty != 1.1f)   requestData["repeat_penalty"]  = g_OllamaRepeatPenalty;
    if (g_OllamaNumCtx > 0)              requestData["num_ctx"]         = g_OllamaNumCtx;
    if (g_OllamaNumThreads > 0)          requestData["num_thread"]     = g_OllamaNumThreads;
    if (!g_OllamaStop.empty()) {
        // If comma-separated, convert to array
        std::vector<std::string> stopSeqs;
        std::stringstream ss(g_OllamaStop);
        std::string item;
        while (std::getline(ss, item, ',')) {
            // trim whitespace
            size_t start = item.find_first_not_of(" \t");
            size_t end = item.find_last_not_of(" \t");
            if (start != std::string::npos && end != std::string::npos)
                stopSeqs.push_back(item.substr(start, end - start + 1));
        }
        if (!stopSeqs.empty())
            requestData["stop"] = stopSeqs;
    }
    if (!g_OllamaSystemPrompt.empty())   requestData["system"]          = g_OllamaSystemPrompt;
    if (!g_OllamaSeed.empty())           requestData["seed"]            = g_OllamaSeed;

    if (g_ThinkModeEnableForModule)
    {
        if(g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] LLM set to Think mode.");
        }
        requestData["think"] = true;
        requestData["hidethinking"] = true;
    }

    std::string requestDataStr = requestData.dump();

    // Make HTTP POST request using our custom client
    std::string responseBuffer = httpClient.Post(url, requestDataStr);

    if (responseBuffer.empty())
    {
        if(g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] Failed to reach Ollama AI.");
        }
        return "Failed to reach Ollama AI.";
    }

    std::stringstream ss(responseBuffer);
    std::string line;
    std::ostringstream extractedResponse;

    try
    {
        while (std::getline(ss, line))
        {
            if (line.empty() || std::all_of(line.begin(), line.end(), isspace))
                continue;

            nlohmann::json jsonResponse = nlohmann::json::parse(line);

            if (jsonResponse.contains("response") && !jsonResponse["response"].get<std::string>().empty())
            {
                extractedResponse << jsonResponse["response"].get<std::string>();
            }
        }
    }
    catch (const std::exception& e)
    {
        if(g_DebugEnabled)
        {
            LOG_INFO("server.loading",
                    "[Ollama Chat] JSON Parsing Error: {}",
                    e.what());
        }
        return "Error processing response.";
    }

    std::string botReply = extractedResponse.str();

    botReply = ExtractTextBetweenDoubleQuotes(botReply);

    if (botReply.empty())
    {
        if(g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] No valid response extracted.");
        }
        return "I'm having trouble understanding.";
    }

    if(g_DebugEnabled)
    {
        LOG_INFO("server.loading", "[Ollama Chat] Parsed bot response: {}", botReply);

        if (g_ThinkModeEnableForModule)
        {
            if(g_DebugEnabled)
            {
                LOG_INFO("server.loading", "[Ollama Chat] Bot used think.");
            }
        }
    }

    return botReply;
}

QueryManager g_queryManager;

// Interface function to submit a query.
std::future<std::string> SubmitQuery(const std::string& prompt)
{
    return g_queryManager.submitQuery(prompt);
}