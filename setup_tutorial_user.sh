#!/usr/bin/env bash
#
# Creates the shared tutorial login used by SIGCOMM'26 attendees on this box.
#
#   user     s26t
#   password sigcomm26tutorial
#   groups   sudo (passwordless), docker
#
# The credentials are deliberately public — this is a throwaway workshop account on a lab
# machine, and every attendee needs the same login. Do NOT run this on a box that matters.
# Override with USERNAME=... PASSWORD=... if you need something else.
#
# Idempotent: re-running resets the password and re-applies the group membership and the
# sudoers drop-in, so it is safe to run before each session.
#
set -euo pipefail

USERNAME="${USERNAME:-s26t}"
PASSWORD="${PASSWORD:-sigcomm26tutorial}"
SUDOERS_FILE="/etc/sudoers.d/90-${USERNAME}-nopasswd"

if [ "$(id -u)" -ne 0 ]; then SUDO=sudo; else SUDO=""; fi

echo "== creating user $USERNAME =="
if id -u "$USERNAME" >/dev/null 2>&1; then
  echo "user already exists, leaving home directory and shell as-is"
else
  $SUDO useradd --create-home --shell /bin/bash "$USERNAME"
fi

echo "== setting password =="
echo "${USERNAME}:${PASSWORD}" | $SUDO chpasswd

echo "== adding to groups: sudo, docker =="
for grp in sudo docker; do
  if getent group "$grp" >/dev/null; then
    $SUDO usermod --append --groups "$grp" "$USERNAME"
  else
    echo "  warning: group '$grp' does not exist (is docker installed?), skipping"
  fi
done

echo "== granting passwordless sudo via $SUDOERS_FILE =="
# Write to a temp file and validate with visudo before installing: a malformed sudoers
# drop-in breaks sudo for *everyone* on the box, including the account fixing it.
TMP_SUDOERS="$(mktemp)"
trap 'rm -f "$TMP_SUDOERS"' EXIT
printf '%s ALL=(ALL) NOPASSWD:ALL\n' "$USERNAME" > "$TMP_SUDOERS"
$SUDO visudo --check --file="$TMP_SUDOERS"
$SUDO install -o root -g root -m 0440 "$TMP_SUDOERS" "$SUDOERS_FILE"

echo
echo "== done =="
echo "user:     $USERNAME"
echo "password: $PASSWORD"
echo "groups:   $(id -nG "$USERNAME")"
echo "sudo:     passwordless ($SUDOERS_FILE)"
echo
echo "verify with:  su - $USERNAME -c 'sudo -n true && docker ps'"
