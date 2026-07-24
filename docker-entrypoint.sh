#!/usr/bin/env bash
#
# Container entrypoint: bring up the RoCE loopback (hugepages + ns0/ns1 + SF placement) by
# running setup/setup_roce_loopback.sh, then hand off to the container command (default: an
# interactive shell). Runs as the 'ubuntu' user; the setup script elevates via passwordless sudo.
#
# Skip it with `-e SKIP_ROCE_SETUP=1` (e.g. when you only want to build). If setup fails — the
# SFs are held by another running container, or this isn't the tutorial DPU — it warns and still
# drops you to the shell so you can investigate.
#
set -u

if [ "${SKIP_ROCE_SETUP:-0}" != "1" ]; then
  echo ">> Running setup/setup_roce_loopback.sh (set SKIP_ROCE_SETUP=1 to skip) ..."
  if ! /workspace/setup/setup_roce_loopback.sh; then
    echo "WARNING: setup_roce_loopback.sh failed — continuing to the shell so you can debug." >&2
  fi
fi

exec "$@"
