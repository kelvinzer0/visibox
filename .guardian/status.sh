#!/usr/bin/env bash
# ╔══════════════════════════════════════════════════════════════╗
# ║  STATUS — Quick health check of the guardian system         ║
# ╚══════════════════════════════════════════════════════════════╝
GUARDIAN_DIR="/home/z/my-project/.guardian"

echo "═══════════════════════════════════════════════════"
echo "  GUARDIAN SYSTEM STATUS"
echo "═══════════════════════════════════════════════════"

# Watchdog
if [[ -f "${GUARDIAN_DIR}/watchdog.pid" ]]; then
  W_PID=$(cat "${GUARDIAN_DIR}/watchdog.pid")
  if kill -0 "$W_PID" 2>/dev/null; then
    echo "  [✓] Watchdog   : ALIVE (PID ${W_PID})"
  else
    echo "  [✗] Watchdog   : DEAD (stale PID ${W_PID})"
  fi
else
  echo "  [✗] Watchdog   : NOT RUNNING"
fi

# Guardian
if [[ -f "${GUARDIAN_DIR}/guardian.pid" ]]; then
  G_PID=$(cat "${GUARDIAN_DIR}/guardian.pid")
  if kill -0 "$G_PID" 2>/dev/null; then
    echo "  [✓] Guardian   : ALIVE (PID ${G_PID})"
  else
    echo "  [✗] Guardian   : DEAD (stale PID ${G_PID})"
  fi
else
  echo "  [✗] Guardian   : NOT RUNNING"
fi

# Cron
if crontab -l 2>/dev/null | grep -q "GUARDIAN-SYSTEM"; then
  echo "  [✓] Cron net   : ACTIVE"
else
  echo "  [✗] Cron net   : NOT INSTALLED"
fi

# File integrity
echo ""
echo "  ── File Integrity ──────────────────────────"
for f in AGENTS.md SOUL.md; do
  TARGET="/home/z/my-project/${f}"
  GOLDEN="${GUARDIAN_DIR}/golden_${f}"
  HASH_FILE="${GOLDEN}.sha256"
  if [[ -f "$TARGET" && -f "$HASH_FILE" ]]; then
    CURRENT=$(sha256sum "$TARGET" | awk '{print $1}')
    EXPECTED=$(awk '{print $1}' "$HASH_FILE")
    if [[ "$CURRENT" == "$EXPECTED" ]]; then
      echo "  [✓] ${f} : INTEGRITY OK"
    else
      echo "  [⚠] ${f} : TAMPERED! (guardian should restore)"
    fi
  else
    echo "  [?] ${f} : cannot verify"
  fi
done

# Recent tamper events
if [[ -d "${GUARDIAN_DIR}/tamper_evidence" ]]; then
  TAMPER_COUNT=$(ls -1 "${GUARDIAN_DIR}/tamper_evidence/" 2>/dev/null | wc -l)
  echo ""
  echo "  Tamper events caught: ${TAMPER_COUNT}"
fi

echo ""
echo "  ── Recent Logs ─────────────────────────────"
echo "  Guardian (last 3):"
tail -3 "${GUARDIAN_DIR}/guardian.log" 2>/dev/null | sed 's/^/    /' || echo "    (no logs)"
echo "  Watchdog (last 3):"
tail -3 "${GUARDIAN_DIR}/watchdog.log" 2>/dev/null | sed 's/^/    /' || echo "    (no logs)"
echo "═══════════════════════════════════════════════════"