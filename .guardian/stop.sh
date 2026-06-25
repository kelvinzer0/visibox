#!/usr/bin/env bash
# ╔══════════════════════════════════════════════════════════════╗
# ║  STOP — Gracefully stop the entire guardian system          ║
# ╚══════════════════════════════════════════════════════════════╝
GUARDIAN_DIR="/home/z/my-project/.guardian"

echo "[*] Stopping guardian system..."

# Kill processes
pkill -f "guardian.sh" 2>/dev/null && echo "[✓] Guardian stopped" || echo "[·] Guardian not running"
pkill -f "watchdog.sh" 2>/dev/null && echo "[✓] Watchdog stopped" || echo "[·] Watchdog not running"

# Remove PID files
rm -f "${GUARDIAN_DIR}/guardian.pid" "${GUARDIAN_DIR}/watchdog.pid"

# Remove cron safety net (if crontab available)
if command -v crontab &>/dev/null; then
  (crontab -l 2>/dev/null | grep -v "GUARDIAN-SYSTEM-DO-NOT-REMOVE") | crontab -
  echo "[✓] Cron safety net removed"
fi

echo "[✓] System fully stopped."
echo "    Run 'bash ${GUARDIAN_DIR}/launcher.sh' to restart."