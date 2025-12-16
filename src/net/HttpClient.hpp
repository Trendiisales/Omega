#pragma once
#include <string>
#include <functional>
#include <thread>
#include <atomic>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#endif

namespace Omega {

class HttpClient {
public:
    using ResponseCallback = std::function<void(const std::string&)>;

    HttpClient();
    ~HttpClient();

    void get(const std::string& url,
             const std::string& apiKey,
             ResponseCallback cb);

    void post(const std::string& url,
              const std::string& apiKey,
              const std::string& body,
              ResponseCallback cb);

private:
    struct ParsedUrl {
        std::string host;
        std::string path;
        int port = 443;
        bool ssl = true;
    };

    ParsedUrl parseUrl(const std::string& url);
    std::string sendRequest(const std::string& method,
                            const ParsedUrl& u,
                            const std::string& apiKey,
                            const std::string& body);
};

} // namespace Omega
