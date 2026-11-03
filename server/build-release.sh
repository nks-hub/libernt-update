#!/bin/sh
# Builds LiberNT.img from the nks branch of nks-hub/LiberNT and publishes it to the update feed.
# luup.py skips the release when the image carries no changed files.
set -eu
SRC=${SRC:-/root/LiberNT}
OUT=${OUT:-$SRC/output-Clang-amd64-debug}
FEED=${FEED:-/srv/luup}
KEY=${KEY:-/opt/luup/signing.key}
LUUP=${LUUP:-/opt/luup/luup.py}

cd "$SRC"
git fetch -q origin nks
git checkout -q -B nks origin/nks
head=$(git rev-parse HEAD)
if python3 -c 'import json,sys; sys.exit(0 if any(r["commit"] == sys.argv[2] for r in json.load(open(sys.argv[1]))["releases"]) else 1)' \
        "$FEED/feed.json" "$head"; then
    echo "commit $head is already published"
    exit 0
fi
echo "building $head"
ninja -C "$OUT" -j"$(nproc)" liberntimg
python3 "$LUUP" publish --key "$KEY" --feed "$FEED" --img "$OUT/LiberNT.img" --commit "$head" --repo "$SRC"
python3 "$LUUP" verify --feed "$FEED" --pub "$(cat "${KEY%.key}.pub")"
