#!/usr/bin/env bash
#
# Fetches and builds ttyplot (github.com/tenox7/ttyplot) from source into ./ttyplot/.
# Not packaged for this OS (Ubuntu 22.04/jammy, arm64) via apt; this builds the single-file
# C source directly. The cloned repo and built binary are git-ignored — re-run after a fresh
# checkout or to pick up upstream changes.
#
set -euo pipefail

REPO_URL="https://github.com/tenox7/ttyplot.git"
# Built into the REPO ROOT, not next to this script: benchmark.sh looks for <repo>/ttyplot/ttyplot
# and the Dockerfiles expect /workspace/ttyplot. This script lives in admin/local_scripts/, hence
# the two levels up. Kept git-ignored (see .gitignore).
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEST_DIR="$REPO_ROOT/ttyplot"

echo "== installing build dependencies (ncurses + pkg-config) =="
sudo apt-get install -y libncurses-dev pkg-config

if [ -d "$DEST_DIR/.git" ]; then
  echo "== updating existing checkout at $DEST_DIR =="
  git -C "$DEST_DIR" pull --ff-only
else
  echo "== cloning $REPO_URL into $DEST_DIR =="
  git clone "$REPO_URL" "$DEST_DIR"
fi

echo "== building =="
make -C "$DEST_DIR"

echo
echo "== done: $DEST_DIR/ttyplot =="
"$DEST_DIR/ttyplot" --help 2>&1 | head -1 || true
