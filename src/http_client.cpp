#include "http_client.hpp"
#include <curl/curl.h>
#include <sstream>
#include <iomanip>
#include <stdexcept>

// libcurl write callback
static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* buf = static_cast<std::string*>(userdata);
    buf->append(ptr, size * nmemb);
    return size * nmemb;
}

static HttpResponse perform(
    const std::string& url,
    const std::string& body,
    const std::string& content_type,
    const std::map<std::string, std::string>& headers,
    bool is_post,
    bool follow_redirects,
    const std::string& user_agent,
    int timeout_sec
) {
    HttpResponse resp;
    CURL* curl = curl_easy_init();
    if (!curl) { resp.error = "curl_easy_init failed"; return resp; }

    struct curl_slist* hlist = nullptr;
    for (auto& [k, v] : headers)
        hlist = curl_slist_append(hlist, (k + ": " + v).c_str());
    if (is_post && !content_type.empty())
        hlist = curl_slist_append(hlist, ("Content-Type: " + content_type).c_str());

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &resp.body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT,      user_agent.c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        (long)timeout_sec);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, follow_redirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hlist);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // auto decompress

    if (is_post) {
        curl_easy_setopt(curl, CURLOPT_POST,       1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body.size());
    }

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
        resp.status_code = (int)code;
    } else {
        resp.error = curl_easy_strerror(rc);
    }

    curl_slist_free_all(hlist);
    curl_easy_cleanup(curl);
    return resp;
}

HttpResponse HttpClient::get(const std::string& url,
                              const std::map<std::string, std::string>& headers) {
    return perform(url, "", "", headers, false, follow_redirects, user_agent, timeout_sec);
}

HttpResponse HttpClient::post(const std::string& url, const std::string& body,
                               const std::string& content_type,
                               const std::map<std::string, std::string>& headers) {
    return perform(url, body, content_type, headers, true, follow_redirects, user_agent, timeout_sec);
}

std::string HttpClient::url_encode(const std::string& s) {
    CURL* curl = curl_easy_init();
    if (!curl) return s;
    char* enc = curl_easy_escape(curl, s.c_str(), (int)s.size());
    std::string result(enc ? enc : s);
    curl_free(enc);
    curl_easy_cleanup(curl);
    return result;
}

std::string HttpClient::url_decode(const std::string& s) {
    CURL* curl = curl_easy_init();
    if (!curl) return s;
    int out_len = 0;
    char* dec = curl_easy_unescape(curl, s.c_str(), (int)s.size(), &out_len);
    std::string result(dec ? dec : s.c_str(), dec ? out_len : s.size());
    curl_free(dec);
    curl_easy_cleanup(curl);
    return result;
}
