#define APP_VERSION "1.0.6"

#include "config.hpp"
#include "dns.hpp"
#include "http_client.hpp"
#include "reportmaker/agent.hpp"
#include "server.hpp"
#include "pdf_writer.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <iomanip>
#include <chrono>
#include <sys/types.h>
#include <unistd.h>
#include <cstdlib>
#include <signal.h>

namespace fs = std::filesystem;
using json = nlohmann::json;

// ── PID helpers ───────────────────────────────────────────────────────────────

static const char* PID_FILE = "/tmp/smartscraper_api.pid";

static int read_pid() { std::ifstream f(PID_FILE); int p = 0; f >> p; return p; }

// Verify the PID is alive AND belongs to our server process, not a stale entry.
static bool is_our_server(int pid) {
    if (pid <= 0 || kill(pid, 0) != 0) return false;
    // Read /proc/<pid>/comm to check the process name
    std::ifstream comm("/proc/" + std::to_string(pid) + "/comm");
    std::string name;
    if (comm >> name) {
        // Accept rp, smartscraper, uvicorn (Python fallback)
        if (name == "rp" || name == "smartscraper" || name == "uvicorn") return true;
    }
    // Stale PID — clean it up silently
    std::remove(PID_FILE);
    return false;
}

// ── Display helpers ───────────────────────────────────────────────────────────

static std::string mask_key(const std::string& key) {
    if (key.empty()) return "(not set)";
    if (key.size() <= 8) return "****";
    return "****" + key.substr(key.size() - 6);
}

static std::string tick(bool ok) { return ok ? "✓" : "✗"; }

static void show_url_dns(const std::string& url, const std::string& label) {
    if (url.empty()) return;
    auto dns = resolve_url(url);
    std::cout << "    " << label << ": " << url << "\n";
    if (dns.resolved) {
        for (auto& ip : dns.a_records)    std::cout << "      A    " << ip << "\n";
        for (auto& ip : dns.aaaa_records) std::cout << "      AAAA " << ip << "\n";
    } else {
        std::cout << "      DNS  ✗  " << (dns.error.empty() ? "unresolved" : dns.error) << "\n";
    }
}

static void print_pair(const ApiPair& p, bool active_main, bool active_search) {
    std::string flags;
    if (active_main)   flags += " [main LLM]";
    if (active_search) flags += " [search LLM]";
    std::cout << "  [" << p.id << "]  "
              << std::left << std::setw(40) << p.url
              << "  " << std::setw(18) << (p.model.empty() ? "deepseek-chat" : p.model)
              << "  " << mask_key(p.key)
              << flags << "\n";
}

// ── rp api ────────────────────────────────────────────────────────────────────

