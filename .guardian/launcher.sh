#!/usr/bin/env bash
# ╔══════════════════════════════════════════════════════════════╗
# ║  LAUNCHER — One command to start the full guardian system   ║
# ║  Starts watchdog (which auto-starts guardian) as nohup      ║
# ║  Also installs a cron safety net for maximum resilience     ║
# ╚══════════════════════════════════════════════════════════════╝
set -euo pipefail

GUARDIAN_DIR="/home/z/my-project/.guardian"
WATCHDOG_SCRIPT="${GUARDIAN_DIR}/watchdog.sh"
CRON_MARKER="# GUARDIAN-SYSTEM-DO-NOT-REMOVE"

echo "═══════════════════════════════════════════════════"
echo "  GUARDIAN SYSTEM LAUNCHER"
echo "═══════════════════════════════════════════════════"

# ─── 1. Kill any existing instances ───────────────────────────
echo "[*] Cleaning up old instances..."
pkill -f "guardian.sh" 2>/dev/null || true
pkill -f "watchdog.sh" 2>/dev/null || true
sleep 1
rm -f "${GUARDIAN_DIR}/guardian.pid" "${GUARDIAN_DIR}/watchdog.pid"

# ─── 2. Verify golden backups exist ──────────────────────────
for f in golden_AGENTS.md golden_SOUL.md golden_AGENTS.md.sha256 golden_SOUL.md.sha256; do
  if [[ ! -f "${GUARDIAN_DIR}/${f}" ]]; then
    echo "[!] ERROR: Missing ${GUARDIAN_DIR}/${f}"
    echo "    Run golden backup creation first."
    exit 1
  fi
done
echo "[✓] Golden backups verified"

# ─── 3. Make scripts executable ───────────────────────────────
chmod +x "$WATCHDOG_SCRIPT"
echo "[✓] Scripts are executable"

# ─── 4. Launch watchdog as nohup daemon ───────────────────────
nohup bash "$WATCHDOG_SCRIPT" >> "${GUARDIAN_DIR}/watchdog.out" 2>&1 &
WATCHDOG_PID=$!
sleep 2

if kill -0 "$WATCHDOG_PID" 2>/dev/null; then
  echo "[✓] Watchdog launched (PID ${WATCHDOG_PID})"
else
  echo "[!] Watchdog failed to start. Check ${GUARDIAN_DIR}/watchdog.log"
  exit 1
fi

# ─── 5. Verify guardian spawned ──────────────────────────────
sleep 2
if [[ -f "${GUARDIAN_DIR}/guardian.pid" ]]; then
  GUARDIAN_PID=$(cat "${GUARDIAN_DIR}/guardian.pid")
  if kill -0 "$GUARDIAN_PID" 2>/dev/null; then
    echo "[✓] Guardian is alive (PID ${GUARDIAN_PID})"
  else
    echo "[!] Guardian PID file exists but process not found. Watchdog will respawn."
  fi
else
  echo "[!] Guardian not yet spawned. Watchdog will handle it."
fi

# ─── 6. Cron safety net (install if crontab available) ──────
if command -v crontab &>/dev/null; then
  CRON_CMD="* * * * * pgrep -f 'watchdog.sh' > /dev/null || nohup bash ${WATCHDOG_SCRIPT} >> ${GUARDIAN_DIR}/watchdog.out 2>&1 & ${CRON_MARKER}"
  (crontab -l 2>/dev/null | grep -v "GUARDIAN-SYSTEM-DO-NOT-REMOVE") | crontab -
  (crontab -l 2>/dev/null; echo "$CRON_CMD") | crontab -
  echo "[✓] Cron safety net installed (every minute check)"
else
  echo "[·] Cron not available — watchdog self-loop is the sole resilience layer"
fi

echo ""
echo "═══════════════════════════════════════════════════"
echo "  SYSTEM ACTIVE"
echo "═══════════════════════════════════════════════════"
echo "  Watchdog PID : ${WATCHDOG_PID}"
echo "  Guardian PID : $(cat ${GUARDIAN_DIR}/guardian.pid 2>/dev/null || echo 'pending...')"
echo "  Logs         : ${GUARDIAN_DIR}/guardian.log"
echo "                ${GUARDIAN_DIR}/watchdog.log"
echo "  Tamper ev.   : ${GUARDIAN_DIR}/tamper_evidence/"
echo ""
echo "  Commands:"
echo "    Status   : bash ${GUARDIAN_DIR}/status.sh"
echo "    Stop     : bash ${GUARDIAN_DIR}/stop.sh"
echo "    Restart  : bash ${GUARDIAN_DIR}/launcher.sh"
echo "═══════════════════════════════════════════════════"