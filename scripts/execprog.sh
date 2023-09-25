#!/vendor/bin/sh

exec > /dev/kmsg 2>&1

echo "execprog: sh execution"

BIND=/vendor/bin/init.kernel.post_boot-cape.sh

echo "execprog: restarting under tmpfs"

# Run under a new tmpfs to avoid /dev selabel
mkdir /dev/ep
mount -t tmpfs nodev /dev/ep

while [ ! -e "$BIND" ]; do
  if [ -e /tmp/recovery.log ]; then
    echo "execprog: recovery detected"
    # Re-enable SELinux
    echo "97" > /sys/fs/selinux/enforce
    exit
  fi
  sleep 0.1
done

# Setup /dev/ep/execprog
cat "$BIND" > /dev/ep/execprog
echo '
# Limit foreground cpus
echo 0-6 > /dev/cpuset/foreground/cpus

# Setup readahead
find /sys/devices -name read_ahead_kb | while read node; do echo 128 > $node; done

# Setup zram
echo 90 > /proc/sys/vm/swappiness
echo lz4 > /sys/block/zram0/comp_algorithm

# Remove unused swapfile
rm -rf /data/vendor/swap

# Setup SSG
echo 25 > /dev/blkio/background/blkio.ssg.max_available_ratio
' >> /dev/ep/execprog

rm /dev/execprog
chown root:shell /dev/ep/execprog

mount --bind /dev/ep/execprog "$BIND"
chcon "u:object_r:vendor_file:s0" "$BIND"

# lazy unmount /dev/ep for invisibility
umount -l /dev/ep

# Re-enable SELinux
echo "97" > /sys/fs/selinux/enforce
