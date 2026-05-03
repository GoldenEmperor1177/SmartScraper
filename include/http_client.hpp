#pragma once
#include <string>
#include <map>

struct HttpResponse {
    int         status_code = 0;
    std::string body;
    std::string error;

    bool ok() const { return status_code >= 200 && status_code < 300; }
};

class HttpClient {
public:
    std::string user_agent   = "SmartScraper/1.0 (research)";
    int         timeout_sec  = 30;
    bool        follow_redirects = true;

    HttpResponse get(
        const std::string& url,
        const std::map<std::string, std::string>& headers = {}
    );

    HttpResponse post(
        const std::string& url,
        const std::string& body,
        const std::string& content_type = "application/json",
        const std::map<std::string, std::string>& headers = {}
    );

    // URL-encode a string (percent-encoding)
    static std::string url_encode(const std::string& s);

    // URL-decode a percent-encoded string
    static std::string url_decode(const std::string& s);
};
