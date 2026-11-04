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

# Prints the folder with the newest client release; cached files are checked on every run.
fetch_client() {
    tag=$(curl -fsSL "https://api.github.com/repos/$CLIENT_REPO/releases/latest" |
          python3 -c 'import json,sys; print(json.load(sys.stdin)["tag_name"])')
    case "$tag" in v[0-9]* ) ;; * ) echo "invalid client release tag" >&2; exit 1 ;; esac
    case "$tag" in *[!a-zA-Z0-9._-]* ) echo "unsafe client release tag" >&2; exit 1 ;; esac
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
    (cd "$dir" && sha256sum --check --strict --quiet SHA256SUMS) >&2
    [ "$(cat "$dir/VERSION")" = "$tag" ] || { echo "cached client version mismatch" >&2; exit 1; }
    echo "$dir"
}

client_dir=$(fetch_client)
client=$(cat "$client_dir/VERSION")
cd "$SRC"
[ -z "$(git status --porcelain --untracked-files=normal)" ] || { echo "source checkout is dirty" >&2; exit 1; }
git fetch -q origin nks
git checkout -q -B nks origin/nks
head=$(git rev-parse HEAD)
feed_index="$FEED/feed.json"
[ ! -f "$FEED/feed-v2.json" ] || feed_index="$FEED/feed-v2.json"
if python3 -c 'import json,sys; sys.exit(0 if any(r["commit"] == sys.argv[2] and r.get("client") == sys.argv[3]
                                                    for r in json.load(open(sys.argv[1]))["releases"]) else 1)' \
        "$feed_index" "$head" "$client"; then
    echo "commit $head with client $client is already published"
else
    echo "building $head with client $client"
    # version.cmake reads the commit at configure time only. Reconfigure when the commit changed, so the build tag
    # names it; not otherwise, because the regenerated buildno.h relinks the kernel for a client-only release.
    if ! grep -q "$head" "$OUT/sdk/include/reactos/buildno.h" 2>/dev/null; then
        cmake -S "$SRC" -B "$OUT" > /dev/null
    fi
    ninja -C "$OUT" -j"$(nproc)" liberntimg
    [ -z "$(git status --porcelain --untracked-files=normal)" ] && [ "$(git rev-parse HEAD)" = "$head" ] || {
        echo "source checkout changed while building" >&2; exit 1;
    }
    if [ -f "$FEED/feed-v2.json" ]; then
        python3 "$LUUP" publish --key "$KEY" --feed "$FEED" --img "$OUT/LiberNT.img" --commit "$head" --repo "$SRC" --client "$client_dir" --registry
    else
        python3 "$LUUP" publish --key "$KEY" --feed "$FEED" --img "$OUT/LiberNT.img" --commit "$head" --repo "$SRC" --client "$client_dir"
    fi
fi

# A previous publish can have failed verification. Never push that feed on the next already-published run.
python3 "$LUUP" verify --feed "$FEED" --pub "$(cat "${KEY%.key}.pub")"

if [ -n "$FEED_PUSH" ]; then
    # Objects and manifests first, the signed feed last: clients never see a release whose files are missing.
    ssh_cmd="ssh -i $PUSH_KEY -o BatchMode=yes -o StrictHostKeyChecking=accept-new"
    rsync -a -e "$ssh_cmd" --exclude feed.json --exclude feed.json.sig --exclude feed-v2.json --exclude feed-v2.json.sig "$FEED/" "$FEED_PUSH"
    for index in feed.json feed-v2.json; do
        [ ! -f "$FEED/$index" ] || rsync -a -e "$ssh_cmd" "$FEED/$index" "$FEED/$index.sig" "$FEED_PUSH"
    done
    echo "feed pushed to $FEED_PUSH"
fi