static void cmd_api(int argc, char** argv) {
    // argv: rp api <sub> [args...]
    std::string sub = (argc >= 3) ? argv[2] : "list";

    if (sub == "list") {
        auto cfg = load_config();
        if (cfg.api_pairs.empty()) {
            std::cout << "\n  No API pairs added yet.\n";
            std::cout << "  Add one:  rp api add <url> <key> [model]\n\n";
            return;
        }
        std::cout << "\n  API Pairs\n";
        std::cout << "  ─────────────────────────────────────────────────────\n";
        for (auto& p : cfg.api_pairs)
            print_pair(p, p.id == cfg.main_llm_pair,
                          p.id == cfg.search_llm_pair);
        std::cout << "\n  Assign to functions:  rp api assign\n\n";

    } else if (sub == "add") {
        if (argc < 5) {
            std::cout << "Usage: rp api add <url> <key> [model]\n";
            return;
        }
        std::string url   = argv[3];
        std::string key   = argv[4];
        std::string model = (argc >= 6) ? argv[5] : "";
        int id = add_api_pair(url, key, model);
        std::cout << "[rp] API pair added as [" << id << "]\n";
        std::cout << "     URL   : " << url << "\n";
        std::cout << "     Model : " << (model.empty() ? "deepseek-chat (default)" : model) << "\n";
        std::cout << "     Key   : " << mask_key(key) << "\n";

        // Show DNS for the URL
        auto dns = resolve_url(url);
        if (dns.resolved) {
            for (auto& ip : dns.a_records)    std::cout << "     A    " << ip << "\n";
            for (auto& ip : dns.aaaa_records) std::cout << "     AAAA " << ip << "\n";
        }

        auto cfg = load_config();
        if (cfg.main_llm_pair == id)
            std::cout << "     Auto-assigned as Main LLM (first pair).\n";
        std::cout << "     Run 'rp api assign' to assign to a function.\n";

    } else if (sub == "rm") {
        if (argc < 4) { std::cout << "Usage: rp api rm <number>\n"; return; }
        int id = std::stoi(argv[3]);
        if (remove_api_pair(id))
            std::cout << "[rp] API pair [" << id << "] removed.\n";
        else
            std::cerr << "[rp] Pair [" << id << "] not found.\n";

    } else if (sub == "assign") {
        auto cfg = load_config();
        if (cfg.api_pairs.empty()) {
            std::cout << "[rp] No API pairs yet. Add one with: rp api add <url> <key>\n";
            return;
        }

        std::cout << "\n  Assign API Pairs to Functions\n";
        std::cout << "  ─────────────────────────────────────────────────────\n";
        for (auto& p : cfg.api_pairs)
            print_pair(p, p.id == cfg.main_llm_pair,
                          p.id == cfg.search_llm_pair);
        std::cout << "\n";

        // Main LLM
        std::cout << "  Main LLM — writes the final research report\n";
        std::cout << "  Current: ";
        if (auto p = active_main_pair(cfg))
            std::cout << "[" << p->id << "] " << p->url << "\n";
        else
            std::cout << "(none)\n";
        std::cout << "  Select number: ";
        int choice = 0;
        std::cin >> choice;
        if (find_pair(cfg, choice)) {
            assign_main_llm(choice);
            std::cout << "  ✓ Main LLM → [" << choice << "]\n\n";
        } else {
            std::cerr << "  Invalid number.\n\n";
        }

        // Reload after save
        cfg = load_config();

        // Search LLM
        std::cout << "  Search LLM — drives query planning inside searches\n";
        std::cout << "  (enter 0 to use same as Main LLM)\n";
        std::cout << "  Current: ";
        if (cfg.search_llm_pair == 0)
            std::cout << "(same as main)\n";
        else if (auto p = find_pair(cfg, cfg.search_llm_pair))
            std::cout << "[" << p->id << "] " << p->url << "\n";
        else
            std::cout << "(none)\n";
        std::cout << "  Select number (0 = same as main): ";
        choice = 0;
        std::cin >> choice;
        if (choice == 0 || find_pair(cfg, choice)) {
            assign_search_llm(choice);
            if (choice == 0)
                std::cout << "  ✓ Search LLM → (same as Main LLM)\n\n";
            else
                std::cout << "  ✓ Search LLM → [" << choice << "]\n\n";
        } else {
            std::cerr << "  Invalid number.\n\n";
        }

    } else if (sub == "model") {
        if (argc < 5) { std::cout << "Usage: rp api model <n> <model-name>\n"; return; }
        int id = std::stoi(argv[3]);
        std::string model = argv[4];
        if (set_pair_model(id, model))
            std::cout << "[rp] Pair [" << id << "] model set to: " << model << "\n";
        else
            std::cerr << "[rp] Pair [" << id << "] not found.\n";

    } else if (sub == "test") {
        if (argc < 4) { std::cout << "Usage: rp api test <number>\n"; return; }
        int id = std::stoi(argv[3]);
        auto cfg = load_config();
        auto pair = find_pair(cfg, id);
        if (!pair) { std::cerr << "[rp] Pair [" << id << "] not found.\n"; return; }

        std::string model = pair->model.empty() ? "deepseek-chat" : pair->model;
        std::cout << "\n  Testing API pair [" << id << "]\n";
        std::cout << "  URL   : " << pair->url << "\n";
        std::cout << "  Model : " << model << "\n";
        std::cout << "  Key   : " << mask_key(pair->key) << "\n\n";

        // DNS check first
        auto dns = resolve_url(pair->url);
        if (dns.resolved) {
            for (auto& ip : dns.a_records)    std::cout << "  A    " << ip << "  ✓\n";
            for (auto& ip : dns.aaaa_records) std::cout << "  AAAA " << ip << "  ✓\n";
        } else {
            std::cout << "  DNS  ✗  " << dns.error << "\n\n";
            return;
        }

        // Minimal API call — 1 token, no tools, just verify auth + measure latency
        std::cout << "\n  Sending test request...\n";
        json body = {
            {"model",      model},
            {"max_tokens", 8},
            {"messages",   json::array({{{"role","user"},{"content","Reply with: ok"}}})}
        };

        HttpClient http;
        http.timeout_sec = 15;
        auto t0 = std::chrono::steady_clock::now();
        auto resp = http.post(
            pair->url + "/chat/completions",
            body.dump(), "application/json",
            {{"Authorization", "Bearer " + pair->key}}
        );
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

        if (resp.status_code == 200) {
            std::string reply;
            try {
                auto j = json::parse(resp.body);
                reply = j["choices"][0]["message"]["content"].get<std::string>();
            } catch (...) { reply = "(could not parse response)"; }
            std::cout << "  ✓  " << ms << "ms  —  model replied: \"" << reply << "\"\n\n";
        } else if (resp.status_code == 401) {
            std::cout << "  ✗  401 Unauthorized — API key is invalid or wrong.\n\n";
        } else if (resp.status_code == 404) {
            std::cout << "  ✗  404 Not Found — URL may be wrong (missing /v1 path?)\n\n";
        } else if (resp.status_code == 0) {
            std::cout << "  ✗  No response — " << resp.error << "\n\n";
        } else {
            std::cout << "  ✗  HTTP " << resp.status_code << "  —  "
                      << resp.body.substr(0, 200) << "\n\n";
        }

    } else {
        std::cout << "Usage:\n"
                  << "  rp api list                      list all API pairs\n"
                  << "  rp api add <url> <key> [model]   add a pair\n"
                  << "  rp api rm <n>                    remove pair by number\n"
                  << "  rp api assign                    assign pairs to functions\n"
                  << "  rp api test <n>                  test a pair (DNS + auth + latency)\n";
    }
}

