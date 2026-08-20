#ifndef MOD_BOT_MINDS_HTTPCLIENT_H
#define MOD_BOT_MINDS_HTTPCLIENT_H

#include <string>
#include <vector>
#include <utility>

// --------------------------------------------
// Minimal HTTPS POST client over cpp-httplib, used to reach the LLM provider.
// --------------------------------------------
class BotMindsHttpClient
{
public:
    BotMindsHttpClient();
    ~BotMindsHttpClient();

    // POST JSON to https://host/path with the given headers. Returns the response
    // body, or "" on any failure or non-200 status.
    std::string PostSecure(const std::string& host,
                           const std::string& path,
                           const std::string& jsonData,
                           const std::vector<std::pair<std::string, std::string>>& headers);

    void SetTimeout(int seconds);

private:
    int m_timeout;
};

#endif // MOD_BOT_MINDS_HTTPCLIENT_H
