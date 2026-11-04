# Operations

## Release a new client

1. Commit and push to `main`; CI builds the client and runs the publisher tests, client self-test and HTTP probe.
2. Tag it: `git tag -a v0.2.5 -m v0.2.5 && git push origin v0.2.5`.
3. `release.yml` builds with `LNTU_VERSION=v0.2.5`, runs the self-test, checks `/version` and attaches `LiberNTUpdate.exe`, `luupboot.exe` and `SHA256SUMS` to the GitHub release.
4. The next run of `build-release.sh` picks the release up (`releases/latest`), checks `SHA256SUMS` and publishes a feed release with it.

## Release a new LiberNT build

`build-release.sh` runs from `luup-release.timer` every day. To publish at once:

```sh
systemctl start luup-release.service
journalctl -u luup-release.service -n 20 -o cat
```

`published <id>: N files, M new objects, K changed` means a new release; `nothing to publish` means the image carries the same files as the last release; `already published` means the commit and the client are both in the feed.

The fork merges upstream daily (`Sync upstream` workflow). To merge now: `gh workflow run "Sync upstream" -R nks-hub/LiberNT --ref nks`. A merge conflict opens an issue in the fork and fails the workflow.

## Enable registry servicing

1. Publish and install an OS bootstrap build containing the `UpdateExecute` SMSS change through the existing file-only feed. Complete its restart verification before enabling registry updates.
2. Install the registry-capable client. A running compatible SMSS exposes `HKLM\HARDWARE\LiberNTUpdate\Version = 1`; the client refuses registry updates without it.
3. Identify the original factory image of each legacy installation. File updates do not establish which hive defaults it started with. Never use an installed machine's hive backup as factory input.
4. In a private staging feed, publish the verified original factory image with `publish --img ... --registry`, then publish the current compatible image with the same option. Retain the existing release history. Do not expose the original baseline as the public latest release between these steps.
5. Set `registryBaseRelease` to the staged release representing that installation's original factory hives. Omit it only when the client already recorded a verified registry baseline.
6. Verify the staging feed and activate its objects and manifests before `feed-v2.json` and its signature. The original v1 index stays frozen for older clients. Subsequent `build-release.sh` runs preserve registry publishing when v2 exists.

The client caches verified registry snapshots and keeps per-value undo records in its transaction. It does not replace whole installed hives. The first successful restart retains recovery; confirmation still requires another boot identity.

## Check the feed

```sh
python3 luup.py verify --feed /srv/luup --pub "$(cat /opt/luup/signing.pub)"
curl -s http://updates.example.org/feed.json | python3 -m json.tool | tail -30
```

## Pull back a bad release

Clients always move to the newest release, and published releases are never edited. To undo a bad one for every machine, publish a release that is newer and carries the good files: revert the commit in `nks` (or tag the previous client again with a higher version) and run `build-release.sh`. Machines that already installed the bad release update to the new one; machines where it failed do not retry it.

## Inside LiberNT

| Task | Command |
|---|---|
| state of the last automatic check | `LiberNTUpdate.exe /status` |
| what would change | `LiberNTUpdate.exe /check` |
| install now | `LiberNTUpdate.exe /install`, then `/restart` if it waits for a restart |
| undo the last update | `LiberNTUpdate.exe /rollback` |
| withdraw an update that waits for a restart | `LiberNTUpdate.exe /cancel` |
| history | `LiberNTUpdate.exe /history` |

Run them with `start "" /wait` from `cmd.exe`; the output is also in `%ProgramData%\LiberNTUpdate\cli-output.txt`.

## Troubleshooting

Everything is in `%ProgramData%\LiberNTUpdate\update.log`.

| Log line | Meaning, what to do |
|---|---|
| `verify: <path> has <hash>, expected <hash>` | a file did not land; the service rolls the update back by itself |
| `luupboot plan.log: line N status X: M <from> -> <to>` | luupboot could not move a file at boot (status is an NTSTATUS) |
| `UNDO FAILED for <path>` | a file could not be restored; its backup is in `tx/<id>/backup/`, copy it back by hand |
| `... is not the file the update wrote ..., left as it is` | a rollback met a file changed by someone else and did not touch it |
| `auto: release <id> failed or was rolled back here` | the service will not retry it; install it by hand to try again |
| `self-update failed, rollback of <tx>` | the new client did not keep running; the previous one is back |
| `service restart failed: ...` | read the reason; `sc query LiberNTUpdate` shows the state |

When a system does not start after an update, choose **LiberNT (undo the last update)** in the boot menu; it starts the previous kernel and HAL and restores the previous files. A system that boots far enough for smss rolls back by itself on the third unconfirmed boot. Recovery remains available after the first replacement boot; confirmation requires another kernel boot. Restarting the service does not cancel a prepared update.

Before updating an installation without a recorded release, set `baseRelease` to its verified release ID. Do not use the previous feed release as a guess. If Setup cannot identify the EFI partition, configure its verified mount path as `bootRoot`; an ambiguous volume is refused.

`build-release.sh` rejects a dirty source checkout and rechecks the cached client's `SHA256SUMS` on every run. It verifies the whole feed before each push, including the `already published` case. Signing keys are created exclusively with mode `0600` before their contents are written.

## Screenshots

See [screenshots](SCREENSHOTS.md) of an update from upstream changes.
