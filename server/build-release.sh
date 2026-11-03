#!/bin/sh
# Builds LiberNT.img from the nks branch of nks-hub/LiberNT, adds the newest released client (GitHub release of
# CLIENT_REPO, checked against its SHA256SUMS), publishes the result to the update feed and, when FEED_PUSH is set
# (rsync destination such as user@host:), copies the feed to the update server.
# A release is made when the LiberNT commit or the client version changed and the image carries changed files.
set -eu
SRC=${SRC:-/root/LiberNT}
OUT=${OUT:-$SRC/output-Clang-amd64-debug}
FEED=${FEED:-/srv/luup}
KEY=${KEY:-/opt/luup/signing.key}
LUUP=${LUUP:-/opt/luup/luup.py}
FEED_PUSH=${FEED_PUSH:-}
PUSH_KEY=${PUSH_KEY:-/root/.ssh/luup_push}
CLIENT_REPO=${CLIENT_REPO:-nks-hub/libernt-update}
CLIENT_CACHE=${CLIENT_CACHE:-/var/cache/luup/client}

# Prints the folder with the newest client release, downloading and checking it the first time.
fetch_client() {
    tag=$(curl -fsSL "https://api.github.com/repos/$CLIENT_REPO/releases/latest" |
          python3 -c 'import json,sys; print(json.load(sys.stdin)["tag_name"])')
    dir="$CLIENT_CACHE/$tag"
    if [ ! -f "$dir/VERSION" ]; then
        mkdir -p "$dir.part"
        for f in LiberNTUpdate.exe luupboot.exe SHA256SUMS; do
            curl -fsSL -o "$dir.part/$f" "https://github.com/$CLIENT_REPO/releases/download/$tag/$f"
        done
        (cd "$dir.part" && sha256sum --check --strict --quiet SHA256SUMS) >&2
        echo "$tag" > "$dir.part/VERSION"
        mv "$dir.part" "$dir"
    fi
    echo "$dir"
}

client_dir=$(fetch_client)
client=$(cat "$client_dir/VERSION")
cd "$SRC"
git fetch -q origin nks
git checkout -q -B nks origin/nks
head=$(git rev-parse HEAD)
if python3 -c 'import json,sys; sys.exit(0 if any(r["commit"] == sys.argv[2] and r.get("client") == sys.argv[3]
                                                    for r in json.load(open(sys.argv[1]))["releases"]) else 1)' \
        "$FEED/feed.json" "$head" "$client"; then
    echo "commit $head with client $client is already published"
else
    echo "building $head with client $client"
    # version.cmake reads the commit at configure time only. Reconfigure when the commit changed, so the build tag
    # names it; not otherwise, because the regenerated buildno.h relinks the kernel for a client-only release.
    if ! grep -q "$head" "$OUT/sdk/include/reactos/buildno.h" 2>/dev/null; then
        cmake -S "$SRC" -B "$OUT" > /dev/null
    fi
    ninja -C "$OUT" -j"$(nproc)" liberntimg
    python3 "$LUUP" publish --key "$KEY" --feed "$FEED" --img "$OUT/LiberNT.img" --commit "$head" --repo "$SRC" --client "$client_dir"
    python3 "$LUUP" verify --feed "$FEED" --pub "$(cat "${KEY%.key}.pub")"
fi

if [ -n "$FEED_PUSH" ]; then
    # Objects and manifests first, the signed feed last: clients never see a release whose files are missing.
    ssh_cmd="ssh -i $PUSH_KEY -o BatchMode=yes -o StrictHostKeyChecking=accept-new"
    rsync -a -e "$ssh_cmd" --exclude feed.json --exclude feed.json.sig "$FEED/" "$FEED_PUSH"
    rsync -a -e "$ssh_cmd" "$FEED/feed.json" "$FEED/feed.json.sig" "$FEED_PUSH"
    echo "feed pushed to $FEED_PUSH"
fi
