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
while [ ! -e /dev/block/zram0 ]; do
  sleep 1
done
if ! grep -q zram /proc/swaps; then
  MemStr=$(cat /proc/meminfo | grep MemTotal)
  MemKb=$((${MemStr:16:8}))

  # Try tuning this again
  echo 90 > /proc/sys/vm/swappiness
  echo 0 > /proc/sys/vm/page-cluster
  echo 0 > /proc/sys/vm/watermark_boost_factor
  echo $((200 * 1024 * 1024)) > /proc/sys/vm/dirty_bytes
  echo $((500 * 1024 * 1024)) > /proc/sys/vm/dirty_background_bytes

  # Set swap size to half of MemTotal
  # Align by 4 MiB
  expr $MemKb / 2 '*' 1024 / 4194304 '*' 4194304 > /sys/block/zram0/disksize

  # Use LZ4
  echo lz4 > /sys/block/zram0/comp_algorithm

  mkswap /dev/block/zram0
  swapon /dev/block/zram0 -p 32758
fi

# Remove unused swapfile
rm -rf /data/vendor/swap

# Setup SSG
echo 25 > /dev/blkio/background/blkio.ssg.max_available_ratio
' >> /dev/ep/execprog

# Tune VM
echo 90 > /proc/sys/vm/swappiness
echo 0 > /proc/sys/vm/page-cluster
echo 0 > /proc/sys/vm/watermark_boost_factor
echo $((200 * 1024 * 1024)) > /proc/sys/vm/dirty_bytes
echo $((500 * 1024 * 1024)) > /proc/sys/vm/dirty_background_bytes

# Remove unused swapfile
rm -rf /data/vendor/swap

# Create empty file and bind to break zramwriteback
touch /dev/ep/empty
mount --bind /dev/ep/empty /vendor/bin/nt_gen_zramwriteback_fstab
mount --bind /dev/ep/empty /vendor/etc/first_zramwriteback.fstab

rm /dev/execprog
chown root:shell /dev/ep/execprog

mount --bind /dev/ep/execprog "$BIND"
chcon "u:object_r:vendor_file:s0" "$BIND"

# lazy unmount /dev/ep for invisibility
umount -l /dev/ep

# Re-enable SELinux
echo "97" > /sys/fs/selinux/enforce
