#pragma once
#include <string>
#include <vector>

struct DnsResult {
    std::string hostname;
    std::vector<std::string> a_records;    // IPv4
    std::vector<std::string> aaaa_records; // IPv6
    bool resolved = false;
    std::string error;
};

// Extract bare hostname from a URL, domain, or host:port string.
// e.g. "https://api.deepseek.com/v1" → "api.deepseek.com"
//      "192.168.1.1:8765"            → "192.168.1.1"
//      "myserver.local"              → "myserver.local"
std::string extract_hostname(const std::string& url);

// Resolve A and AAAA records for a hostname using getaddrinfo.
DnsResult resolve_dns(const std::string& hostname);

// Convenience: resolve the hostname embedded in a URL string.
DnsResult resolve_url(const std::string& url);

// Print A/AAAA records in the rp status format.
void print_dns(const DnsResult&, int indent = 4);

// Try to open a TCP connection to host:port within timeout_sec.
bool check_tcp(const std::string& host, int port, int timeout_sec = 5);

// Fetch this machine's public IPv4 and IPv6 via ifconfig.me / api6.ipify.org.
std::string public_ipv4();
std::string public_ipv6();
