#pragma once
#include <string>
#include <vector>
#include <optional>
#include <filesystem>

namespace fs = std::filesystem;

// ── API pair — a URL + key + model stored together, identified by number ──────

struct ApiPair {
    int         id    = 0;
    std::string url;
    std::string key;
    std::string model; // optional; defaults to "deepseek-chat" at use-time
};

// ── Server key — a key clients use to call THIS server's API ─────────────────

struct ServerKey {
    int         id    = 0;
    std::string key;
    std::string label;
};

// ── Budget limits ─────────────────────────────────────────────────────────────

struct Limits {
    int ss_min =  2;  // smart_search minimum calls before report is accepted
    int ss_max =  5;  // smart_search hard cap
    int fc_min =  3;  // fact_check minimum calls before report is accepted
    int fc_max =  8;  // fact_check hard cap
};

// ── Root config ───────────────────────────────────────────────────────────────

struct Config {
    std::vector<ApiPair>   api_pairs;
    int                    main_llm_pair    = 0;
    int                    search_llm_pair  = 0;
    int                    server_port      = 8766;
    std::string            server_host      = "0.0.0.0";
    std::string            server_domain;
    std::vector<ServerKey> server_keys;
    Limits                 limits;
    bool                   cache_enabled    = true;
};

// ── Paths ─────────────────────────────────────────────────────────────────────

fs::path config_path();
fs::path reports_dir();

// ── Load / save ───────────────────────────────────────────────────────────────

Config load_config();
void   save_config(const Config&);

// ── API pair management ───────────────────────────────────────────────────────

int  add_api_pair(const std::string& url, const std::string& key,
                  const std::string& model = "");
bool remove_api_pair(int id);
void assign_main_llm(int id);
void assign_search_llm(int id);          // 0 = same as main
bool set_pair_model(int id, const std::string& model);

// ── Limits management ─────────────────────────────────────────────────────────

void set_limits(const Limits&);

// ── Server key management ─────────────────────────────────────────────────────

// Generates a cryptographically random key, stores it, returns the key string.
std::string add_server_key(const std::string& label = "");
bool        remove_server_key(int id);

// ── Lookup helpers ────────────────────────────────────────────────────────────

std::optional<ApiPair> active_main_pair(const Config&);
std::optional<ApiPair> active_search_pair(const Config&); // falls back to main
std::optional<ApiPair> find_pair(const Config&, int id);