// ── rp skey ───────────────────────────────────────────────────────────────────

static void cmd_skey(int argc, char** argv) {
    std::string sub = (argc >= 3) ? argv[2] : "list";

    if (sub == "list") {
        auto cfg = load_config();
        std::cout << "\n  Server API Keys (used by clients calling this server)\n";
        std::cout << "  ─────────────────────────────────────────────────────\n";
        if (cfg.server_keys.empty()) {
            std::cout << "  (none — generate one with: rp skey new)\n\n";
            return;
        }
        for (auto& k : cfg.server_keys)
            std::cout << "  [" << k.id << "]  "
                      << std::left << std::setw(20) << k.label
                      << "  " << mask_key(k.key) << "\n";
        std::cout << "\n  Use 'rp skey new' to generate a new key.\n\n";

    } else if (sub == "new") {
        std::string label;
        if (argc >= 4) {
            label = argv[3];
        } else {
            std::cout << "  Label (press Enter to skip): ";
            std::getline(std::cin, label);
        }
        std::string key = add_server_key(label);
        auto cfg = load_config();
        int id = cfg.server_keys.back().id;
        std::cout << "\n  [" << id << "] New server key generated\n";
        std::cout << "  Label : " << cfg.server_keys.back().label << "\n";
        std::cout << "  Key   : " << key << "\n";
        std::cout << "\n  Clients send this as:  X-API-Key: " << key << "\n\n";

    } else if (sub == "rm") {
        if (argc < 4) { std::cout << "Usage: rp skey rm <number>\n"; return; }
        int id = std::stoi(argv[3]);
        if (remove_server_key(id))
            std::cout << "[rp] Server key [" << id << "] removed.\n";
        else
            std::cerr << "[rp] Key [" << id << "] not found.\n";

    } else {
        std::cout << "Usage:\n"
                  << "  rp skey list        list server API keys\n"
                  << "  rp skey new [label] generate a new server key\n"
                  << "  rp skey rm <n>      remove key by number\n";
    }
}

// ── rp lim ────────────────────────────────────────────────────────────────────

static void cmd_lim(int argc, char** argv) {
    auto cfg = load_config();

    if (argc < 4) {
        // Show current limits
        std::cout << "\n  Budget Limits\n";
        std::cout << "  ─────────────────────────────────────────────────────\n\n";
        std::cout << "  smart_search\n";
        std::cout << "    min : " << cfg.limits.ss_min << "  (AI must call at least this many times)\n";
        std::cout << "    max : " << cfg.limits.ss_max << "  (hard cap — blocked beyond this)\n\n";
        std::cout << "  fact_check\n";
        std::cout << "    min : " << cfg.limits.fc_min << "  (AI must call at least this many times)\n";
        std::cout << "    max : " << cfg.limits.fc_max << "  (hard cap — blocked beyond this)\n\n";
        std::cout << "  Set with:\n";
        std::cout << "    rp lim ss     <n>   smart_search hard cap\n";
        std::cout << "    rp lim ss-min <n>   smart_search minimum\n";
        std::cout << "    rp lim fc     <n>   fact_check hard cap\n";
        std::cout << "    rp lim fc-min <n>   fact_check minimum\n\n";
        return;
    }

    std::string field = argv[2];
    int val = 0;
    try { val = std::stoi(argv[3]); }
    catch (...) { std::cerr << "[rp] Value must be a number.\n"; return; }
    if (val < 1) { std::cerr << "[rp] Value must be >= 1.\n"; return; }

    auto lim = cfg.limits;

    if      (field == "ss")     lim.ss_max = val;
    else if (field == "ss-min") lim.ss_min = val;
    else if (field == "fc")     lim.fc_max = val;
    else if (field == "fc-min") lim.fc_min = val;
    else {
        std::cerr << "[rp] Unknown field: " << field << "\n"
                  << "     Use: ss, ss-min, fc, fc-min\n";
        return;
    }

    // Sanity: min must be < max
    if (lim.ss_min >= lim.ss_max) {
        std::cerr << "[rp] ss-min (" << lim.ss_min << ") must be less than ss-max (" << lim.ss_max << ").\n";
        return;
    }
    if (lim.fc_min >= lim.fc_max) {
        std::cerr << "[rp] fc-min (" << lim.fc_min << ") must be less than fc-max (" << lim.fc_max << ").\n";
        return;
    }

    set_limits(lim);
    std::cout << "[rp] " << field << " set to " << val << "\n";
    std::cout << "     smart_search: min=" << lim.ss_min << " max=" << lim.ss_max << "\n";
    std::cout << "     fact_check  : min=" << lim.fc_min << " max=" << lim.fc_max << "\n";
}

// ── rp records ────────────────────────────────────────────────────────────────

