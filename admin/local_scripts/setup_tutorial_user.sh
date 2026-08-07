#!/usr/bin/env bash
#
# Bring this box's shared tutorial login to the state the tutorial expects:
#
#   user     s26t
#   password sigcomm26tutorial
#   shell    /bin/bash, with a home directory it owns
#   groups   sudo, docker
#   sudo     passwordless, via /etc/sudoers.d/90-s26t-nopasswd
#
# The credentials are deliberately public — this is a throwaway workshop account on a lab
# machine, and every attendee needs the same login. Do NOT run this on a box that matters.
# Override with USERNAME=... PASSWORD=... if you need something else.
#
# This is a RECONCILER, not a creator: it inspects each property above, reports which are already
# correct, and changes only what is not. That matters because the account is often created by
# hand — by whoever owns the machine, before this script existed — and then differs in exactly the
# ways that break things later: no sudo group, no sudoers drop-in, a shell of /usr/sbin/nologin.
# Everything admin/fleet.py does over ssh assumes passwordless sudo, so a hand-made account fails
# every verb with "passwordless sudo is required" and nothing else explains why.
#
# Needs root. If the tutorial account itself cannot sudo on this machine — the very case this
# script exists to repair — then it cannot bootstrap itself, and whoever owns the box has to run
# it (or grant sudo once, by hand).
#
# Machine-readable results are printed as '@@key=value' lines; everything else is log output.
#
# Usage:
#   sudo ./setup_tutorial_user.sh            # report and fix
#   sudo ./setup_tutorial_user.sh --check    # report only, change nothing
#   sudo ./setup_tutorial_user.sh --emit     # '@@key=value' lines for admin/fleet.py
#
set -euo pipefail

usage() {
	cat <<'EOF'
Usage: setup_tutorial_user.sh [--check] [--emit]

Reconcile the shared tutorial account (default s26t) with what the tutorial expects: password,
shell, home, sudo+docker groups and a passwordless-sudo drop-in. Reports what was already correct
and changes only what was not.

      --check   report what differs and change nothing
      --emit    print '@@key=value' lines instead of the human report, for admin/fleet.py
EOF
}

CHECK=0
EMIT=0
for arg in "$@"; do
	case "$arg" in
		--check)   CHECK=1 ;;
		--emit)    EMIT=1 ;;
		-h|--help) usage; exit 0 ;;
		*)         echo "unknown argument: $arg" >&2; usage >&2; exit 2 ;;
	esac
done

USERNAME="${USERNAME:-s26t}"
PASSWORD="${PASSWORD:-sigcomm26tutorial}"
SHELL_WANTED="${SHELL_WANTED:-/bin/bash}"
SUDOERS_FILE="/etc/sudoers.d/90-${USERNAME}-nopasswd"
SUDOERS_LINE="${USERNAME} ALL=(ALL) NOPASSWD:ALL"

emit() { printf '@@%s=%s\n' "$1" "$2"; }
die()  { echo "ERROR: $*" >&2; exit 1; }
say()  { [ "$EMIT" -eq 1 ] || echo "$@"; }

# --- privileges ----------------------------------------------------------------------------------
# Only fixing needs root; --check inspects what it can and says so when something is unreadable.
SUDO=()
if [ "$(id -u)" -ne 0 ]; then
	if sudo -n true 2>/dev/null; then
		SUDO=(sudo -n)
	elif [ "$CHECK" -eq 1 ]; then
		say "note: not root and cannot sudo — reporting only what is readable as $(id -un)"
	else
		die "must run as root (this account cannot sudo here; ask whoever owns the box)"
	fi
fi

ok=()       # properties already correct
fixed=()    # properties this run changed
broken=()   # properties still wrong (--check, or unfixable)

# runuser is the clean way to become another user non-interactively; su is the fallback for
# images that do not ship it (its -c takes a single string, hence the "$*").
run_as_user() {
	if command -v runuser >/dev/null 2>&1; then
		"${SUDO[@]}" runuser -u "$USERNAME" -- "$@"
	else
		"${SUDO[@]}" su -s /bin/sh "$USERNAME" -c "$*"
	fi
}

