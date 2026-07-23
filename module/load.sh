#!/system/bin/sh

MODDIR=${0%/*}
LOG_FILE="$MODDIR/gki_ntsync.log"
MODULE_FILE="$MODDIR/gki_ntsync.ko"
SYSFS_MODULE=${SYSFS_MODULE:-/sys/module/gki_ntsync}
SOURCE=${1:-manual}

log() {
  echo "$(date '+%Y-%m-%d %H:%M:%S') $*" >> "$LOG_FILE"
}

run_and_log() {
  output="$LOG_FILE.command.$$.out"
  "$@" > "$output" 2>&1
  status=$?

  while IFS= read -r line || [ -n "$line" ]; do
    echo "$line"
    log "$line"
  done < "$output"
  rm -f "$output"
  return "$status"
}

log "load request: source=$SOURCE kernel=$(uname -r) module=$MODULE_FILE"

if [ "$KSU" != "true" ]; then
  log "check KernelSU: failed (KSU=${KSU:-unset}); skipping load"
  exit 0
fi
log "check KernelSU: passed"

if [ ! -f "$MODULE_FILE" ]; then
  log "check module file: failed ($MODULE_FILE not found)"
  exit 1
fi
log "check module file: passed"

if [ -d "$SYSFS_MODULE" ]; then
  log "check loaded state: already loaded; skipping load"
  exit 0
fi
log "check loaded state: not loaded"

KSUD=$(command -v ksud 2>/dev/null)
if [ -z "$KSUD" ]; then
  log "check ksud: failed (command not found)"
  exit 1
fi
log "check ksud: passed ($KSUD)"

log "loading gki_ntsync"
run_and_log "$KSUD" insmod "$MODULE_FILE"
status=$?
if [ "$status" -ne 0 ]; then
  log "load failed: ksud insmod exited with status $status"
  exit "$status"
fi

if [ ! -d "$SYSFS_MODULE" ]; then
  log "load failed: gki_ntsync is absent from $SYSFS_MODULE after ksud insmod"
  exit 1
fi

log "load succeeded: gki_ntsync is present at $SYSFS_MODULE"