static void cmd_records() {
    auto cfg = load_config();

    std::cout << "\n  SmartScraper — Domain & Connectivity\n";
    std::cout << "  ─────────────────────────────────────────────────────\n\n";

    if (cfg.server_domain.empty()) {
        std::cout << "  No domain configured.\n";
        std::cout << "  Set one with:  rp domain <yourdomain.com>\n\n";
        return;
    }

    std::cout << "  Domain : " << cfg.server_domain << "\n";
    std::cout << "  Port   : " << cfg.server_port   << "\n\n";

    std::cout << "  Detecting this server's public IP...\n";
    std::string my_v4 = public_ipv4();
    std::string my_v6 = public_ipv6();
    std::cout << "  This server  A    : " << (my_v4.empty() ? "(no public IPv4)" : my_v4) << "\n";
    std::cout << "  This server  AAAA : " << (my_v6.empty() ? "(no public IPv6)" : my_v6) << "\n\n";

    std::cout << "  Resolving " << cfg.server_domain << "...\n";
    auto dns = resolve_dns(cfg.server_domain);

    if (!dns.resolved) {
        std::cout << "  DNS  ✗  " << (dns.error.empty() ? "not resolved" : dns.error) << "\n\n";
        std::cout << "  Add these records at your DNS provider:\n\n";
        if (!my_v4.empty()) std::cout << "    A     " << cfg.server_domain << "    " << my_v4 << "\n";
        if (!my_v6.empty()) std::cout << "    AAAA  " << cfg.server_domain << "    " << my_v6 << "\n";
        std::cout << "\n";
        return;
    }

    bool a_match    = !my_v4.empty() && std::find(dns.a_records.begin(),    dns.a_records.end(),    my_v4) != dns.a_records.end();
    bool aaaa_match = !my_v6.empty() && std::find(dns.aaaa_records.begin(), dns.aaaa_records.end(), my_v6) != dns.aaaa_records.end();

    std::cout << "\n  DNS Records\n";
    for (auto& ip : dns.a_records) {
        bool mine = (ip == my_v4);
        std::cout << "    A     " << std::left << std::setw(30) << cfg.server_domain
                  << "  " << std::setw(40) << ip
                  << "  " << tick(mine) << (mine ? " points here" : " NOT this server") << "\n";
    }
    if (dns.a_records.empty()) std::cout << "    A     (none)\n";

    for (auto& ip : dns.aaaa_records) {
        bool mine = (ip == my_v6);
        std::cout << "    AAAA  " << std::left << std::setw(30) << cfg.server_domain
                  << "  " << std::setw(40) << ip
                  << "  " << tick(mine) << (mine ? " points here" : " NOT this server") << "\n";
    }
    if (dns.aaaa_records.empty()) std::cout << "    AAAA  (none)\n";

    std::string check_host = a_match    ? my_v4 :
                             aaaa_match ? my_v6 :
                             !dns.a_records.empty() ? dns.a_records[0] : cfg.server_domain;

    std::cout << "\n  Connectivity (TCP port " << cfg.server_port << ")\n";
    bool reachable = check_tcp(check_host, cfg.server_port, 5);
    std::cout << "    " << tick(reachable) << "  " << cfg.server_domain << ":" << cfg.server_port;
    if (!reachable) std::cout << "  (port closed — run: rp start)";
    std::cout << "\n";

    std::cout << "\n  Summary\n";
    if ((a_match || aaaa_match) && reachable) {
        std::cout << "    ✓  Correctly pointed at this server and port is open.\n";
        std::cout << "       API URL: http://" << cfg.server_domain << ":" << cfg.server_port << "\n";
    } else {
        if (!a_match && !aaaa_match) {
            std::cout << "    ✗  DNS does not point at this server. Set at your DNS provider:\n";
            if (!my_v4.empty()) std::cout << "         A     " << cfg.server_domain << "    " << my_v4 << "\n";
            if (!my_v6.empty()) std::cout << "         AAAA  " << cfg.server_domain << "    " << my_v6 << "\n";
        }
        if (!reachable)
            std::cout << "    ✗  Port " << cfg.server_port << " not reachable. Run: rp start\n";
    }
    std::cout << "\n";
}

// ── rp domain ─────────────────────────────────────────────────────────────────

static void cmd_domain(const std::string& raw) {
    // Reuse extract_hostname to normalise whatever the user typed
    std::string domain = extract_hostname(raw);
    if (domain.empty()) { std::cerr << "[rp] Invalid domain.\n"; return; }

    auto cfg = load_config();
    cfg.server_domain = domain;
    save_config(cfg);

    std::cout << "[rp] Domain set: " << domain << "\n";
    auto dns = resolve_dns(domain);
    if (dns.resolved) {
        for (auto& ip : dns.a_records)    std::cout << "     A    " << ip << "\n";
        for (auto& ip : dns.aaaa_records) std::cout << "     AAAA " << ip << "\n";
        std::cout << "     Run 'rp records' for full connectivity check.\n";
    } else {
        std::cout << "     DNS ✗  not resolved yet — set the A/AAAA records at your provider first.\n";
        std::cout << "     Run 'rp records' to see what to set.\n";
    }
}

// ── rp config ─────────────────────────────────────────────────────────────────