note_ok()     { ok+=("$1"); say "  ok       $1"; }
note_fixed()  { fixed+=("$1"); say "  FIXED    $1"; }
note_broken() { broken+=("$1"); say "  WRONG    $1"; }

say "== reconciling '$USERNAME' on $(hostname) =="

# --- 1. the account ------------------------------------------------------------------------------
if id -u "$USERNAME" >/dev/null 2>&1; then
	note_ok "user exists"
elif [ "$CHECK" -eq 1 ]; then
	note_broken "user does not exist"
else
	"${SUDO[@]}" useradd --create-home --shell "$SHELL_WANTED" "$USERNAME"
	note_fixed "user created"
fi

user_exists=0
if id -u "$USERNAME" >/dev/null 2>&1; then
	user_exists=1
fi

if [ "$user_exists" -eq 1 ]; then
	# --- 2. shell ---------------------------------------------------------------------------------
	# A hand-made account is often /usr/sbin/nologin or /bin/sh, which breaks `ssh <host> bash -s`.
	current_shell=$(getent passwd "$USERNAME" | awk -F: '{print $7}')
	if [ "$current_shell" = "$SHELL_WANTED" ]; then
		note_ok "shell is $SHELL_WANTED"
	elif [ "$CHECK" -eq 1 ]; then
		note_broken "shell is '$current_shell', expected $SHELL_WANTED"
	else
		"${SUDO[@]}" usermod --shell "$SHELL_WANTED" "$USERNAME"
		note_fixed "shell $current_shell -> $SHELL_WANTED"
	fi

	# --- 3. home ----------------------------------------------------------------------------------
	home_dir=$(getent passwd "$USERNAME" | awk -F: '{print $6}')
	if [ -d "$home_dir" ] && [ "$(stat -c '%U' "$home_dir" 2>/dev/null)" = "$USERNAME" ]; then
		note_ok "home $home_dir exists and is owned by $USERNAME"
	elif [ "$CHECK" -eq 1 ]; then
		note_broken "home $home_dir missing or not owned by $USERNAME"
	else
		"${SUDO[@]}" mkdir -p "$home_dir"
		"${SUDO[@]}" chown -R "$USERNAME:$USERNAME" "$home_dir"
		note_fixed "home $home_dir created/reowned"
	fi

	# --- 4. groups --------------------------------------------------------------------------------
	# docker is optional (not every box runs it); sudo is not.
	member_of=$(id -nG "$USERNAME" 2>/dev/null || echo "")
	for grp in sudo docker; do
		if ! getent group "$grp" >/dev/null; then
			say "  -        group '$grp' does not exist on this box, skipping"
			continue
		fi
		case " $member_of " in
			*" $grp "*) note_ok "member of group '$grp'" ;;
			*)
				if [ "$CHECK" -eq 1 ]; then
					note_broken "not in group '$grp'"
				else
					"${SUDO[@]}" usermod --append --groups "$grp" "$USERNAME"
					note_fixed "added to group '$grp'"
				fi
				;;
		esac
	done

	# --- 5. password ------------------------------------------------------------------------------
	# There is no way to verify a password non-interactively, so this one is always (re)applied
	# rather than checked — it is idempotent and the value is published anyway.
	if [ "$CHECK" -eq 1 ]; then
		say "  -        password not checked (cannot be verified; --check never sets it)"
	else
		echo "${USERNAME}:${PASSWORD}" | "${SUDO[@]}" chpasswd
		note_fixed "password set"
	fi
fi

