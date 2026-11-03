#!/bin/sh
# Builds LiberNT.img from the nks branch of nks-hub/LiberNT, publishes it to the update feed and, when FEED_PUSH
# is set (rsync destination such as user@host:), copies the feed to the update server.
# luup.py skips the release when the image carries no changed files.
set -eu
SRC=${SRC:-/root/LiberNT}
OUT=${OUT:-$SRC/output-Clang-amd64-debug}
FEED=${FEED:-/srv/luup}
KEY=${KEY:-/opt/luup/signing.key}
LUUP=${LUUP:-/opt/luup/luup.py}
FEED_PUSH=${FEED_PUSH:-}
PUSH_KEY=${PUSH_KEY:-/root/.ssh/luup_push}

cd "$SRC"
git fetch -q origin nks
git checkout -q -B nks origin/nks
head=$(git rev-parse HEAD)
if python3 -c 'import json,sys; sys.exit(0 if any(r["commit"] == sys.argv[2] for r in json.load(open(sys.argv[1]))["releases"]) else 1)' \
        "$FEED/feed.json" "$head"; then
    echo "commit $head is already published"
else
    echo "building $head"
    ninja -C "$OUT" -j"$(nproc)" liberntimg
    python3 "$LUUP" publish --key "$KEY" --feed "$FEED" --img "$OUT/LiberNT.img" --commit "$head" --repo "$SRC"
    python3 "$LUUP" verify --feed "$FEED" --pub "$(cat "${KEY%.key}.pub")"
fi

if [ -n "$FEED_PUSH" ]; then
    # Objects and manifests first, the signed feed last: clients never see a release whose files are missing.
    ssh_cmd="ssh -i $PUSH_KEY -o BatchMode=yes -o StrictHostKeyChecking=accept-new"
    rsync -a -e "$ssh_cmd" --exclude feed.json --exclude feed.json.sig "$FEED/" "$FEED_PUSH"
    rsync -a -e "$ssh_cmd" "$FEED/feed.json" "$FEED/feed.json.sig" "$FEED_PUSH"
    echo "feed pushed to $FEED_PUSH"
fi