static void cmd_config(bool reveal) {
    auto cfg = load_config();

    std::cout << "\n  SmartScraper Config\n";
    std::cout << "  ─────────────────────────────────────────────────────\n\n";

    // API pairs
    std::cout << "  API Pairs\n";
    if (cfg.api_pairs.empty()) {
        std::cout << "    (none — add with: rp api add <url> <key> [model])\n";
    } else {
        for (auto& p : cfg.api_pairs) {
            bool is_main   = p.id == cfg.main_llm_pair;
            bool is_search = p.id == cfg.search_llm_pair;
            std::cout << "    [" << p.id << "]  "
                      << std::left << std::setw(38) << p.url
                      << "  " << std::setw(16) << (p.model.empty() ? "deepseek-chat" : p.model)
                      << "  " << (reveal ? p.key : mask_key(p.key));
            if (is_main)   std::cout << "  ← main LLM";
            if (is_search) std::cout << "  ← search LLM";
            std::cout << "\n";
        }
    }

    std::cout << "\n  Active Functions\n";
    if (auto p = active_main_pair(cfg))
        std::cout << "    Main LLM   : [" << p->id << "] " << p->url << "  " << (p->model.empty() ? "deepseek-chat" : p->model) << "\n";
    else
        std::cout << "    Main LLM   : (not assigned — rp api assign)\n";

    if (cfg.search_llm_pair == 0)
        std::cout << "    Search LLM : (same as main)\n";
    else if (auto p = find_pair(cfg, cfg.search_llm_pair))
        std::cout << "    Search LLM : [" << p->id << "] " << p->url << "  " << (p->model.empty() ? "deepseek-chat" : p->model) << "\n";
    else
        std::cout << "    Search LLM : (not set)\n";

    std::cout << "\n  Server\n";
    std::cout << "    Host   : " << cfg.server_host << "\n";
    std::cout << "    Port   : " << cfg.server_port << "\n";
    std::cout << "    Domain : " << (cfg.server_domain.empty() ? "(not set — rp domain <domain>)" : cfg.server_domain) << "\n";
    std::cout << "    Keys   : " << cfg.server_keys.size() << " key(s)  (rp skey list)\n\n";

    std::cout << "  Config : " << config_path().string() << "\n";
    std::cout << "  Reports: " << reports_dir().string()  << "\n";
    if (!reveal) std::cout << "\n  Tip: rp config --reveal to see full keys\n";
    std::cout << "\n";
}

// ── rp status ─────────────────────────────────────────────────────────────────

static void cmd_status() {
    auto cfg = load_config();

    std::cout << "\n  SmartScraper Status\n";
    std::cout << "  ─────────────────────────────────────────────────────\n\n";

    // Server process
    int pid = read_pid();
    std::cout << "  API server   " << tick(is_our_server(pid));
    if (is_our_server(pid)) std::cout << "  pid=" << pid << "  http://localhost:" << cfg.server_port << "\n";
    else                 std::cout << "  stopped  (rp start)\n";

    // Active LLM
    std::cout << "\n  Main LLM (report writer)\n";
    if (auto p = active_main_pair(cfg)) {
        show_url_dns(p->url, "URL  ");
        std::cout << "    Model : " << (p->model.empty() ? "deepseek-chat" : p->model) << "\n";
        std::cout << "    Key   : " << mask_key(p->key) << "\n";
    } else {
        std::cout << "    (not assigned — rp api add <url> <key>, then rp api assign)\n";
    }

    std::cout << "\n  Search LLM (query planning)\n";
    if (cfg.search_llm_pair == 0) {
        std::cout << "    (same as main LLM)\n";
    } else if (auto p = find_pair(cfg, cfg.search_llm_pair)) {
        show_url_dns(p->url, "URL  ");
        std::cout << "    Model : " << (p->model.empty() ? "deepseek-chat" : p->model) << "\n";
        std::cout << "    Key   : " << mask_key(p->key) << "\n";
    } else {
        std::cout << "    (not assigned)\n";
    }

    // Domain
    std::cout << "\n  Domain\n";
    if (cfg.server_domain.empty()) {
        std::cout << "    (not set — rp domain <yourdomain.com>)\n";
    } else {
        auto dns       = resolve_dns(cfg.server_domain);
        bool reachable = dns.resolved && check_tcp(
            dns.a_records.empty() ? cfg.server_domain : dns.a_records[0],
            cfg.server_port, 3);
        std::cout << "    " << cfg.server_domain << "\n";
        if (dns.resolved) {
            for (auto& ip : dns.a_records)    std::cout << "      A    " << ip << "\n";
            for (auto& ip : dns.aaaa_records) std::cout << "      AAAA " << ip << "\n";
        } else {
            std::cout << "      DNS ✗  " << dns.error << "\n";
        }
        std::cout << "      Port " << cfg.server_port << "  " << tick(reachable)
                  << (reachable ? "  reachable" : "  not reachable") << "\n";
        std::cout << "      (run 'rp records' for full report)\n";
    }

    // Cache
    auto rdir = reports_dir();
    int rc = 0;
    if (fs::exists(rdir))
        for (auto& p : fs::directory_iterator(rdir))
            if (p.path().extension() == ".json") ++rc;
    std::cout << "\n  Cached reports : " << rc << "\n\n";
}

// ── rp start / stop ───────────────────────────────────────────────────────────

