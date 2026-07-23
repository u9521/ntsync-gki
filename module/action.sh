#!/system/bin/sh

MODDIR=${0%/*}
SYSFS_MODULE=${SYSFS_MODULE:-/sys/module/gki_ntsync}

if [ "$KSU" != "true" ]; then
  echo "action request failed: KernelSU is unavailable (KSU=${KSU:-unset})"
  exit 1
fi

if [ ! -d "$SYSFS_MODULE" ]; then
  echo "gki_ntsync is not loaded; starting load"
  ash "$MODDIR/load.sh" action
  status=$?
  if [ "$status" -ne 0 ]; then
    echo "load failed with status $status; see $MODDIR/gki_ntsync.log"
    exit "$status"
  fi
  echo "gki_ntsync loaded successfully"
  exit 0
fi

RMMOD=$(command -v rmmod 2>/dev/null)
if [ -z "$RMMOD" ]; then
  echo "unload failed: rmmod command not found"
  exit 1
fi

echo "gki_ntsync is loaded; starting unload"
"$RMMOD" gki_ntsync
status=$?
if [ "$status" -ne 0 ]; then
  echo "unload failed: rmmod exited with status $status"
  exit "$status"
fi

if [ -d "$SYSFS_MODULE" ]; then
  echo "unload failed: gki_ntsync remains present at $SYSFS_MODULE"
  exit 1
fi

echo "gki_ntsync unloaded successfully"
