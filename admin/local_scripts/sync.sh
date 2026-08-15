#!/usr/bin/env bash
#
# Target-side: clone or update the tutorial repo in the tutorial user's home.
#
# fleet.py pipes this over stdin (`ssh <host> bash -s -- <args>`), so it MUST stay
# self-contained: it cannot source anything else from the repo, because on a fresh machine this
# script is what creates the repo in the first place. Keep it dependency-free.
#
# It is also meant to be runnable by hand — copy it to a machine the fleet tool cannot reach and
# run it there, and you get the same result.
#
# Safety: the default path refuses to touch a tree that has local modifications or sits on the
# wrong branch, because those trees are usually somebody's exercise work. --force discards them.
#
# Machine-readable results are printed as '@@key=value' lines; everything else is log output.
#
set -euo pipefail

REPO_URL=""
BRANCH="main"
TUSER="s26t"
DEST=""
FORCE=0

while [ $# -gt 0 ]; do
	case "$1" in
		--repo)   REPO_URL="$2"; shift 2 ;;
		--branch) BRANCH="$2";   shift 2 ;;
		--user)   TUSER="$2";    shift 2 ;;
		--dest)   DEST="$2";     shift 2 ;;
		--force)  FORCE=1;       shift ;;
		*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

DEST="${DEST:-/home/$TUSER/$(basename "${REPO_URL%.git}")}"

die()  { echo "ERROR: $*" >&2; exit 1; }
emit() { printf '@@%s=%s\n' "$1" "$2"; }

[ -n "$REPO_URL" ] || die "--repo is required"
command -v git >/dev/null 2>&1 || die "git is not installed"
id -u "$TUSER" >/dev/null 2>&1 || die "user '$TUSER' does not exist (run the 'user' verb first)"

# Own the checkout as the tutorial user: participants have to edit and rebuild in this tree, so a
# root-owned or admin-owned checkout would break the exercise. When we are already logged in as
# that user there is nothing to elevate, which is the normal case.
if [ "$(id -un)" = "$TUSER" ]; then
	as_tuser() { "$@"; }
else
	sudo -n true 2>/dev/null || die "passwordless sudo needed to act as '$TUSER' (logged in as $(id -un))"
	as_tuser() { sudo -n -u "$TUSER" -H "$@"; }
fi

git_t() { as_tuser git -C "$DEST" "$@"; }

if [ ! -e "$DEST" ]; then
	echo "cloning $REPO_URL ($BRANCH) -> $DEST"
	as_tuser git clone --branch "$BRANCH" "$REPO_URL" "$DEST"
	action="cloned"
else
	[ -d "$DEST/.git" ] || die "$DEST exists but is not a git repository"

	remote=$(git_t remote get-url origin 2>/dev/null || echo "")
	if [ "$remote" != "$REPO_URL" ]; then
		echo "warning: origin is '$remote', expected '$REPO_URL'" >&2
	fi

	echo "fetching $BRANCH in $DEST"
	git_t fetch --prune origin

	before=$(git_t rev-parse HEAD)

	if [ "$FORCE" -eq 1 ]; then
		# Deliberately no `git clean -fdx`: build/ is gitignored, and wiping it would force a
		# full meson rebuild on every machine for no reason. Untracked files that are not in the
		# incoming commit therefore survive --force; only ones it would overwrite are replaced.
		#
		# --force on the checkout is what makes this work at all. A plain `checkout -B` refuses to
		# run when the working tree has modifications, or when an untracked file sits where the
		# target commit has one — which is exactly the tree --force exists to rescue — and with
		# `set -e` it aborts the script before the `reset --hard` below can clean anything up.
		#
		# The checkout must also come FIRST. Resetting before switching would point whatever
		# branch HEAD currently happens to be on at origin/BRANCH, and on a machine where somebody
		# is working on their own branch that silently throws their commits away. `checkout -B`
		# moves only BRANCH and leaves every other branch where it is.
		git_t checkout --force -B "$BRANCH" --track "origin/$BRANCH"
		git_t reset --hard "origin/$BRANCH"
		action="reset"
	else
		dirty=$(git_t status --porcelain)
		if [ -n "$dirty" ]; then
			echo "$dirty" >&2
			die "working tree has local modifications; re-run with --force to discard them"
		fi

		branch=$(git_t rev-parse --abbrev-ref HEAD)
		[ "$branch" = "$BRANCH" ] || die "on branch '$branch', expected '$BRANCH'; re-run with --force"

		git_t merge --ff-only "origin/$BRANCH"
		after=$(git_t rev-parse HEAD)
		if [ "$before" = "$after" ]; then action="up-to-date"; else action="updated"; fi
	fi
fi

# Keep nested exercise repositories usable after both a clone and an update. The
# tutorial's submodules are public HTTPS repositories, so this needs no deploy key.
echo "initializing submodules"
git_t submodule sync --recursive
git_t submodule update --init --recursive

emit action  "$action"
emit sha     "$(git_t rev-parse --short HEAD)"
emit branch  "$(git_t rev-parse --abbrev-ref HEAD)"
emit dest    "$DEST"
emit subject "$(git_t log -1 --format=%s)"