static void cmd_start() {
    if (is_our_server(read_pid())) {
        auto cfg = load_config();
        std::cout << "[rp] Already running (pid " << read_pid()
                  << ") on " << cfg.server_host << ":" << cfg.server_port << "\n";
        return;
    }
    run_server_daemon();
}

static void cmd_stop() {
    int pid = read_pid();
    if (!is_our_server(pid)) { std::cout << "[rp] Not running.\n"; std::remove(PID_FILE); return; }
    if (kill(pid, SIGTERM) == 0) { std::cout << "[rp] Stopped (pid " << pid << ").\n"; std::remove(PID_FILE); }
    else std::cerr << "[rp] Failed to stop pid " << pid << "\n";
}

// ── shell helper (silences warn_unused_result on std::system) ─────────────────

static int run(const std::string& cmd) {
    int r = std::system(cmd.c_str());
    return r;
}

// ── rp logs ──────────────────────────────────────────────────────────────────

static void cmd_logs(int argc, char** argv) {
    auto lp = log_path();
    std::string sub = (argc >= 3) ? argv[2] : "";

    if (sub == "clear") {
        if (!fs::exists(lp)) { std::cout << "[rp] Log is already empty.\n"; return; }
        std::ofstream(lp, std::ios::trunc);
        std::cout << "[rp] Log cleared.\n";
        return;
    }

    if (sub == "-f" || sub == "--follow") {
        if (!fs::exists(lp)) { std::cout << "[rp] No log file yet. Start the server first.\n"; return; }
        // Print last 20 lines then follow
        run("tail -n 20 -f " + lp.string());
        return;
    }

    // Default: print last 50 lines
    if (!fs::exists(lp)) { std::cout << "[rp] No log file yet. Start the server first.\n"; return; }
    int n = 50;
    if (!sub.empty()) { try { n = std::stoi(sub); } catch (...) {} }
    run("tail -n " + std::to_string(n) + " " + lp.string());
}

// ── rp cache ─────────────────────────────────────────────────────────────────

static void cmd_cache(int argc, char** argv) {
    std::string sub = (argc >= 3) ? argv[2] : "";
    if (sub == "clear") {
        auto dir = reports_dir();
        if (!fs::exists(dir)) { std::cout << "[rp] Cache is already empty.\n"; return; }
        int count = 0;
        for (auto& p : fs::directory_iterator(dir)) {
            if (p.path().extension() == ".json") { fs::remove(p.path()); count++; }
        }
        std::cout << "[rp] Cleared " << count << " cached report(s).\n";
    } else if (sub == "size") {
        auto dir = reports_dir();
        if (!fs::exists(dir)) { std::cout << "Cache: 0 reports\n"; return; }
        int count = 0;
        uintmax_t total = 0;
        for (auto& p : fs::directory_iterator(dir)) {
            if (p.path().extension() == ".json") { count++; total += fs::file_size(p.path()); }
        }
        std::cout << "Cache: " << count << " report(s), "
                  << (total / 1024) << " KB\n";
    } else if (sub == "on" || sub == "off") {
        auto cfg = load_config();
        cfg.cache_enabled = (sub == "on");
        save_config(cfg);
        std::cout << "[rp] Cache " << (cfg.cache_enabled ? "enabled" : "disabled") << ".\n";
    } else if (sub == "status") {
        auto cfg = load_config();
        std::cout << "Cache: " << (cfg.cache_enabled ? "enabled" : "disabled") << "\n";
    } else {
        std::cout << "Usage:\n"
                  << "  rp cache on       enable report caching (default)\n"
                  << "  rp cache off      disable report caching\n"
                  << "  rp cache status   show whether caching is on or off\n"
                  << "  rp cache clear    delete all cached reports\n"
                  << "  rp cache size     show report count and disk usage\n";
    }
}

// ── rp update ────────────────────────────────────────────────────────────────

static void cmd_update() {
    // Find where the binary was installed from (the repo dir)
    // We look for the repo by checking known relative paths from the binary
    // Strategy: use the GitHub repo directly — clone fresh to /tmp, build, install
    std::cout << "[rp] Checking for updates from GitHub...\n\n";

    bool server_was_running = is_our_server(read_pid());
    if (server_was_running) {
        std::cout << "[rp] Stopping server for update...\n";
        int pid = read_pid();
        kill(pid, SIGTERM);
        std::remove(PID_FILE);
        sleep(1);
    }

    const char* tmp_dir = "/tmp/smartscraper_update";
    std::string clone_cmd  = std::string("rm -rf ") + tmp_dir +
                             " && git clone --depth=1 https://github.com/GoldenEmperor1177/SmartScraper " + tmp_dir;
    std::string build_cmd  = std::string("cmake -S ") + tmp_dir + " -B " + tmp_dir + "/build"
                             " -DCMAKE_BUILD_TYPE=Release -Wno-dev"
                             " && cmake --build " + tmp_dir + "/build --parallel $(nproc)";
    std::string install_cmd = std::string("cmake --install ") + tmp_dir + "/build";
    std::string cleanup_cmd = std::string("rm -rf ") + tmp_dir;

    std::cout << "[rp] Cloning latest version...\n";
    if (run(clone_cmd) != 0) {
        std::cerr << "[rp] Clone failed. Check your internet connection.\n"; return;
    }

    std::cout << "\n[rp] Building...\n";
    if (run(build_cmd) != 0) {
        std::cerr << "[rp] Build failed.\n"; run(cleanup_cmd); return;
    }

    std::cout << "\n[rp] Installing...\n";
    std::string inst = (getuid() == 0) ? install_cmd : "sudo " + install_cmd;
    if (run(inst) != 0) {
        std::cerr << "[rp] Install failed.\n"; run(cleanup_cmd); return;
    }

    run(cleanup_cmd);
    std::cout << "\n[rp] Updated to v" APP_VERSION ". ✓\n";

    if (server_was_running) {
        std::cout << "[rp] Restarting server...\n";
        run("rp start");
    }
}

