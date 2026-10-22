#!/bin/sh
# Every config's first step when its container is created. deps/ is a Docker
# volume, so a rebuilt container keeps the toolchains it fetched; Docker makes
# a new volume root's, and build_mp.py writes to it as you.
set -eu
cd "$(dirname "$0")/.."
sudo chown "$(id -u):$(id -g)" deps
