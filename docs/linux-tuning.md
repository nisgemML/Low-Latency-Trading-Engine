# Linux Tuning for Sub-Microsecond Latency

A checklist of kernel and system settings that take a system from
"fast in a benchmark" to "fast in production." Applied to a dedicated
co-location server running this engine.

---

## CPU Isolation

### Boot parameters (`/etc/default/grub`)

```
GRUB_CMDLINE_LINUX="isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3
                    nosoftlockup tsc=reliable"
```

- `isolcpus=2,3` — Remove CPUs 2 and 3 from the general scheduler pool.
  No OS tasks, no kernel threads, no interrupts will be scheduled here.
- `nohz_full=2,3` — Disable the timer tick on isolated cores (no 1 ms
  timer interrupt to disrupt the spinning matching thread).
- `rcu_nocbs=2,3` — Move RCU callbacks off isolated cores.
- `tsc=reliable` — Disable TSC calibration at boot (reduces jitter on VMs
  and some cloud instances).

Apply with: `sudo update-grub && sudo reboot`

### Verify isolation

```bash
# Should show no tasks scheduled on CPU 2 outside your process:
watch -n1 'ps -eLo psr,pid,comm | grep "^ 2"'
```

---

## Interrupt Affinity

Move all interrupts off the matching cores.

```bash
# Move all IRQs to CPU 0 (leave matching cores clean).
for irq in /proc/irq/*/smp_affinity; do
    echo 1 > "$irq" 2>/dev/null || true
done

# Verify:
cat /proc/interrupts | head -20
```

For the NIC (DPDK use case), set the NIC queue affinity explicitly:
```bash
# Example: set NIC queue 0 interrupt to CPU 0, queue 1 to CPU 1.
ethtool -L eth0 combined 2
echo 1 > /proc/irq/$(cat /sys/class/net/eth0/device/msi_irqs/128)/smp_affinity
```

---

## Memory

### Huge pages

```bash
# Reserve 1 GB huge pages at boot (add to GRUB_CMDLINE_LINUX):
hugepages=512 hugepagesz=2M

# Or at runtime:
echo 512 > /proc/sys/vm/nr_hugepages

# Verify:
grep -i huge /proc/meminfo
```

The pool allocator uses `madvise(MADV_HUGEPAGE)` to request transparent huge
pages, but explicit reservation is more reliable.

### NUMA

If the server has multiple NUMA nodes, ensure the engine runs on the same node
as the NIC:

```bash
# Check NIC NUMA node:
cat /sys/class/net/eth0/device/numa_node

# Launch engine on the right NUMA node:
numactl --cpunodebind=0 --membind=0 ./trading_engine
```

Cross-NUMA memory access adds ~80–100 ns per access — enough to blow the p99
budget.

---

## Scheduler

```bash
# Enable SCHED_FIFO for the matching thread (the engine does this internally,
# but you need CAP_SYS_NICE or root):
sudo setcap cap_sys_nice+ep ./trading_engine

# Or run as root (not recommended for production):
sudo ./trading_engine
```

### Disable CPU frequency scaling

```bash
# Set performance governor on all cores:
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$cpu"
done

# Disable turbo boost (reduces frequency jitter at the cost of peak clock):
echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
```

Turbo boost allows CPUs to briefly exceed their base clock, but the frequency
transition causes jitter (~1–5 µs). For consistent p99 latency, a stable
frequency beats a higher but variable one.

### Disable CPU C-states

```bash
# Force C0 (active) state — no sleep states on the matching core.
# Add to GRUB_CMDLINE_LINUX:
intel_idle.max_cstate=0 processor.max_cstate=0

# Or at runtime:
cpupower idle-set -D 0
```

C-state transitions (the CPU going into sleep to save power) take 10–100 µs
to exit. With `nohz_full`, the OS won't trigger sleep states on isolated cores,
but disabling them explicitly is belt-and-suspenders.

---

## Network (DPDK / Kernel Bypass)

For kernel bypass UDP (market data feed):

```bash
# Bind NIC to DPDK's VFIO driver:
sudo modprobe vfio-pci
sudo dpdk-devbind.py --bind=vfio-pci 0000:01:00.0

# Reserve hugepages for DPDK:
echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# Run DPDK app on isolated core:
./dpdk_receiver --lcores 2 --socket-mem 512
```

With kernel bypass, the NIC DMA's packet data directly into a hugepage ring
buffer. The application polls the ring without any system calls. This eliminates
the ~5–15 µs kernel receive path and is the primary source of latency reduction
in production co-located systems.

---

## Kernel Parameters (`/etc/sysctl.conf`)

```ini
# Disable NUMA balancing (can cause unexpected page migrations):
kernel.numa_balancing = 0

# Reduce memory compaction (can cause latency spikes):
vm.compaction_proactiveness = 0

# Disable transparent huge page defragmentation:
# (echo defer+madvise > /sys/kernel/mm/transparent_hugepage/defrag)

# Increase perf event paranoia for profiling:
kernel.perf_event_paranoid = -1

# Disable watchdog on isolated cores:
kernel.watchdog_cpumask = 0x1  # only CPU 0
```

Apply with: `sudo sysctl -p`

---

## Verification

After applying all settings, verify the system is configured correctly:

```bash
# Check isolation:
cat /sys/devices/system/cpu/isolated

# Check frequency governor:
cat /sys/devices/system/cpu/cpu2/cpufreq/scaling_governor

# Check C-states:
cpupower idle-info

# Check NUMA:
numactl --hardware

# Measure timer resolution:
cyclictest --mlockall -t1 -p99 -n -i200 -l10000 -q --cpu=2
# Target: max latency < 10 µs with the above settings applied.
```

---

## Expected Impact

| Setting                  | p50 impact | p99 impact |
|--------------------------|-----------|-----------|
| `isolcpus`               | +0 ns      | −50 µs     |
| `nohz_full`              | +0 ns      | −1 ms      |
| No turbo boost           | +20 ns     | −5 µs      |
| C-states disabled        | +0 ns      | −100 µs    |
| Huge pages               | −5 ns      | −10 µs     |
| NUMA locality            | −10 ns     | −20 µs     |
| DPDK (vs kernel UDP)     | −3 µs      | −15 µs     |

The p50 changes little — the matching engine is already cache-warm and
computing efficiently. The p99 improvements are dramatic because they
eliminate the tail events (preemption, timer interrupt, page fault, NUMA
miss) that dominate latency at the 99th percentile.