// ── rp --list-reports / --show-report ────────────────────────────────────────

static void cmd_list_reports() {
    auto rdir = reports_dir();
    if (!fs::exists(rdir)) { std::cout << "No cached reports.\n"; return; }

    struct Entry { std::string id, ts, req; int tools; };
    std::vector<Entry> rows;
    for (auto& p : fs::directory_iterator(rdir)) {
        if (p.path().extension() != ".json") continue;
        try {
            std::ifstream f(p.path());
            auto j = json::parse(f);
            rows.push_back({ j.value("id",""), j.value("timestamp","").substr(0,19),
                             j.value("user_request",""), j.value("tool_calls_used",0) });
        } catch (...) {}
    }
    std::sort(rows.begin(), rows.end(), [](auto& a, auto& b){ return a.ts > b.ts; });
    if (rows.empty()) { std::cout << "No cached reports.\n"; return; }

    std::cout << "\n" << std::left
              << std::setw(35) << "ID" << std::setw(22) << "Timestamp"
              << std::setw(6)  << "Tools" << "Request\n"
              << std::string(100, '-') << "\n";
    for (auto& r : rows) {
        std::string req = r.req.size() > 55 ? r.req.substr(0,55)+"..." : r.req;
        std::cout << std::setw(35) << r.id << std::setw(22) << r.ts
                  << std::setw(6)  << r.tools << req << "\n";
    }
    std::cout << "\n" << rows.size() << " report(s). Use --show-report <id> to view.\n\n";
}

static void cmd_show_report(const std::string& id) {
    auto path = reports_dir() / (id + ".json");
    if (!fs::exists(path)) { std::cerr << "Report not found: " << id << "\n"; std::exit(1); }
    std::ifstream f(path);
    auto j = json::parse(f);
    std::cout << "\n# " << j.value("user_request","") << "\n"
              << "# " << j.value("timestamp","").substr(0,19)
              << "  |  " << j.value("tool_calls_used",0) << " tool calls\n\n"
              << std::string(60,'=') << "\n\n"
              << j.value("final_report","") << "\n";
}

// ── help ──────────────────────────────────────────────────────────────────────

