#include "mod-ollama-chat_httpclient.h"

// Include cpp-httplib for HTTP functionality
#include <httplib.h>

#include "Log.h"
#include <sstream>
#include <regex>

OllamaHttpClient::OllamaHttpClient()
    : m_timeout(30), m_available(true)
{
    // Default 30 second timeout
}

OllamaHttpClient::~OllamaHttpClient()
{
}

std::string OllamaHttpClient::Post(const std::string& url, const std::string& jsonData)
{
    try 
    {
        // Parse URL to extract host and path
        std::regex urlRegex(R"(^https?://([^:/]+)(?::(\d+))?(/.*)?$)");
        std::smatch match;
        
        if (!std::regex_match(url, match, urlRegex))
        {
            LOG_INFO("server.loading", "[Ollama Chat] Invalid URL format: {}", url);
            return "";
        }
        
        std::string host = match[1].str();
        int port = 80;
        if (match[2].matched)
        {
            port = std::stoi(match[2].str());
        }
        else if (url.substr(0, 5) == "https")
        {
            port = 443;
        }
        
        std::string path = match[3].matched ? match[3].str() : "/";
        
        // Create HTTP client
        httplib::Client client(host, port);
        client.set_connection_timeout(m_timeout);
        client.set_read_timeout(m_timeout);
        client.set_write_timeout(m_timeout);
        
        // Set headers
        httplib::Headers headers = {
            {"Content-Type", "application/json"}
        };
        
        // Make POST request
        auto response = client.Post(path, headers, jsonData, "application/json");
        
        if (!response)
        {
            LOG_INFO("server.loading", "[Ollama Chat] HTTP request failed - no response");
            return "";
        }
        
        if (response->status != 200)
        {
            LOG_INFO("server.loading", "[Ollama Chat] HTTP request failed with status: {}", response->status);
            return "";
        }
        
        return response->body;
    }
    catch (const std::exception& e)
    {
        LOG_INFO("server.loading", "[Ollama Chat] HTTP client exception: {}", e.what());
        return "";
    }
}

void OllamaHttpClient::SetTimeout(int seconds)
{
    m_timeout = seconds;
}

bool OllamaHttpClient::IsAvailable() const
{
    return m_available;
}