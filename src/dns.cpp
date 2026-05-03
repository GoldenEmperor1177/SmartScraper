#include "dns.hpp"
#include "http_client.hpp"
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/select.h>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <iomanip>

std::string extract_hostname(const std::string& url) {
    std::string s = url;

    // Strip protocol
    auto proto = s.find("://");
    if (proto != std::string::npos) s = s.substr(proto + 3);

    // Strip path
    auto slash = s.find('/');
    if (slash != std::string::npos) s = s.substr(0, slash);

    // Strip port — but only if not an IPv6 literal like [::1]:8080
    if (!s.empty() && s.front() == '[') {
        // IPv6 literal: [addr]:port
        auto close = s.find(']');
        if (close != std::string::npos) s = s.substr(1, close - 1);
    } else {
        auto colon = s.find(':');
        if (colon != std::string::npos) s = s.substr(0, colon);
    }

    return s;
}

DnsResult resolve_dns(const std::string& hostname) {
    DnsResult result;
    result.hostname = hostname;

    if (hostname.empty()) {
        result.error = "empty hostname";
        return result;
    }

    struct addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_CANONNAME;

    struct addrinfo* info = nullptr;
    int rc = getaddrinfo(hostname.c_str(), nullptr, &hints, &info);
    if (rc != 0) {
        result.error = gai_strerror(rc);
        return result;
    }

    result.resolved = true;
    char buf[INET6_ADDRSTRLEN];

    auto dedup_push = [](std::vector<std::string>& vec, const std::string& ip) {
        for (auto& x : vec) if (x == ip) return;
        vec.push_back(ip);
    };

    for (auto* p = info; p != nullptr; p = p->ai_next) {
        std::memset(buf, 0, sizeof(buf));
        if (p->ai_family == AF_INET) {
            auto* addr = reinterpret_cast<struct sockaddr_in*>(p->ai_addr);
            inet_ntop(AF_INET, &addr->sin_addr, buf, sizeof(buf));
            dedup_push(result.a_records, buf);
        } else if (p->ai_family == AF_INET6) {
            auto* addr = reinterpret_cast<struct sockaddr_in6*>(p->ai_addr);
            inet_ntop(AF_INET6, &addr->sin6_addr, buf, sizeof(buf));
            dedup_push(result.aaaa_records, buf);
        }
    }

    freeaddrinfo(info);
    return result;
}

DnsResult resolve_url(const std::string& url) {
    return resolve_dns(extract_hostname(url));
}

bool check_tcp(const std::string& host, int port, int timeout_sec) {
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    std::string port_str = std::to_string(port);
    if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res)
        return false;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return false; }

    // Set non-blocking so we can timeout
    fcntl(fd, F_SETFL, O_NONBLOCK);
    connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);

    fd_set wset;
    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    struct timeval tv{ timeout_sec, 0 };
    int rc = select(fd + 1, nullptr, &wset, nullptr, &tv);

    bool ok = false;
    if (rc == 1) {
        int err = 0;
        socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        ok = (err == 0);
    }
    close(fd);
    return ok;
}

std::string public_ipv4() {
    HttpClient http;
    http.timeout_sec = 5;
    auto r = http.get("https://api4.ipify.org");
    if (r.ok() && !r.body.empty()) {
        // trim whitespace
        auto s = r.body;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
            s.pop_back();
        return s;
    }
    // fallback
    r = http.get("https://ifconfig.me/ip");
    if (r.ok()) {
        auto s = r.body;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
            s.pop_back();
        return s;
    }
    return "";
}

std::string public_ipv6() {
    HttpClient http;
    http.timeout_sec = 5;
    auto r = http.get("https://api6.ipify.org");
    if (r.ok() && !r.body.empty()) {
        auto s = r.body;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' '))
            s.pop_back();
        // sanity check — must contain a colon to be IPv6
        if (s.find(':') != std::string::npos) return s;
    }
    return "";
}

void print_dns(const DnsResult& r, int indent) {
    std::string pad(indent, ' ');
    if (!r.resolved) {
        std::cout << pad << "DNS  ✗  " << r.hostname
                  << "  (" << (r.error.empty() ? "unresolved" : r.error) << ")\n";
        return;
    }
    for (auto& ip : r.a_records)
        std::cout << pad << "A    " << std::left << std::setw(30) << r.hostname << "  " << ip << "\n";
    for (auto& ip : r.aaaa_records)
        std::cout << pad << "AAAA " << std::left << std::setw(30) << r.hostname << "  " << ip << "\n";
}
