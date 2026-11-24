#!/bin/sh
# Turns off boost and sets every CPU's energy preference to performance, so that
# benchmark times don't vary with temperature. Also lets perf read the hardware
# counters, and frees the counter that the NMI watchdog uses, so that perf stat
# doesn't have to multiplex them. Needs root. cpu_normal_clocks.sh or a reboot
# restores the defaults.
#
# To run both without a password, install root-owned copies and allow only
# those in sudoers. sudo must not point at the copies in the repo, since
# anything running as the user could edit them:
#
#   sudo install -o root -g root -m 755 bench/cpu_fixed_clocks.sh /usr/local/sbin/
#   sudo install -o root -g root -m 755 bench/cpu_normal_clocks.sh /usr/local/sbin/
#   echo "$USER ALL=(root) NOPASSWD: /usr/local/sbin/cpu_fixed_clocks.sh, /usr/local/sbin/cpu_normal_clocks.sh" |
#       sudo tee /etc/sudoers.d/luvr-bench
#   sudo chmod 440 /etc/sudoers.d/luvr-bench
#   sudo visudo -c
#
# Then `sudo cpu_fixed_clocks.sh` doesn't ask for a password.
set -e
# Ubuntu defaults to 4, which doesn't allow perf at all without root.
echo 1 > /proc/sys/kernel/perf_event_paranoid
echo 0 > /proc/sys/kernel/nmi_watchdog
echo 0 > /sys/devices/system/cpu/cpufreq/boost
for f in /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference; do
    echo performance > "$f"
done
echo "boost $(cat /sys/devices/system/cpu/cpufreq/boost)," \
    "energy preference $(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference)," \
    "perf_event_paranoid $(cat /proc/sys/kernel/perf_event_paranoid)," \
    "nmi_watchdog $(cat /proc/sys/kernel/nmi_watchdog)"
