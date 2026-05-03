#include "server.hpp"
#include "config.hpp"
#include "reportmaker/agent.hpp"
#include "pdf_writer.hpp"
#include <nlohmann/json.hpp>

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <map>
#include <vector>
#include <algorithm>
#include <iostream>

using json = nlohmann::json;

static const char* PID_FILE = "/tmp/smartscraper_api.pid";

static volatile sig_atomic_t g_shutdown = 0;

static void sigterm_handler(int) { g_shutdown = 1; }

// ── PID helpers ───────────────────────────────────────────────────────────────

static void write_pid(pid_t p) {
    FILE* f = fopen(PID_FILE, "w");
    if (f) { fprintf(f, "%d\n", p); fclose(f); }
}

// ── Socket helpers ────────────────────────────────────────────────────────────

static int bind_server_socket(const std::string& host, int port) {
    // Try dual-stack IPv6 first
    int fd = socket(AF_INET6, SOCK_STREAM, 0);
    bool using_ipv6 = (fd >= 0);
    if (!using_ipv6) fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));

    if (using_ipv6) {
        int zero = 0;
        setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port   = htons(port);
        if (host.empty() || host == "0.0.0.0" || host == "::") {
            addr.sin6_addr = in6addr_any;
        } else {
            inet_pton(AF_INET6, host.c_str(), &addr.sin6_addr);
        }
        if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    } else {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(port);
        addr.sin_addr.s_addr = (host.empty() || host == "0.0.0.0")
                                ? INADDR_ANY : inet_addr(host.c_str());
        if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) { close(fd); return -1; }
    }

    if (listen(fd, 8) < 0) { close(fd); return -1; }
    return fd;
}

// ── HTTP primitives ───────────────────────────────────────────────────────────

struct Request {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
};

static bool recv_until(int fd, std::string& buf, const std::string& marker, size_t cap = 65536) {
    while (buf.find(marker) == std::string::npos) {
        char tmp[4096];
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, n);
        if (buf.size() > cap) return false;
    }
    return true;
}

static bool parse_request(int fd, Request& req) {
    std::string buf;
    if (!recv_until(fd, buf, "\r\n\r\n")) return false;

    size_t hdr_end = buf.find("\r\n\r\n");
    std::string hdr_block = buf.substr(0, hdr_end);
    std::string leftover  = buf.substr(hdr_end + 4);

    std::istringstream ss(hdr_block);
    std::string request_line;
    std::getline(ss, request_line);
    if (!request_line.empty() && request_line.back() == '\r') request_line.pop_back();

    std::istringstream rls(request_line);
    rls >> req.method >> req.path;

    std::string hline;
    while (std::getline(ss, hline)) {
        if (!hline.empty() && hline.back() == '\r') hline.pop_back();
        auto colon = hline.find(':');
        if (colon == std::string::npos) continue;
        std::string key = hline.substr(0, colon);
        std::string val = hline.substr(colon + 1);
        // lowercase key, trim val
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        size_t vs = val.find_first_not_of(' ');
        if (vs != std::string::npos) val = val.substr(vs);
        req.headers[key] = val;
    }

    // Read body if Content-Length present
    size_t body_len = 0;
    auto it = req.headers.find("content-length");
    if (it != req.headers.end()) {
        try { body_len = std::stoul(it->second); } catch (...) {}
    }
    if (body_len > 0) {
        req.body = leftover;
        while (req.body.size() < body_len) {
            char tmp[4096];
            size_t want = std::min(sizeof(tmp), body_len - req.body.size());
            ssize_t n = recv(fd, tmp, want, 0);
            if (n <= 0) break;
            req.body.append(tmp, n);
        }
    }
    return true;
}

static void send_all(int fd, const char* data, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n <= 0) break;
        data += n; len -= n;
    }
}

static void send_response(int fd, int status, const std::string& ctype,
                           const char* body, size_t body_len) {
    const char* phrase = status == 200 ? "OK"
                       : status == 400 ? "Bad Request"
                       : status == 401 ? "Unauthorized"
                       : status == 404 ? "Not Found"
                       : "Internal Server Error";
    std::ostringstream hdr;
    hdr << "HTTP/1.1 " << status << " " << phrase << "\r\n"
        << "Content-Type: " << ctype << "\r\n"
        << "Content-Length: " << body_len << "\r\n"
        << "Connection: close\r\n\r\n";
    std::string h = hdr.str();
    send_all(fd, h.c_str(), h.size());
    if (body_len > 0) send_all(fd, body, body_len);
}

static void send_json(int fd, int status, const json& j) {
    std::string s = j.dump();
    send_response(fd, status, "application/json", s.c_str(), s.size());
}

// ── Auth ──────────────────────────────────────────────────────────────────────

static bool authenticate(const Request& req, const Config& cfg) {
    auto it = req.headers.find("authorization");
    if (it == req.headers.end()) return false;
    const std::string& val = it->second;
    if (val.size() < 8 || val.substr(0, 7) != "Bearer ") return false;
    std::string token = val.substr(7);
    // trim trailing whitespace
    while (!token.empty() && (token.back() == ' ' || token.back() == '\r' || token.back() == '\n'))
        token.pop_back();
    for (auto& sk : cfg.server_keys)
        if (sk.key == token) return true;
    return false;
}

