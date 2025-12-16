#include "HttpClient.hpp"
#include <sstream>
#include <cstring>

namespace Omega {

HttpClient::HttpClient() {
#if defined(_WIN32)
    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif
}

HttpClient::~HttpClient() {
#if defined(_WIN32)
    WSACleanup();
#endif
}

HttpClient::ParsedUrl HttpClient::parseUrl(const std::string& url) {
    ParsedUrl p;
    
    size_t pos = 0;
    if (url.substr(0, 8) == "https://") {
        p.ssl = true;
        p.port = 443;
        pos = 8;
    } else if (url.substr(0, 7) == "http://") {
        p.ssl = false;
        p.port = 80;
        pos = 7;
    }
    
    size_t pathStart = url.find('/', pos);
    if (pathStart == std::string::npos) {
        p.host = url.substr(pos);
        p.path = "/";
    } else {
        p.host = url.substr(pos, pathStart - pos);
        p.path = url.substr(pathStart);
    }
    
    // Check for port in host
    size_t colonPos = p.host.find(':');
    if (colonPos != std::string::npos) {
        p.port = std::stoi(p.host.substr(colonPos + 1));
        p.host = p.host.substr(0, colonPos);
    }
    
    return p;
}

std::string HttpClient::sendRequest(const std::string& method,
                                    const ParsedUrl& u,
                                    const std::string& apiKey,
                                    const std::string& body)
{
    // Note: This is a simplified HTTP client without SSL
    // For production, use libcurl or similar
    
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return "";
    
    struct hostent* host = gethostbyname(u.host.c_str());
    if (!host) {
#if defined(_WIN32)
        closesocket(sock);
#else
        close(sock);
#endif
        return "";
    }
    
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(u.port);
    memcpy(&addr.sin_addr, host->h_addr, host->h_length);
    
    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
#if defined(_WIN32)
        closesocket(sock);
#else
        close(sock);
#endif
        return "";
    }
    
    std::ostringstream req;
    req << method << " " << u.path << " HTTP/1.1\r\n";
    req << "Host: " << u.host << "\r\n";
    req << "Connection: close\r\n";
    if (!apiKey.empty()) {
        req << "X-MBX-APIKEY: " << apiKey << "\r\n";
    }
    if (!body.empty()) {
        req << "Content-Type: application/x-www-form-urlencoded\r\n";
        req << "Content-Length: " << body.size() << "\r\n";
    }
    req << "\r\n";
    if (!body.empty()) {
        req << body;
    }
    
    std::string request = req.str();
    send(sock, request.c_str(), (int)request.size(), 0);
    
    std::string response;
    char buf[4096];
    int n;
    while ((n = recv(sock, buf, sizeof(buf), 0)) > 0) {
        response.append(buf, n);
    }
    
#if defined(_WIN32)
    closesocket(sock);
#else
    close(sock);
#endif
    
    // Extract body from HTTP response
    size_t bodyStart = response.find("\r\n\r\n");
    if (bodyStart != std::string::npos) {
        return response.substr(bodyStart + 4);
    }
    
    return response;
}

void HttpClient::get(const std::string& url,
                     const std::string& apiKey,
                     ResponseCallback cb)
{
    ParsedUrl u = parseUrl(url);
    std::string response = sendRequest("GET", u, apiKey, "");
    if (cb) cb(response);
}

void HttpClient::post(const std::string& url,
                      const std::string& apiKey,
                      const std::string& body,
                      ResponseCallback cb)
{
    ParsedUrl u = parseUrl(url);
    std::string response = sendRequest("POST", u, apiKey, body);
    if (cb) cb(response);
}

} // namespace Omega
