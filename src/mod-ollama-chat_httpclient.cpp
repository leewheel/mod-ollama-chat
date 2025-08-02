#include "mod-ollama-chat_httpclient.h"
#include "mod-ollama-chat_config.h"

// Include cpp-httplib for HTTP functionality
#include <httplib.h>

#include "Log.h"
#include <sstream>
#include <regex>

OllamaHttpClient::OllamaHttpClient()
    : m_timeout(120), m_available(true)
{
    // Default 120 second timeout
}

OllamaHttpClient::~OllamaHttpClient()
{
}

std::string OllamaHttpClient::Post(const std::string& url, const std::string& jsonData)
{
    try 
    {
        // Parse URL to extract host and path
        std::regex urlRegex(R"(^(https?)://([^:/]+)(?::(\d+))?(/.*)?$)");
        std::smatch match;
        
        if (!std::regex_match(url, match, urlRegex))
        {
            LOG_INFO("server.loading", "[Ollama Chat] Invalid URL format: {}", url);
            return "";
        }
        
        std::string protocol = match[1].str();
        std::string host = match[2].str();
        int port = 11434;  // Default Ollama port
        if (match[3].matched)
        {
            port = std::stoi(match[3].str());
        }
        else if (protocol == "https")
        {
            port = 443;  // HTTPS default
        }
        else if (protocol == "http")
        {
            port = 11434;  // Ollama default port for HTTP
        }
        
        std::string path = match[4].matched ? match[4].str() : "/";
        
        if(g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] HTTP Request - Protocol: {}, Host: {}, Port: {}, Path: {}", 
                protocol, host, port, path);
        }
        
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
            LOG_ERROR("server.loading", "[Ollama Chat] HTTP request failed - no response from {}:{}{}", host, port, path);
            return "";
        }
        
        if (response->status != 200)
        {
            LOG_ERROR("server.loading", "[Ollama Chat] HTTP request failed with status: {} for {}:{}{}", 
                response->status, host, port, path);
            if(g_DebugEnabled)
            {
                LOG_INFO("server.loading", "[Ollama Chat] Response body: {}", response->body);
            }
            return "";
        }
        
        if(g_DebugEnabled)
        {
            LOG_INFO("server.loading", "[Ollama Chat] HTTP request successful, response length: {}", response->body.length());
        }
        
        return response->body;
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("server.loading", "[Ollama Chat] HTTP client exception: {}", e.what());
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