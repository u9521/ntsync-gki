if [ "$KSU" != "true" ]; then
  abort "! This module requires KernelSU"
fi

MODDIR=$MODPATH

if [ -f "$MODDIR/gki_ntsync.ko" ]; then
  set_perm "$MODDIR/gki_ntsync.ko" 0 0 0644
  rm -rf "$MODDIR/payload"
else
  if [ "$ARCH" != "arm64" ]; then
    abort "! Only arm64 GKI devices are supported"
  fi

  . "$MODDIR/select-kmi.sh"

  TARGET=$(select_kmi "$(uname -r)") || abort "! Unsupported GKI KMI: $(uname -r)"
  PAYLOAD="$MODDIR/payload/$TARGET/gki_ntsync.ko.gz"

  [ -f "$PAYLOAD" ] || abort "! Missing module payload for $TARGET"
  gzip -dc "$PAYLOAD" > "$MODDIR/gki_ntsync.ko" || abort "! Failed to unpack gki_ntsync.ko"

  set_perm "$MODDIR/gki_ntsync.ko" 0 0 0644
  rm -rf "$MODDIR/payload"
fi
