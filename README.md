# SmartScraper

AI-powered research report tool. Give it a query, it searches five sources, runs a grounded agentic loop against a DeepSeek-compatible LLM, and produces a sourced markdown report.

**Sources:** DuckDuckGo · Wikipedia · HackerNews · StackExchange · arXiv — all keyless, no dashboards.

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
