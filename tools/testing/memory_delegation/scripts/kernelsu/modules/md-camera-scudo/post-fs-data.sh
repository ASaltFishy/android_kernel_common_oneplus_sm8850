#!/system/bin/sh
MODDIR=${0%/*}
LOG=/data/local/tmp/md_camera_scudo_module.log
WRAP_TARGET=/system/bin/bugreportz
SCUDO_TARGET=/system/lib64/libaconfig_test_rust_library.dylib.so

mkdir -p /data/local/tmp/md
{
  echo "post-fs-data start $(date)"
  echo "MODDIR=$MODDIR"
  echo "WRAP_TARGET=$WRAP_TARGET"
  echo "SCUDO_TARGET=$SCUDO_TARGET"
} >> "$LOG" 2>&1

chcon u:object_r:system_file:s0 "$MODDIR/system/bin/md_camera_scudo_wrap.sh" 2>/dev/null || true
chcon u:object_r:system_file:s0 "$MODDIR/system/lib64/libscudo_md_android.so" 2>/dev/null || true
umount "$WRAP_TARGET" 2>/dev/null || true
umount "$SCUDO_TARGET" 2>/dev/null || true
mount --bind "$MODDIR/system/bin/md_camera_scudo_wrap.sh" "$WRAP_TARGET" >> "$LOG" 2>&1 || true
mount --bind "$MODDIR/system/lib64/libscudo_md_android.so" "$SCUDO_TARGET" >> "$LOG" 2>&1 || true

# This property is consumed by zygote when starting com.oplus.camera.
setprop wrap.com.oplus.camera "$WRAP_TARGET"
echo "wrap.com.oplus.camera=$(getprop wrap.com.oplus.camera)" >> "$LOG" 2>&1
mount | grep -E "$WRAP_TARGET|$SCUDO_TARGET" >> "$LOG" 2>&1 || true
