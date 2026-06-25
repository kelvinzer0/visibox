#!/usr/bin/env bash
# ╔══════════════════════════════════════════════════════════════╗
# ║  WATCHDOG — Ensures guardian.sh NEVER stops running        ║
# ║  If guardian dies → watchdog resurrects it instantly        ║
# ║  If watchdog dies → cron/systemd restarts watchdog          ║
# ╚══════════════════════════════════════════════════════════════╝
set -euo pipefail

# ─── Config ───────────────────────────────────────────────────
GUARDIAN_DIR="/home/z/my-project/.guardian"
GUARDIAN_SCRIPT="${GUARDIAN_DIR}/guardian.sh"
LOG_FILE="${GUARDIAN_DIR}/watchdog.log"
PID_FILE="${GUARDIAN_DIR}/watchdog.pid"
GUARDIAN_PID_FILE="${GUARDIAN_DIR}/guardian.pid"
CHECK_INTERVAL=3       # seconds between guardian health checks
MAX_RESTART_BURST=10   # max restarts within burst window
BURST_WINDOW=60        # seconds for restart burst detection

# ─── Ensure dirs ──────────────────────────────────────────────
mkdir -p "$GUARDIAN_DIR"

# ─── PID lock ─────────────────────────────────────────────────
if [[ -f "$PID_FILE" ]]; then
  OLD_PID=$(cat "$PID_FILE")
  if kill -0 "$OLD_PID" 2>/dev/null; then
    echo "[$(date -Iseconds)] Watchdog already running (PID ${OLD_PID}). Exiting." >> "$LOG_FILE"
    exit 0
  fi
fi
echo $$ > "$PID_FILE"

trap 'rm -f "$PID_FILE"' EXIT

# ─── Logging ──────────────────────────────────────────────────
log() {
  local level="$1"; shift
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] [${level}] $*" | tee -a "$LOG_FILE"
}

log "INFO" "══════════════════════════════════════════════════"
log "INFO" "WATCHDOG STARTED  (PID $$)"
log "INFO" "Guardian script: ${GUARDIAN_SCRIPT}"
log "INFO" "Check interval:  ${CHECK_INTERVAL}s"
log "INFO" "══════════════════════════════════════════════════"

# ─── State tracking for restart burst detection ──────────────
declare -a RESTART_TIMESTAMPS=()

# ─── Function: check and restart guardian ─────────────────────
ensure_guardian_alive() {
  local guardian_pid=""

  if [[ -f "$GUARDIAN_PID_FILE" ]]; then
    guardian_pid=$(cat "$GUARDIAN_PID_FILE")
    # Check if process is actually alive
    if kill -0 "$guardian_pid" 2>/dev/null; then
      # Process exists — verify it's actually OUR guardian (not PID recycled)
      local cmd_line
      cmd_line=$(cat /proc/$guardian_pid/cmdline 2>/dev/null | tr '\0' ' ' || echo "")
      if [[ "$cmd_line" == *"guardian.sh"* ]]; then
        return 0  # Guardian is alive and well
      else
        log "WARN" "PID ${guardian_pid} recycled by another process. Forcing restart."
      fi
    else
      log "WARN" "Guardian PID ${guardian_pid} is dead."
    fi
  else
    log "WARN" "Guardian PID file missing."
  fi

  # ── Burst detection ──
  local now
  now=$(date +%s)
  RESTART_TIMESTAMPS+=("$now")

  # Prune old timestamps outside burst window
  local cutoff=$((now - BURST_WINDOW))
  local -a filtered=()
  for ts in "${RESTART_TIMESTAMPS[@]}"; do
    (( ts > cutoff )) && filtered+=("$ts")
  done
  RESTART_TIMESTAMPS=("${filtered[@]}")

  if [[ ${#RESTART_TIMESTAMPS[@]} -ge $MAX_RESTART_BURST ]]; then
    log "ALERT" "⚠ BURST DETECTED: ${#RESTART_TIMESTAMPS[@]} restarts in ${BURST_WINDOW}s"
    log "ALERT" "  Possible kill loop attack. Slowing down to 30s interval."
    sleep 30
  fi

  # ── Kill any stale guardian processes ──
  pkill -f "guardian.sh" 2>/dev/null || true
  sleep 1

  # ── Respawn guardian ──
  nohup bash "$GUARDIAN_SCRIPT" >> "${GUARDIAN_DIR}/guardian.out" 2>&1 &
  local new_pid=$!
  sleep 1

  if kill -0 "$new_pid" 2>/dev/null; then
    log "RESTORE" "✓ Guardian resurrected (new PID ${new_pid})"
  else
    log "FATAL"  "✗ Guardian failed to start! Retrying in 5s..."
    sleep 5
    nohup bash "$GUARDIAN_SCRIPT" >> "${GUARDIAN_DIR}/guardian.out" 2>&1 &
    log "RESTORE" "✓ Guardian second attempt launched (PID $!)"
  fi
}

# ─── Function: self-heal — verify OWN integrity ───────────────
verify_self_integrity() {
  # Make sure guardian script hasn't been tampered with
  if [[ ! -f "$GUARDIAN_SCRIPT" ]]; then
    log "FATAL" "⚠ guardian.sh was DELETED! This is a critical attack."
    return 1
  fi
  # Check script is executable-ish (bash will run it anyway)
  if [[ ! -r "$GUARDIAN_SCRIPT" ]]; then
    log "FATAL" "⚠ guardian.sh is not readable!"
    return 1
  fi
  return 0
}

# ─── Main infinite loop ──────────────────────────────────────
RESTART_COUNT=0
while true; do
  if ! verify_self_integrity; then
    log "ALERT" "Self-integrity check failed. Sleeping 10s before retry."
    sleep 10
    continue
  fi

  ensure_guardian_alive
  sleep "$CHECK_INTERVAL"
done