# --- 6. the sudoers drop-in ----------------------------------------------------------------------
sudoers_ok=0
if "${SUDO[@]}" test -f "$SUDOERS_FILE" 2>/dev/null; then
	content=$("${SUDO[@]}" cat "$SUDOERS_FILE" 2>/dev/null || echo "")
	mode=$("${SUDO[@]}" stat -c '%a %U:%G' "$SUDOERS_FILE" 2>/dev/null || echo "")
	if [ "$content" = "$SUDOERS_LINE" ] && [ "$mode" = "440 root:root" ]; then
		sudoers_ok=1
		note_ok "$SUDOERS_FILE present, correct, 0440 root:root"
	elif [ "$CHECK" -eq 1 ]; then
		note_broken "$SUDOERS_FILE exists but content or mode is wrong ($mode)"
	fi
elif [ "$CHECK" -eq 1 ]; then
	note_broken "$SUDOERS_FILE missing"
fi

if [ "$sudoers_ok" -eq 0 ] && [ "$CHECK" -eq 0 ]; then
	# Write to a temp file and validate with visudo before installing: a malformed sudoers
	# drop-in breaks sudo for *everyone* on the box, including the account fixing it.
	TMP_SUDOERS="$(mktemp)"
	trap 'rm -f "$TMP_SUDOERS"' EXIT
	printf '%s\n' "$SUDOERS_LINE" > "$TMP_SUDOERS"
	"${SUDO[@]}" visudo --check --file="$TMP_SUDOERS" >/dev/null \
		|| die "refusing to install: visudo rejected '$SUDOERS_LINE'"
	"${SUDO[@]}" install -o root -g root -m 0440 "$TMP_SUDOERS" "$SUDOERS_FILE"
	note_fixed "installed $SUDOERS_FILE"
fi

# --- 7. does passwordless sudo actually work? ----------------------------------------------------
# The properties above are the ingredients; this is the thing every fleet.py verb depends on, so
# test it for real rather than inferring it from group membership and a file on disk.
nopasswd="unknown"
if [ "$user_exists" -eq 1 ] && { [ "$(id -u)" -eq 0 ] || [ ${#SUDO[@]} -gt 0 ]; }; then
	if run_as_user sudo -n true 2>/dev/null; then
		nopasswd="yes"; note_ok "passwordless sudo works for $USERNAME"
	else
		nopasswd="no"
		if [ "$CHECK" -eq 1 ]; then
			note_broken "passwordless sudo does NOT work for $USERNAME"
		else
			note_broken "passwordless sudo still does not work — check /etc/sudoers for a later rule overriding the drop-in"
		fi
	fi
fi

join() { local IFS=,; if [ $# -eq 0 ]; then echo "-"; else echo "$*"; fi; }

if [ "$EMIT" -eq 1 ]; then
	emit user     "$([ "$user_exists" -eq 1 ] && echo present || echo missing)"
	emit groups   "$(id -nG "$USERNAME" 2>/dev/null | tr ' ' ',' || echo '-')"
	emit nopasswd "$nopasswd"
	emit ok       "${#ok[@]}"
	emit fixed    "$(join "${fixed[@]}")"
	emit broken   "$(join "${broken[@]}")"
	emit subject  "$([ ${#broken[@]} -gt 0 ] && echo "${#broken[@]} still wrong" || { [ ${#fixed[@]} -gt 0 ] && echo "${#fixed[@]} fixed" || echo "already correct"; })"
	if [ ${#broken[@]} -gt 0 ]; then
		exit 1
	fi
	exit 0
fi

echo
echo "already correct: ${#ok[@]}"
if [ ${#fixed[@]} -gt 0 ]; then
	echo "changed:         ${fixed[*]}"
fi
if [ ${#broken[@]} -gt 0 ]; then
	echo "STILL WRONG:     ${broken[*]}"
	echo
	if [ "$CHECK" -eq 1 ]; then
		echo "re-run without --check to fix."
	fi
	exit 1
fi
echo
echo "user:     $USERNAME"
echo "password: $PASSWORD"
echo "groups:   $(id -nG "$USERNAME" 2>/dev/null || echo '-')"
echo "sudo:     passwordless ($SUDOERS_FILE)"
