# Operations

## Release a new client

1. Commit and push to `main`; CI builds the client and runs the publisher tests, client self-test and HTTP probe.
2. Tag it: `git tag -a v0.2.5 -m v0.2.5 && git push origin v0.2.5`.
3. `release.yml` builds with `LNTU_VERSION=v0.2.5`, runs the self-test, checks `/version` and attaches `LiberNTUpdate.exe`, `luupboot.exe` and `SHA256SUMS` to the GitHub release.
4. The next run of `build-release.sh` picks the release up (`releases/latest`), checks `SHA256SUMS` and publishes a feed release with it.

Tags with a prerelease suffix, such as `v0.3.0-rc.1`, are marked as GitHub prereleases and never replace the latest stable release. The automatic build rejects drafts and unrequested prereleases. To test a particular published client, set `CLIENT_TAG` and use a separate feed directory and push destination:

```sh
CLIENT_TAG=v0.3.0-rc.1 FEED=/srv/luup-testing FEED_PUSH= ./build-release.sh
```

For an existing installation, first seed that private testing directory from a verified copy of its current feed so it retains the installed release and registry baseline. Serve it at a separate URL and set the test client's `feedUrl` to that URL. The example does not push to the update server. These are client release channels; OS source synchronization and image builds keep their existing schedule. Switching a tested installation back to a stable feed still requires that feed to contain its recorded release history; an older feed is refused.

## Release a new LiberNT build

`build-release.sh` runs from `luup-release.timer` every day. To publish at once:

```sh
systemctl start luup-release.service
journalctl -u luup-release.service -n 20 -o cat
```

`published <id>: N files, M new objects, K changed` means a new release; `nothing to publish` means the image carries the same files as the last release; `already published` means the commit and the client are both in the feed.

The fork merges upstream daily (`Sync upstream` workflow). To merge now: `gh workflow run "Sync upstream" -R nks-hub/LiberNT --ref nks`. A merge conflict opens an issue in the fork and fails the workflow.

## Migrate legacy updater data

Run the registry-capable client as an administrator. Set `FEED_URL` to the verified feed and `BASE_RELEASE` to a file baseline established from trusted installation records or its signed manifest:

```bat
start "" /wait LiberNTUpdate.exe /migrate-legacy --feed "%FEED_URL%" --base-release "%BASE_RELEASE%"
```

For a registry feed, also pass `--registry-base-release` with the release of the original factory hives. Use `--boot-root` only for a separately verified boot partition. The command checks the signed feed and manifests in a new protected directory before stopping and disabling the legacy updater services. It archives the old directory without adopting its plans, history or recorded release, then activates the protected state with `auto=off`.

A prepared update is archived without being applied. An already applied, unconfirmed update must be finished or independently recovered first. The archive path is printed on success and retained on a failed rollback; an occupied destination is never replaced.

Keep that archive until verification is complete. Install the tested binaries under Program Files, run `/register` and start `LiberNTUpdate`. Check the service, `/check` and a normal restart before enabling automatic installation. Keep `auto=off` while a candidate client is newer than the client in its selected feed.

## Recover from an independent system

Boot a separate LiberNT amd64 recovery installation or USB image with storage drivers for the target disk. Run the recovery client built from this revision as an administrator. The target Windows directory and protected updater data must be readable; recovery refuses the volume running the recovery system. The EFI partition must be on the target OS disk for this implementation.

Identify the target OS volume and its boot partition before running the commands. These examples use `E:` for the offline OS and `S:` for its ESP. Drive letters can differ between the installed system and the recovery system. From the folder containing the recovery client:

```bat
start "" /wait LiberNTUpdate.exe /recover-offline --root E:\ --list
set /p RECOVERY_TX=Transaction ID printed above:
start "" /wait LiberNTUpdate.exe /recover-offline --root E:\ --tx "%RECOVERY_TX%" --boot-root S:\
```

For BIOS boot files stored on the OS volume, pass `--boot-root E:\`. An EFI volume GUID path from `mountvol` can replace `S:\`; the recovery client refuses an ESP that is not on the target OS disk. A completed direct install, such as an EFI loader update, is listed the same way as a pending one. Recovery restores the loader files from the transaction backup and does not change BCD. `--data-relative` selects an updater directory below the target volume when it differs from `ProgramData/LiberNTUpdate`. Recovery output goes to the console; this command does not create the recovery system's `cli-output.txt`.

The selected transaction must be the current pending update or the confirmed install printed by `--list`. A pending install is undone; a pending rollback is completed. Backups and expected target contents are checked before the recovery plan runs. A changed local file or registry value is preserved and reported as a conflict. Incomplete recovery keeps the transaction active and blocks automatic reapplication and confirmation.

Registry values are restored inside the offline SYSTEM, SOFTWARE and DEFAULT hives. Local values outside the transaction stay intact. The hives must be flushed and unloaded before recovery is recorded as complete. If loading a hive reports it in use, finish any other recovery operation. After an interrupted recovery process, restart the independent recovery OS before repeating the command.

Read `tx/<id>/offline/plan.log` in the target updater directory if a plan fails. Once offline recovery has started, the boot-time update and rollback plans of that transaction no longer run and the service does not schedule a restart for it. The BIOS recovery entry still starts the previous kernel and HAL; repeat the offline recovery to finish the rest. Keep the backups until the repaired installation boots. After success, shut down the recovery system, remove its boot medium and verify the installed OS.

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

Before updating an installation without a recorded release, set `baseRelease` to its verified release ID. Do not use the previous feed release as a guess. Automatic UEFI mapping checks the current firmware boot device, the partition table, FAT filesystem and LiberNT loader. It accepts a missing Setup value but refuses a conflicting one, the OS partition and multiple ESPs, including clones. For an unsupported or ambiguous layout, an administrator must verify and configure `bootRoot` explicitly.

`build-release.sh` rejects a dirty source checkout and rechecks the cached client's `SHA256SUMS` on every run. It verifies the whole feed before each push, including the `already published` case. Signing keys are created exclusively with mode `0600` before their contents are written.

## Screenshots

See [screenshots](SCREENSHOTS.md) of an update from upstream changes.
