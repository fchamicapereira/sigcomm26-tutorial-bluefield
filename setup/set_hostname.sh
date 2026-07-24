#!/usr/bin/env bash
#
# Set the BlueField-3 DPU's hostname persistently, for the SIGCOMM tutorial.
#
# Does three things that all need to happen together, or the change won't stick and `sudo`
# will nag on every invocation:
#   1. Sets a real *static* hostname via hostnamectl (these DPUs ship with only a transient
#      cloud-init hostname and no /etc/hostname).
#   2. Updates /etc/hosts so the new name resolves to loopback -> this is what stops
#      `sudo: unable to resolve host <name>: Name or service not known`.
#   3. Tells cloud-init to preserve the hostname, so it isn't reverted on the next boot.
#
# Idempotent and safe to re-run. Backs up /etc/hosts before touching it.
#
# The Tailscale node name is derived from this hostname at `tailscale up` time, so set it
# before bringing Tailscale up. See README.md.
#
# Usage: ./setup/set_hostname.sh <new-hostname>

set -euo pipefail

# --- arguments ---------------------------------------------------------------------------------
if [ "$#" -ne 1 ] || [ -z "${1:-}" ]; then
  echo "Usage: $0 <new-hostname>" >&2
  echo "  Sets the DPU's static hostname, fixes /etc/hosts so sudo stops complaining, and" >&2
  echo "  tells cloud-init to keep it across reboots." >&2
  exit 1
fi

NEW_HOSTNAME="$1"

# --- validate: RFC 1123 hostname (single label or dotted), each label <=63 chars ---------------
if [ "${#NEW_HOSTNAME}" -gt 253 ] || \
   ! [[ "$NEW_HOSTNAME" =~ ^[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?(\.[a-zA-Z0-9]([a-zA-Z0-9-]{0,61}[a-zA-Z0-9])?)*$ ]]; then
  echo "ERROR: '$NEW_HOSTNAME' is not a valid hostname." >&2
  echo "       Use letters, digits and hyphens (labels 1-63 chars, not starting/ending with '-')." >&2
  exit 1
fi

OLD_HOSTNAME="$(hostname)"
HOSTS=/etc/hosts

echo "== changing hostname: '${OLD_HOSTNAME}' -> '${NEW_HOSTNAME}' =="

# --- 1. static hostname ------------------------------------------------------------------------
echo "== setting static hostname (hostnamectl) =="
sudo hostnamectl set-hostname "$NEW_HOSTNAME"

# --- 2. /etc/hosts so the new name resolves (this silences sudo) -------------------------------
BACKUP="${HOSTS}.set_hostname.$(date +%Y%m%d-%H%M%S).bak"
echo "== updating ${HOSTS} (backup: ${BACKUP}) =="
sudo cp -a "$HOSTS" "$BACKUP"

# Drop any 127.0.1.1 mapping (we manage this line); '\b' keeps 127.0.1.10 etc. safe.
sudo sed -i '/^[[:space:]]*127\.0\.1\.1\b/d' "$HOSTS"

# Remove the previous hostname token wherever it was appended (e.g. on the 127.0.0.1 line),
# so we don't leave a stale mapping. Escape '.' in the old name for the regex.
if [ -n "$OLD_HOSTNAME" ] && [ "$OLD_HOSTNAME" != "$NEW_HOSTNAME" ]; then
  OLD_ESC="${OLD_HOSTNAME//./\\.}"
  sudo sed -i "s/[[:space:]]\{1,\}${OLD_ESC}\b//g" "$HOSTS"
fi

# Add the canonical mapping (Debian/Ubuntu convention: the hostname lives on 127.0.1.1).
printf '127.0.1.1\t%s\n' "$NEW_HOSTNAME" | sudo tee -a "$HOSTS" >/dev/null

# --- 3. keep cloud-init from reverting it on reboot -------------------------------------------
CLOUD_DROPIN=/etc/cloud/cloud.cfg.d/99-preserve-hostname.cfg
if [ -d /etc/cloud/cloud.cfg.d ]; then
  echo "== telling cloud-init to preserve the hostname (${CLOUD_DROPIN}) =="
  printf 'preserve_hostname: true\n' | sudo tee "$CLOUD_DROPIN" >/dev/null
else
  echo "== cloud-init not present; skipping preserve_hostname drop-in =="
fi

# --- verify -----------------------------------------------------------------------------------
echo "== verification =="
echo "static hostname : $(hostnamectl --static status 2>/dev/null || hostname)"
if getent hosts "$NEW_HOSTNAME" >/dev/null; then
  echo "resolves        : yes ($(getent hosts "$NEW_HOSTNAME" | awk '{print $1}' | paste -sd,)) -> sudo will be quiet"
else
  echo "resolves        : NO -- sudo may still complain; check ${HOSTS}" >&2
fi

echo "== done. Open a new shell to pick up the new prompt. =="
