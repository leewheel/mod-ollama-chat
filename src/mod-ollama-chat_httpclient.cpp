#include "mod-ollama-chat_httpclient.h"
#include "mod-ollama-chat_config.h"

// Include cpp-httplib for HTTP functionality
#include <httplib.h>

#include "Log.h"
#include <sstream>
#include <regex>
#include <memory>

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
        
        // Create HTTP client (with SSL support for HTTPS)
        std::unique_ptr<httplib::Client> client;
        if (protocol == "https") {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
            auto sslClient = std::make_unique<httplib::SSLClient>(host, port);
            // Disable SSL verification for ngrok and self-signed certificates
            sslClient->enable_server_certificate_verification(false);
            client = std::move(sslClient);
            if(g_DebugEnabled) {
                LOG_INFO("server.loading", "[Ollama Chat] Using SSL client for HTTPS connection");
            }
#else
            LOG_ERROR("server.loading", "[Ollama Chat] HTTPS requested but SSL support not available.");
            LOG_ERROR("server.loading", "[Ollama Chat] Please rebuild with OpenSSL support enabled.");
            LOG_ERROR("server.loading", "[Ollama Chat] See CMake output for OpenSSL installation instructions.");
            return "";
#endif
        } else {
            client = std::make_unique<httplib::Client>(host, port);
            if(g_DebugEnabled) {
                LOG_INFO("server.loading", "[Ollama Chat] Using standard HTTP client");
            }
        }
        
        client->set_connection_timeout(m_timeout);
        client->set_read_timeout(m_timeout);
        client->set_write_timeout(m_timeout);
        
        // Set headers (with ngrok-specific headers)
        httplib::Headers headers = {
            {"Content-Type", "application/json"},
            {"User-Agent", "AzerothCore-OllamaChat/1.0"},
            {"Accept", "application/json"}
        };
        
        // Add ngrok bypass header if this is an ngrok URL
        if (host.find("ngrok") != std::string::npos || host.find("ngrok-free.app") != std::string::npos) {
            headers.emplace("ngrok-skip-browser-warning", "true");
            if(g_DebugEnabled) {
                LOG_INFO("server.loading", "[Ollama Chat] Added ngrok bypass header");
            }
        }
        
        // Make POST request
        auto response = client->Post(path, headers, jsonData, "application/json");
        
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