// ── AgentConfig from Config ───────────────────────────────────────────────────

static reportmaker::AgentConfig make_agent_cfg(const Config& cfg) {
    reportmaker::AgentConfig a;
    auto pair = active_main_pair(cfg);
    if (pair) {
        a.base_url = pair->url;
        a.api_key  = pair->key;
        a.model    = pair->model;
    }
    a.ss_min = cfg.limits.ss_min;
    a.ss_max = cfg.limits.ss_max;
    a.fc_min = cfg.limits.fc_min;
    a.fc_max = cfg.limits.fc_max;
    a.verbose = false;
    return a;
}

// ── Route handlers ────────────────────────────────────────────────────────────

static void handle_report(int fd, const Request& req) {
    json body;
    try { body = json::parse(req.body); }
    catch (...) {
        send_json(fd, 400, {{"error", "invalid JSON body"}});
        return;
    }
    if (!body.contains("query") || !body["query"].is_string()) {
        send_json(fd, 400, {{"error", "missing string field: query"}});
        return;
    }
    std::string query  = body["query"].get<std::string>();
    std::string format = body.value("format", "text");

    if (query.empty()) {
        send_json(fd, 400, {{"error", "query must not be empty"}});
        return;
    }

    Config cfg = load_config();
    auto agent = make_agent_cfg(cfg);
    if (agent.api_key.empty()) {
        send_json(fd, 500, {{"error", "no API key configured"}});
        return;
    }

    std::string report = reportmaker::run_report(query, agent);

    if (format == "pdf") {
        auto pdf_bytes = render_pdf(report);
        if (pdf_bytes.empty()) {
            send_json(fd, 500, {{"error", "PDF generation failed"}});
            return;
        }
        send_response(fd, 200, "application/pdf",
                      reinterpret_cast<const char*>(pdf_bytes.data()), pdf_bytes.size());
    } else {
        send_json(fd, 200, {{"report", report}});
    }
}

static void handle_request(int fd) {
    Request req;
    if (!parse_request(fd, req)) {
        send_json(fd, 400, {{"error", "bad request"}});
        return;
    }

    // Health — unauthenticated
    if (req.method == "GET" && req.path == "/health") {
        send_json(fd, 200, {{"status", "ok"}});
        return;
    }

    // All other routes require a server key
    Config cfg = load_config();
    if (cfg.server_keys.empty()) {
        send_json(fd, 401, {{"error", "no server keys configured — run: rp skey new"}});
        return;
    }
    if (!authenticate(req, cfg)) {
        send_json(fd, 401, {{"error", "Unauthorized"}});
        return;
    }

    if (req.method == "POST" && req.path == "/report") {
        handle_report(fd, req);
        return;
    }

    send_json(fd, 404, {{"error", "not found"}});
}

// ── Server loop (runs in daemon child) ────────────────────────────────────────

static void server_loop(int server_fd) {
    struct sigaction sa{};
    sa.sa_handler = sigterm_handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT,  &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    while (!g_shutdown) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(server_fd, &rfds);
        struct timeval tv{1, 0};
        int sel = select(server_fd + 1, &rfds, nullptr, nullptr, &tv);
        if (sel < 0) { if (errno == EINTR) continue; break; }
        if (sel == 0) continue;

        sockaddr_storage cli_addr{};
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = accept(server_fd, (sockaddr*)&cli_addr, &cli_len);
        if (client_fd < 0) continue;

        handle_request(client_fd);
        close(client_fd);
    }

    close(server_fd);
    std::remove(PID_FILE);
}

// ── Public entry point ────────────────────────────────────────────────────────

void run_server_daemon() {
    Config cfg = load_config();

    int server_fd = bind_server_socket(cfg.server_host, cfg.server_port);
    if (server_fd < 0) {
        std::cerr << "[rp] Failed to bind " << cfg.server_host << ":" << cfg.server_port
                  << " — " << strerror(errno) << "\n";
        return;
    }

    // Double-fork daemon
    pid_t pid1 = fork();
    if (pid1 < 0) { std::cerr << "[rp] fork failed\n"; close(server_fd); return; }

    if (pid1 > 0) {
        // Parent: print and return
        std::cout << "[rp] Server started (pid " << pid1
                  << ") on " << cfg.server_host << ":" << cfg.server_port << "\n";
        close(server_fd);
        return;
    }

    // First child: become session leader
    setsid();

    pid_t pid2 = fork();
    if (pid2 < 0) _exit(1);
    if (pid2 > 0) _exit(0);  // First child exits, orphaning the grandchild

    // Grandchild: full daemon
    if (chdir("/") != 0) {}  // daemon cwd reset, ignore error
    umask(0);
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > 2) close(devnull);
    }

    write_pid(getpid());
    server_loop(server_fd);
    _exit(0);
}
