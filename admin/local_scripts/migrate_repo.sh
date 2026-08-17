#!/usr/bin/env bash
# Move an existing tutorial checkout from the tutorial user's home into /opt.
set -euo pipefail

TUSER="s26t"
SOURCE="/home/s26t/sigcomm26-tutorial-bluefield"
DEST="/opt/sigcomm26-tutorial-bluefield"

while [ $# -gt 0 ]; do
	case "$1" in
		--user)   TUSER="$2"; shift 2 ;;
		--source) SOURCE="$2"; shift 2 ;;
		--dest)   DEST="$2"; shift 2 ;;
		*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

die()  { echo "ERROR: $*" >&2; exit 1; }
emit() { printf '@@%s=%s\n' "$1" "$2"; }

id -u "$TUSER" >/dev/null 2>&1 || die "user '$TUSER' does not exist (run the 'user' verb first)"

if [ -e "$DEST" ] || [ -L "$DEST" ]; then
	[ -d "$DEST/.git" ] || die "$DEST already exists but is not a git checkout; left both paths untouched"
	[ ! -e "$SOURCE" ] && [ ! -L "$SOURCE" ] || die "both $SOURCE and $DEST exist; left both paths untouched"
	emit action "already-migrated"
	emit dest "$DEST"
	exit 0
fi

[ -d "$SOURCE/.git" ] || die "source checkout not found at $SOURCE (run 'sync' to create a fresh checkout at $DEST)"
[ ! -L "$SOURCE" ] || die "$SOURCE is a symlink; migrate its real target explicitly instead"
[ -d "$(dirname "$DEST")" ] || die "destination parent $(dirname "$DEST") does not exist"

if [ "$(id -u)" -eq 0 ]; then
	mv -- "$SOURCE" "$DEST"
	chown -R "$TUSER:$TUSER" "$DEST"
else
	sudo -n true 2>/dev/null || die "passwordless sudo is needed to move the checkout into /opt"
	sudo -n mv -- "$SOURCE" "$DEST"
	sudo -n chown -R "$TUSER:$TUSER" "$DEST"
fi

emit action "moved"
emit dest "$DEST"