static void print_help() {
    std::cout << R"(
  rp — SmartScraper Report Maker
  ════════════════════════════════════════════════════

  API PAIRS  (url + key + model stored together, numbered)
    rp api add <url> <key> [model]   add a new API pair
    rp api list                      show all pairs with numbers
    rp api rm <n>                    remove pair by number
    rp api model <n> <model>         update model name on a pair
    rp api assign                    assign pairs to Main LLM / Search LLM
    rp api test <n>                  test pair: DNS + auth check + latency

  SERVER KEYS  (keys clients use to call this server)
    rp skey new [label]              generate a new server key
    rp skey list                     list server keys with numbers
    rp skey rm <n>                   remove key by number

  BUDGET LIMITS
    rp lim                           show current limits
    rp lim ss     <n>                smart_search hard cap  (default 5)
    rp lim ss-min <n>                smart_search minimum   (default 2)
    rp lim fc     <n>                fact_check hard cap    (default 8)
    rp lim fc-min <n>                fact_check minimum     (default 3)

  DOMAIN & CONNECTIVITY
    rp domain <domain>               set server domain (e.g. api.mysite.com)
    rp records                       check A/AAAA records + port connectivity

  CONFIGURATION
    rp config                        show full config (keys masked)
    rp config --reveal               show config with full keys
    rp status                        live status of server, LLMs, domain

  REPORT GENERATION
    rp "your query"                  run a report
    rp --quiet "query"               suppress tool traces
    rp --out file.md "query"         save report as markdown
    rp --pdf report.pdf "query"      save report as PDF
    rp --no-cache "query"            skip cache
    rp --no-context "query"          clean run (ignore prior reports)
    rp --max-iters 30 "query"        allow more tool-call rounds

  REPORT CACHE
    rp --list-reports                list cached reports
    rp --show-report <id>            print a cached report by id
    rp cache on                      enable report caching (default)
    rp cache off                     disable report caching
    rp cache status                  show whether caching is on or off
    rp cache clear                   delete all cached reports
    rp cache size                    show report count and disk usage

  SERVER
    rp --version                     show version
    rp start                         start HTTP API server (daemonises)
    rp stop                          stop API server
    rp update                        pull latest from GitHub, rebuild, reinstall
    rp logs                          show last 50 server log lines
    rp logs <n>                      show last N lines
    rp logs -f                       follow log live (Ctrl+C to stop)
    rp logs clear                    wipe the log file

)";
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    if (argc < 2) { print_help(); return 0; }
    std::string cmd = argv[1];

    if (cmd == "api")                        { cmd_api(argc, argv);    return 0; }
    if (cmd == "skey")                       { cmd_skey(argc, argv);   return 0; }
    if (cmd == "lim")                        { cmd_lim(argc, argv);    return 0; }
    if (cmd == "domain" && argc >= 3)        { cmd_domain(argv[2]);    return 0; }
    if (cmd == "records" || cmd == "record") { cmd_records();          return 0; }
    if (cmd == "config")                     { cmd_config(argc >= 3 && std::string(argv[2]) == "--reveal"); return 0; }
    if (cmd == "status")                     { cmd_status();           return 0; }
    if (cmd == "start")                      { cmd_start();            return 0; }
    if (cmd == "stop")                       { cmd_stop();             return 0; }
    if (cmd == "update")                     { cmd_update();           return 0; }
    if (cmd == "cache")                      { cmd_cache(argc, argv);  return 0; }
    if (cmd == "logs")                       { cmd_logs(argc, argv);   return 0; }
    if (cmd == "--list-reports")             { cmd_list_reports();     return 0; }
    if (cmd == "--show-report" && argc >= 3) { cmd_show_report(argv[2]); return 0; }
    if (cmd == "-h" || cmd == "-help" || cmd == "--help" || cmd == "help") { print_help(); return 0; }
    if (cmd == "--version" || cmd == "-v" || cmd == "version") {
        std::cout << "rp v" APP_VERSION "\n"; return 0;
    }

    // Catch unrecognised bare words that look like commands (no spaces, no flags)
    // A query must start with a flag (--quiet etc.) or be quoted (single argv token with spaces
    // is fine), but a bare single word that matches nothing is almost certainly a typo.
    if (argc == 2 && cmd.find(' ') == std::string::npos && cmd.rfind("--", 0) != 0) {
        std::cerr << "[rp] Unknown command: " << cmd << "\n"
                  << "     Try: rp --help\n";
        return 1;
    }

    // Report generation — everything else is treated as a query
    auto cfg = load_config();
    auto pair = active_main_pair(cfg);
    if (!pair) {
        std::cerr << "[rp] No API pair assigned. Run:\n"
                  << "       rp api add <url> <key>\n"
                  << "       rp api assign\n";
        return 1;
    }

    reportmaker::AgentConfig agent;
    agent.base_url = pair->url;
    agent.api_key  = pair->key;
    agent.model    = pair->model.empty() ? "deepseek-chat" : pair->model;
    agent.ss_min   = cfg.limits.ss_min;
    agent.ss_max   = cfg.limits.ss_max;
    agent.fc_min   = cfg.limits.fc_min;
    agent.fc_max   = cfg.limits.fc_max;
    agent.use_cache = cfg.cache_enabled;

    std::vector<std::string> parts;
    std::string out_file;
    std::string pdf_file;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--quiet")                   agent.verbose        = false;
        else if (a == "--no-cache")                agent.use_cache      = false;
        else if (a == "--no-context")              agent.use_context    = false;
        else if (a == "--max-iters" && i+1 < argc) agent.max_iterations = std::stoi(argv[++i]);
        else if (a == "--out"       && i+1 < argc) out_file             = argv[++i];
        else if (a == "--pdf"       && i+1 < argc) pdf_file             = argv[++i];
        else if (a == "--list-reports" || a == "--show-report") {
            // already handled above, skip here
        } else if (a.rfind("--", 0) == 0) {
            std::cerr << "[rp] Unknown flag: " << a << "\n"
                      << "     Try: rp --help\n";
            return 1;
        } else {
            parts.push_back(a);
        }
    }

    if (parts.empty()) {
        std::cerr << "[rp] No query provided. Wrap your query in quotes: rp \"your question\"\n"
                  << "     Try: rp --help\n";
        return 1;
    }

    std::string query;
    for (size_t i = 0; i < parts.size(); ++i) { if (i) query += ' '; query += parts[i]; }


    std::string report = reportmaker::run_report(query, agent);

    if (!pdf_file.empty()) {
        auto pdf_bytes = render_pdf(report);
        if (pdf_bytes.empty()) {
            std::cerr << "[rp] PDF generation failed.\n";
            return 1;
        }
        std::ofstream f(pdf_file, std::ios::binary);
        f.write(reinterpret_cast<const char*>(pdf_bytes.data()), pdf_bytes.size());
        std::cout << "\n[wrote PDF to " << pdf_file << "]\n";
    } else if (!out_file.empty()) {
        std::ofstream f(out_file);
        f << report << "\n";
        std::cout << "\n[wrote to " << out_file << "]\n";
    } else {
        std::cout << "\n" << std::string(60,'=') << "\nREPORT\n"
                  << std::string(60,'=') << "\n\n" << report << "\n";
    }
    return 0;
}
