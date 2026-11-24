#!/bin/sh
# Undoes cpu_fixed_clocks.sh: turns boost and the NMI watchdog back on, and sets
# every CPU's energy preference back to balance_performance, the default for
# amd-pstate-epp. perf_event_paranoid is left at 1, so that perf still works
# outside of benchmark runs. Needs root. See cpu_fixed_clocks.sh for running it
# without a password.
set -e
echo 1 > /proc/sys/kernel/nmi_watchdog
echo 1 > /sys/devices/system/cpu/cpufreq/boost
for f in /sys/devices/system/cpu/cpu*/cpufreq/energy_performance_preference; do
    echo balance_performance > "$f"
done
echo "boost $(cat /sys/devices/system/cpu/cpufreq/boost)," \
    "energy preference $(cat /sys/devices/system/cpu/cpu0/cpufreq/energy_performance_preference)," \
    "nmi_watchdog $(cat /proc/sys/kernel/nmi_watchdog)"
