#!/bin/sh
# Start a one-node Slurm cluster on this machine (CI and local trials of
# mregress -farm slurm). Needs root and the slurm-wlm and munge packages.
# Ends with the node idle, or exits 1.
set -e
host=$(hostname -s)
cpus=$(nproc)
mem=$(awk '/MemTotal/ {print int($2 / 1024 * 0.8)}' /proc/meminfo)
mkdir -p /etc/slurm /var/spool/slurmctld /var/spool/slurmd /var/log/slurm /run/munge /var/log/munge /var/lib/munge
[ -s /etc/munge/munge.key ] || mungekey -c 2>/dev/null || dd if=/dev/urandom of=/etc/munge/munge.key bs=1024 count=1 2>/dev/null
chown -R munge:munge /etc/munge /run/munge /var/log/munge /var/lib/munge
chmod 400 /etc/munge/munge.key
pgrep -x munged >/dev/null || runuser -u munge -- munged --force || munged --force
cat > /etc/slurm/slurm.conf <<CONF
ClusterName=minisim-ci
SlurmctldHost=$host
SlurmUser=root
SlurmdUser=root
AuthType=auth/munge
StateSaveLocation=/var/spool/slurmctld
SlurmdSpoolDir=/var/spool/slurmd
SlurmctldLogFile=/var/log/slurm/slurmctld.log
SlurmdLogFile=/var/log/slurm/slurmd.log
ProctrackType=proctrack/linuxproc
TaskPlugin=task/none
SchedulerType=sched/backfill
SelectType=select/cons_tres
SelectTypeParameters=CR_Core
ReturnToService=2
MpiDefault=none
JobAcctGatherType=jobacct_gather/none
NodeName=$host CPUs=$cpus RealMemory=$mem State=UNKNOWN
PartitionName=debug Nodes=$host Default=YES MaxTime=INFINITE State=UP
CONF
rm -f /etc/slurm/cgroup.conf
pgrep -x slurmctld >/dev/null || slurmctld
pgrep -x slurmd >/dev/null || slurmd
for i in $(seq 30); do
  if sinfo -h -o %T 2>/dev/null | grep -qx idle; then
    sinfo
    exit 0
  fi
  sleep 1
done
echo "slurm_up: the node did not come up" >&2
tail -20 /var/log/slurm/slurmctld.log /var/log/slurm/slurmd.log >&2 || true
exit 1
