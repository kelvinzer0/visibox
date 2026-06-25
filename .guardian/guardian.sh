#!/usr/bin/env bash
# ╔══════════════════════════════════════════════════════════════╗
# ║  GUARDIAN — File Integrity Monitor & Auto-Restorer         ║
# ║  Watches AGENTS.md & SOUL.md. If tampered → instant restore ║
# ╚══════════════════════════════════════════════════════════════╝
set -euo pipefail

# ─── Config ───────────────────────────────────────────────────
PROJECT_DIR="/home/z/my-project"
GUARDIAN_DIR="${PROJECT_DIR}/.guardian"
LOG_FILE="${GUARDIAN_DIR}/guardian.log"
PID_FILE="${GUARDIAN_DIR}/guardian.pid"
POLL_INTERVAL=2  # seconds between checks

WATCHED_FILES=(
  "AGENTS.md"
  "SOUL.md"
)

# ─── Ensure guardian dir exists ──────────────────────────────
mkdir -p "$GUARDIAN_DIR"

# ─── PID lock to prevent duplicate guardians ─────────────────
if [[ -f "$PID_FILE" ]]; then
  OLD_PID=$(cat "$PID_FILE")
  if kill -0 "$OLD_PID" 2>/dev/null; then
    echo "[$(date -Iseconds)] Guardian already running (PID ${OLD_PID}). Exiting." >> "$LOG_FILE"
    exit 0
  fi
fi
echo $$ > "$PID_FILE"

# ─── Trap: clean PID on exit (but we never exit) ─────────────
trap 'rm -f "$PID_FILE"' EXIT

# ─── Logging helper ──────────────────────────────────────────
log() {
  local level="$1"; shift
  echo "[$(date '+%Y-%m-%d %H:%M:%S')] [${level}] $*" | tee -a "$LOG_FILE"
}

# ─── Verify golden files exist ───────────────────────────────
for f in "${WATCHED_FILES[@]}"; do
  GOLDEN="${GUARDIAN_DIR}/golden_${f}"
  HASH_FILE="${GOLDEN}.sha256"
  if [[ ! -f "$GOLDEN" || ! -f "$HASH_FILE" ]]; then
    log "FATAL" "Missing golden file or hash for ${f}. Guardian cannot operate."
    exit 1
  fi
done

log "INFO" "══════════════════════════════════════════════════"
log "INFO" "GUARDIAN STARTED  (PID $$)"
log "INFO" "Watching: ${WATCHED_FILES[*]}"
log "INFO" "Poll interval: ${POLL_INTERVAL}s"
log "INFO" "══════════════════════════════════════════════════"

# ─── Main infinite loop ──────────────────────────────────────
while true; do
  for f in "${WATCHED_FILES[@]}"; do
    TARGET="${PROJECT_DIR}/${f}"
    GOLDEN="${GUARDIAN_DIR}/golden_${f}"
    HASH_FILE="${GOLDEN}.sha256"

    # ── File deleted? Restore immediately ──
    if [[ ! -f "$TARGET" ]]; then
      log "ALERT" "⚠ FILE DELETED: ${f} — restoring from golden backup"
      cp -f "$GOLDEN" "$TARGET"
      chmod 644 "$TARGET"
      log "RESTORE" "✓ ${f} restored (was deleted)"
      continue
    fi

    # ── Compute current hash ──
    CURRENT_HASH=$(sha256sum "$TARGET" | awk '{print $1}')
    GOLDEN_HASH=$(awk '{print $1}' "$HASH_FILE")

    # ── Compare hashes ──
    if [[ "$CURRENT_HASH" != "$GOLDEN_HASH" ]]; then
      # Save evidence of the tampering
      TAMPER_DIR="${GUARDIAN_DIR}/tamper_evidence"
      mkdir -p "$TAMPER_DIR"
      EVIDENCE_FILE="${TAMPER_DIR}/${f}.tampered.$(date +%s)"
      cp -f "$TARGET" "$EVIDENCE_FILE"

      # Get diff summary
      DIFF_LINES=$(diff "$GOLDEN" "$TARGET" 2>/dev/null | wc -l || echo "0")

      log "ALERT"  "⚠ TAMPER DETECTED: ${f} (${DIFF_LINES} lines changed)"
      log "ALERT"  "  Expected hash: ${GOLDEN_HASH}"
      log "ALERT"  "  Current hash:  ${CURRENT_HASH}"
      log "ALERT"  "  Evidence saved: ${EVIDENCE_FILE}"

      # Restore golden file
      cp -f "$GOLDEN" "$TARGET"
      chmod 644 "$TARGET"

      VERIFY_HASH=$(sha256sum "$TARGET" | awk '{print $1}')
      if [[ "$VERIFY_HASH" == "$GOLDEN_HASH" ]]; then
        log "RESTORE" "✓ ${f} restored to golden state (hash verified)"
      else
        log "FATAL"  "✗ Restore verification FAILED for ${f}! Golden file may be corrupted!"
      fi
    fi
  done

  sleep "$POLL_INTERVAL"
done