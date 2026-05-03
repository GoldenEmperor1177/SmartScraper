#include "config.hpp"
#include <nlohmann/json.hpp>
#include <fstream>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <algorithm>

using json = nlohmann::json;

// ── Paths ─────────────────────────────────────────────────────────────────────

fs::path config_path() {
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : "/root") / ".smartscraper" / "config.json";
}

fs::path reports_dir() {
    return config_path().parent_path() / "reports";
}

// ── Load / save ───────────────────────────────────────────────────────────────

Config load_config() {
    Config cfg;
    auto path = config_path();
    if (!fs::exists(path)) return cfg;

    try {
        std::ifstream f(path);
        auto j = json::parse(f);

        for (auto& p : j.value("api_pairs", json::array())) {
            ApiPair pair;
            pair.id    = p.value("id",    0);
            pair.url   = p.value("url",   "");
            pair.key   = p.value("key",   "");
            pair.model = p.value("model", "");
            cfg.api_pairs.push_back(pair);
        }

        cfg.main_llm_pair   = j.value("main_llm_pair",   0);
        cfg.search_llm_pair = j.value("search_llm_pair", 0);

        if (j.contains("limits")) {
            auto& lm = j["limits"];
            if (lm.contains("ss_min")) cfg.limits.ss_min = lm["ss_min"].get<int>();
            if (lm.contains("ss_max")) cfg.limits.ss_max = lm["ss_max"].get<int>();
            if (lm.contains("fc_min")) cfg.limits.fc_min = lm["fc_min"].get<int>();
            if (lm.contains("fc_max")) cfg.limits.fc_max = lm["fc_max"].get<int>();
        }

        if (j.contains("server")) {
            auto& sv = j["server"];
            cfg.server_port   = sv.value("port",   8766);
            cfg.server_host   = sv.value("host",   "0.0.0.0");
            cfg.server_domain = sv.value("domain", "");
            cfg.cache_enabled = sv.value("cache_enabled", true);

            for (auto& k : sv.value("keys", json::array())) {
                ServerKey sk;
                sk.id    = k.value("id",    0);
                sk.key   = k.value("key",   "");
                sk.label = k.value("label", "");
                cfg.server_keys.push_back(sk);
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "[config] Warning: " << e.what() << "\n";
    }
    return cfg;
}

void save_config(const Config& cfg) {
    auto path = config_path();
    fs::create_directories(path.parent_path());
    fs::create_directories(reports_dir());

    json j;
    j["api_pairs"] = json::array();
    for (auto& p : cfg.api_pairs) {
        j["api_pairs"].push_back({
            {"id",    p.id},
            {"url",   p.url},
            {"key",   p.key},
            {"model", p.model}
        });
    }

    j["limits"] = {
        {"ss_min", cfg.limits.ss_min},
        {"ss_max", cfg.limits.ss_max},
        {"fc_min", cfg.limits.fc_min},
        {"fc_max", cfg.limits.fc_max}
    };

    j["main_llm_pair"]   = cfg.main_llm_pair;
    j["search_llm_pair"] = cfg.search_llm_pair;

    json skeys = json::array();
    for (auto& k : cfg.server_keys)
        skeys.push_back({{"id", k.id}, {"key", k.key}, {"label", k.label}});

    j["server"] = {
        {"port",          cfg.server_port},
        {"host",          cfg.server_host},
        {"domain",        cfg.server_domain},
        {"keys",          skeys},
        {"cache_enabled", cfg.cache_enabled}
    };

    std::ofstream f(path);
    f << j.dump(2) << "\n";
}

// ── mutate helper ─────────────────────────────────────────────────────────────

static void mutate(auto fn) {
    auto cfg = load_config();
    fn(cfg);
    save_config(cfg);
}

// ── API pair management ───────────────────────────────────────────────────────

int add_api_pair(const std::string& url, const std::string& key, const std::string& model) {
    auto cfg = load_config();
    int next_id = 1;
    for (auto& p : cfg.api_pairs)
        if (p.id >= next_id) next_id = p.id + 1;

    ApiPair pair;
    pair.id    = next_id;
    pair.url   = url;
    pair.key   = key;
    pair.model = model;
    cfg.api_pairs.push_back(pair);

    // Auto-assign as main if this is the first pair
    if (cfg.main_llm_pair == 0) cfg.main_llm_pair = next_id;

    save_config(cfg);
    return next_id;
}

bool remove_api_pair(int id) {
    auto cfg = load_config();
    auto it = std::remove_if(cfg.api_pairs.begin(), cfg.api_pairs.end(),
                             [id](auto& p){ return p.id == id; });
    if (it == cfg.api_pairs.end()) return false;
    cfg.api_pairs.erase(it, cfg.api_pairs.end());

    // Clear assignment if it pointed to this pair
    if (cfg.main_llm_pair   == id) cfg.main_llm_pair   = 0;
    if (cfg.search_llm_pair == id) cfg.search_llm_pair = 0;

    save_config(cfg);
    return true;
}

void assign_main_llm(int id)   { mutate([id](Config& c){ c.main_llm_pair   = id; }); }
void assign_search_llm(int id) { mutate([id](Config& c){ c.search_llm_pair = id; }); }

bool set_pair_model(int id, const std::string& model) {
    auto cfg = load_config();
    for (auto& p : cfg.api_pairs) {
        if (p.id == id) { p.model = model; save_config(cfg); return true; }
    }
    return false;
}

void set_limits(const Limits& lim) { mutate([&](Config& c){ c.limits = lim; }); }

// ── Server key management ─────────────────────────────────────────────────────

static std::string generate_key() {
    std::ifstream rng("/dev/urandom", std::ios::binary);
    unsigned char buf[32];
    rng.read(reinterpret_cast<char*>(buf), sizeof(buf));
    std::ostringstream ss;
    for (auto b : buf) ss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    return "sk-" + ss.str();
}

std::string add_server_key(const std::string& label) {
    auto cfg = load_config();
    int next_id = 1;
    for (auto& k : cfg.server_keys)
        if (k.id >= next_id) next_id = k.id + 1;

    ServerKey sk;
    sk.id    = next_id;
    sk.key   = generate_key();
    sk.label = label.empty() ? ("key-" + std::to_string(next_id)) : label;
    cfg.server_keys.push_back(sk);
    save_config(cfg);
    return sk.key;
}

bool remove_server_key(int id) {
    auto cfg = load_config();
    auto it = std::remove_if(cfg.server_keys.begin(), cfg.server_keys.end(),
                             [id](auto& k){ return k.id == id; });
    if (it == cfg.server_keys.end()) return false;
    cfg.server_keys.erase(it, cfg.server_keys.end());
    save_config(cfg);
    return true;
}

// ── Lookup helpers ────────────────────────────────────────────────────────────

std::optional<ApiPair> find_pair(const Config& cfg, int id) {
    for (auto& p : cfg.api_pairs)
        if (p.id == id) return p;
    return std::nullopt;
}

std::optional<ApiPair> active_main_pair(const Config& cfg) {
    return find_pair(cfg, cfg.main_llm_pair);
}

std::optional<ApiPair> active_search_pair(const Config& cfg) {
    if (cfg.search_llm_pair != 0)
        if (auto p = find_pair(cfg, cfg.search_llm_pair)) return p;
    return active_main_pair(cfg); // fallback to main
}
