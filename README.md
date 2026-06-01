# SmartScraper

AI-powered research report tool. Give it a query, it searches the open web, runs a grounded agentic loop against a DeepSeek-compatible LLM, and produces a sourced markdown report.

**Search:** DuckDuckGo (worldwide) — indexes the open web including news, market reports, industry blogs, and reference pages. No API key required.

---

## Install

```bash
# Dependencies (Debian/Ubuntu)
sudo apt install cmake build-essential libcurl4-openssl-dev libssl-dev libhpdf-dev

# Build and install
git clone <repo>
cd SmartScraperCpp
bash scripts/install.sh
```

---

## Quick start

```bash
# Add your LLM API pair (DeepSeek or any OpenAI-compatible endpoint)
rp api add https://api.deepseek.com <your-api-key> deepseek-chat

# Run a report
rp "what is the current state of the AI agent market"
```

---

## Commands

### API pairs — LLM credentials, numbered
```
rp api add <url> <key> [model]   add a pair → auto-numbered
rp api list                      list all pairs
rp api rm <n>                    remove by number
rp api model <n> <model>         update model on a pair
rp api assign                    assign pairs to Main LLM / Search LLM
rp api test <n>                  DNS + auth + latency check
```

### Server keys — for clients calling this server
```
rp skey new [label]              generate a new server key
rp skey list                     list keys with numbers
rp skey rm <n>                   remove by number
```

### Budget limits
```
rp lim                           show current limits
rp lim ss <n>                    smart_search hard cap  (default 5)
rp lim ss-min <n>                smart_search minimum   (default 2)
rp lim fc <n>                    fact_check hard cap    (default 8)
rp lim fc-min <n>                fact_check minimum     (default 3)
```

### Domain & connectivity
```
rp domain <domain>               set server domain
rp records                       resolve DNS, check A/AAAA, test port
```

### Config & status
```
rp config                        show config (keys masked)
rp config --reveal               show config with full keys
rp status                        live status: server, LLMs, domain
```

### Report generation
```
rp "your query"                  run a report
rp --quiet "query"               suppress tool traces
rp --out report.md "query"       save as markdown
rp --pdf report.pdf "query"      save as PDF
rp --no-cache "query"            skip cache
rp --no-context "query"          ignore prior reports
rp --max-iters 30 "query"        more tool-call rounds
```

### Report cache
```
rp --list-reports                list cached reports
rp --show-report <id>            print a report by id
```

### Server
```
rp start                         start HTTP API server (daemonises)
rp stop                          stop API server
```

### HTTP API

Once running, the server listens on port 8766.

**Health check** (no auth):
```
GET /health
→ {"status":"ok"}
```

**Generate a report** (requires server key):
```
POST /report
Authorization: Bearer <skey>
Content-Type: application/json

{"query": "your question"}              → {"report": "<markdown>"}
{"query": "your question", "format": "pdf"}  → application/pdf binary
```

---

## Config file

Stored at `~/.smartscraper/config.json` — managed entirely by `rp` commands, no manual editing needed.

Reports cached at `~/.smartscraper/reports/`.

---

## Dependencies

| Library | Use | Source |
|---------|-----|--------|
| libcurl | HTTP requests | system |
| OpenSSL | TLS | system |
| libhpdf | PDF generation | system |
| nlohmann/json | JSON | auto-fetched by CMake |

No other runtime dependencies. Single binary output.

---

## For AI agents / contributors — critical rules

### Search sources
`smart_search` uses **DuckDuckGo only** (worldwide region `wt-wt`). Do not add Wikipedia, HackerNews, StackExchange, or arXiv back as sources. They were deliberately removed because:
- Wikipedia returns encyclopedia articles about companies, not business/marketing data
- HackerNews and StackExchange are tech forums with no domain-specific research data
- arXiv is academic papers — irrelevant for most real-world research queries

These sources caused the agent to report zero grounded data for any business or marketing query. DuckDuckGo indexes the full open web including industry reports, blogs, and news — it is the right tool for general research.

The worldwide region (`wt-wt`) is also intentional. `us-en` was the original default and caused India-specific and other regional queries to be geo-filtered, returning no results.

### Versioning
The version string lives in exactly one place: `src/main.cpp` line 1 — `#define APP_VERSION "x.y.z"`.

**Every commit must bump this string.** If you change any source file and forget to bump `APP_VERSION`, `rp update` will build and install the new binary but report the old version number, which looks broken.

Commit message convention (match existing history):
```
v1.0.X — short description of what changed
```

Tag every release commit:
```bash
git tag v1.0.X
git push origin v1.0.X
```

### How `rp update` works (bootstrapping caveat)
`rp update` clones main from GitHub, builds, installs to `/usr/local/bin/rp`, then calls `rp --version` on the newly installed binary to report the result. This means:

- The **first** `rp update` after a fix to the update command itself will still show the old version number (the old binary runs the update, installs the new binary, then exits — the popen call reads the new binary correctly but the old binary's flow already ended)
- Run `rp update` a **second time** to confirm — the new binary will correctly report its own version
- `rp --version` always tells you exactly what is currently installed

### Files that must all be committed together
When making changes, these files are commonly modified together. Do **not** commit `tools.cpp` alone without checking whether the others need to go with it — a partial commit will break the build on `rp update`:

| File | What it contains |
|------|-----------------|
| `src/main.cpp` | Version string + all CLI commands |
| `src/reportmaker/tools.cpp` | `smart_search` and `fact_check` tool implementations |
| `src/reportmaker/agent.cpp` | Agentic loop, system prompt, tool dispatch |
| `src/reportmaker/stats.cpp` | Rate-limit tracking, server stats persistence |
| `include/reportmaker/stats.hpp` | Stats header — must be present or build fails |
| `src/server.cpp` | HTTP API server |
| `src/config.cpp` / `include/config.hpp` | Config load/save |
