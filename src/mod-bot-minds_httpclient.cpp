#include "mod-bot-minds_httpclient.h"
#include "mod-bot-minds_config.h"

#include <httplib.h>

#include "Log.h"

BotMindsHttpClient::BotMindsHttpClient()
    : m_timeout(120)
{
}

BotMindsHttpClient::~BotMindsHttpClient()
{
}

std::string BotMindsHttpClient::PostSecure(const std::string& host,
                                           const std::string& path,
                                           const std::string& jsonData,
                                           const std::vector<std::pair<std::string, std::string>>& headers)
{
    try
    {
#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
        if (g_DebugEnabled)
            LOG_INFO("server.loading", "[BotMinds] HTTPS request to {}{}", host, path);

        httplib::SSLClient sslClient(host, 443);
        sslClient.enable_server_certificate_verification(true);
        sslClient.set_connection_timeout(m_timeout);
        sslClient.set_read_timeout(m_timeout);
        sslClient.set_write_timeout(m_timeout);

        httplib::Headers httpHeaders;
        for (const auto& header : headers)
            httpHeaders.emplace(header.first, header.second);

        httplib::Result response = sslClient.Post(path, httpHeaders, jsonData, "application/json");

        if (!response)
        {
            LOG_ERROR("server.loading", "[BotMinds] No response from {}{}", host, path);
            return "";
        }

        if (response->status != 200)
        {
            LOG_ERROR("server.loading", "[BotMinds] {}{} returned status {}", host, path, response->status);
            if (g_DebugEnabled)
                LOG_INFO("server.loading", "[BotMinds] Response body: {}", response->body);
            return "";
        }

        if (g_DebugEnabled)
            LOG_INFO("server.loading", "[BotMinds] Response received, {} bytes.", response->body.length());

        return response->body;
#else
        LOG_ERROR("server.loading", "[BotMinds] HTTPS needed but this build has no OpenSSL support.");
        return "";
#endif
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("server.loading", "[BotMinds] HTTPS client exception: {}", e.what());
        return "";
    }
}

void BotMindsHttpClient::SetTimeout(int seconds)
{
    m_timeout = seconds;
}
