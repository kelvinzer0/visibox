#!/usr/bin/env bash
# VisiBox Installer — one-liner: bash <(curl -sL https://raw.githubusercontent.com/kelvinzer0/visibox/master/install.sh)
set -euo pipefail

REPO="kelvinzer0/visibox"
INSTALL_DIR="${VISIBOX_INSTALL_DIR:-/usr/local/bin}"
TAG="${VISIBOX_VERSION:-latest}"

BOLD='\033[1m'
DIM='\033[2m'
OK='\033[0;32m'
ERR='\033[0;31m'
RST='\033[0m'

info()  { echo -e "${BOLD}→${RST} $1"; }
ok()    { echo -e "${OK}✓${RST} $1"; }
fail()  { echo -e "${ERR}✗${RST} $1" >&2; exit 1; }

# ── detect arch ──
ARCH=$(uname -m)
case "$ARCH" in
    x86_64|amd64)  ARCH_SUFFIX="x86_64" ;;
    aarch64|arm64)  ARCH_SUFFIX="aarch64" ;;
    *) fail "Unsupported architecture: $ARCH (only x86_64 and aarch64)" ;;
esac

# ── detect os ──
OS=$(uname -s)
case "$OS" in
    Linux) ;;
    *) fail "Unsupported OS: $OS (only Linux)" ;;
esac

# ── resolve tag ──
if [ "$TAG" = "latest" ]; then
    info "Fetching latest release..."
    TAG=$(curl -sfL "https://api.github.com/repos/${REPO}/releases/latest" | grep '"tag_name"' | sed 's/.*"v\(.*\)".*/\1/')
    [ -z "$TAG" ] && fail "Failed to resolve latest version"
fi

ARCHIVE="visibox-${ARCH_SUFFIX}-linux-gnu.tar.gz"
URL="https://github.com/${REPO}/releases/download/v${TAG}/${ARCHIVE}"

info "Installing VisiBox v${TAG} (${ARCH_SUFFIX})"

# ── download ──
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

info "Downloading ${URL}"
if ! curl -fSL --progress-bar -o "${TMPDIR}/${ARCHIVE}" "$URL"; then
    # fallback: try without 'gnu' suffix
    ARCHIVE="visibox-${ARCH_SUFFIX}-linux.tar.gz"
    URL="https://github.com/${REPO}/releases/download/v${TAG}/${ARCHIVE}"
    info "Retrying with ${URL}"
    curl -fSL --progress-bar -o "${TMPDIR}/${ARCHIVE}" "$URL" || fail "Download failed"
fi

# ── extract ──
info "Extracting..."
tar xzf "${TMPDIR}/${ARCHIVE}" -C "$TMPDIR"

# ── install ──
if [ -w "$INSTALL_DIR" ] || [ "$EUID" -eq 0 ]; then
    cp "${TMPDIR}/visibox" "${INSTALL_DIR}/visibox"
    chmod +x "${INSTALL_DIR}/visibox"
    ok "Installed to ${INSTALL_DIR}/visibox"
else
    sudo cp "${TMPDIR}/visibox" "${INSTALL_DIR}/visibox"
    sudo chmod +x "${INSTALL_DIR}/visibox"
    ok "Installed to ${INSTALL_DIR}/visibox (via sudo)"
fi

# ── verify ──
if command -v visibox &>/dev/null; then
    VERS=$(visibox --version 2>/dev/null | head -1)
    ok "Ready! ${VERS}"
else
    ok "Installed. Make sure ${INSTALL_DIR} is in your PATH."
fi

echo ""
echo -e "${DIM}Usage:${RST}"
echo -e "  visibox --visibox                    # REPL mode"
echo -e "  echo '{\"type\":\"execute\",\"command\":\"ls\"}' | visibox --visibox  # Pipe mode"
echo -e "  visibox --visibox-daemon             # Socket daemon mode"
echo ""