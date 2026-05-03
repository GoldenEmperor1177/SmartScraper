#!/usr/bin/env bash
# SmartScraper C++ — install script
# Usage: bash scripts/install.sh
set -e

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$REPO_DIR/build"
INSTALL_BIN="/usr/local/bin/rp"
CONFIG_DIR="$HOME/.smartscraper"
CONFIG_FILE="$CONFIG_DIR/config.json"
SYSTEMD_DIR="$HOME/.config/systemd/user"
SERVICE_FILE="$SYSTEMD_DIR/smartscraper.service"

echo ""
echo "  SmartScraper — Install"
echo "  ════════════════════════════════════════════════════"
echo ""

# ── Check dependencies ────────────────────────────────────────────────────────

check_dep() {
    if ! command -v "$1" &>/dev/null; then
        echo "  ✗  $1 not found."
        echo "     Install: $2"
        exit 1
    fi
    echo "  ✓  $1"
}

check_dep cmake  "sudo apt install cmake"
check_dep make   "sudo apt install build-essential"
check_dep g++    "sudo apt install build-essential"
check_dep curl   "sudo apt install curl"

if ! pkg-config --exists libcurl 2>/dev/null; then
    echo "  ✗  libcurl dev headers not found."
    echo "     sudo apt install libcurl4-openssl-dev   (Debian/Ubuntu)"
    echo "     sudo dnf install libcurl-devel           (Fedora/RHEL)"
    exit 1
fi
echo "  ✓  libcurl $(pkg-config --modversion libcurl)"

if ! pkg-config --exists openssl 2>/dev/null; then
    echo "  ✗  OpenSSL dev headers not found."
    echo "     sudo apt install libssl-dev"
    exit 1
fi
echo "  ✓  openssl $(pkg-config --modversion openssl)"

if ! ldconfig -p 2>/dev/null | grep -q libhpdf; then
    echo "  ✗  libhpdf not found."
    echo "     sudo apt install libhpdf-dev"
    exit 1
fi
echo "  ✓  libhpdf (PDF generation)"

# ── Build ─────────────────────────────────────────────────────────────────────

echo ""
echo "  Building..."
echo ""

mkdir -p "$BUILD_DIR"
cmake -S "$REPO_DIR" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    -Wno-dev 2>&1 | tail -3

cmake --build "$BUILD_DIR" --parallel "$(nproc)"

echo ""
echo "  Installing rp → $INSTALL_BIN"
echo ""

if [ "$(id -u)" = "0" ]; then
    cmake --install "$BUILD_DIR"
else
    sudo cmake --install "$BUILD_DIR"
fi

echo "  ✓  rp installed"

# ── Config directory ──────────────────────────────────────────────────────────

mkdir -p "$CONFIG_DIR" "$CONFIG_DIR/reports"

if [ ! -f "$CONFIG_FILE" ]; then
    # Write a clean empty config — the app manages structure from here
    cat > "$CONFIG_FILE" <<'EOF'
{
  "api_pairs": [],
  "main_llm_pair": 0,
  "search_llm_pair": 0,
  "limits": {
    "ss_min": 2,
    "ss_max": 5,
    "fc_min": 3,
    "fc_max": 8
  },
  "server": {
    "port": 8766,
    "host": "0.0.0.0",
    "domain": "",
    "keys": []
  }
}
EOF
    echo "  ✓  Config created: $CONFIG_FILE"
else
    echo "  ✓  Config exists:  $CONFIG_FILE"
fi

# ── Systemd user service ──────────────────────────────────────────────────────

if command -v systemctl &>/dev/null; then
    mkdir -p "$SYSTEMD_DIR"
    cat > "$SERVICE_FILE" <<EOF
[Unit]
Description=SmartScraper API Server
After=network.target

[Service]
Type=simple
ExecStart=$INSTALL_BIN start
Restart=on-failure
RestartSec=5

[Install]
WantedBy=default.target
EOF
    systemctl --user daemon-reload 2>/dev/null || true
    echo "  ✓  Systemd service: $SERVICE_FILE"
    echo "     Enable at boot:  systemctl --user enable smartscraper"
fi

# ── Done ──────────────────────────────────────────────────────────────────────

echo ""
echo "  ════════════════════════════════════════════════════"
echo "  Done. Next steps:"
echo ""
echo "  1. Add your API key and URL:"
echo "       rp api add https://api.deepseek.com <your-key> deepseek-chat"
echo ""
echo "  2. (Optional) Set a second pair for the search agent:"
echo "       rp api add https://api.deepseek.com <your-key> deepseek-v2-chat"
echo "       rp api assign"
echo ""
echo "  3. Set your server domain:"
echo "       rp domain api.yourdomain.com"
echo ""
echo "  4. Check everything:"
echo "       rp status"
echo "       rp records"
echo ""
echo "  5. Run a report:"
echo "       rp \"your research query\""
echo ""
echo "  Full command list:  rp --help"
echo ""
