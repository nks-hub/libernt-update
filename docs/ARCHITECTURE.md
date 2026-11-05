# Architecture

LiberNT Update has four parts: the publisher on the build machine, a static update server, the client in LiberNT and the native boot-time program `luupboot.exe`.

```
 nks-hub/LiberNT (nks)          GitHub release vX.Y.Z
         │                    (LiberNTUpdate.exe, luupboot.exe)
         ▼                                │
 build machine: build-release.sh ◄────────┘
   ninja liberntimg → luup.py publish ──► feed/ ──rsync──► update server (nginx)
                                                                │ HTTP
                                                                ▼
                                       LiberNT: LiberNTUpdate service, window, tray
                                                                │ restart
                                                                ▼
                                           luupboot.exe (UpdateExecute / BootExecute)
```

## Feed

| Path | Content |
|---|---|
| `feed.json` | list of releases: id, LiberNT commit, client version, manifest hash, notes |
| `feed.json.sig` | Ed25519 signature of `feed.json` (hex) |
| `feed-v2.json`, `feed-v2.json.sig` | registry-capable release index and signature; starts with the historical release identities from v1 |
| `releases/<id>/manifest.json` | every serviced file of the release: path, SHA-256, size; kernel and HAL variants |
| `objects/<aa>/<sha256>` | file contents, stored once per hash |

The client trusts only the public key compiled into it. It checks the signature, then the manifest against `manifestSha256`, then every downloaded object against its SHA-256, so the transport can be plain HTTP.

A release lists the files under `Windows/`, `Program Files/` and `Program Files (x86)/` of the image's NTFS partition, the boot loaders of the image's boot partition under `BootPartition/`, and the client under `Program Files/LiberNT Update/`. Registry hives, logs, caches and `unattend.inf` are not serviced.

`publish --img ... --registry` attaches a hashed snapshot of factory registry defaults and publishes through `feed-v2.json`. It records keys and typed values from SYSTEM's selected Control and Services branches, SOFTWARE and DEFAULT; it excludes SAM, SECURITY and registry links. The input must be a trusted factory image, not an installed machine backup; `--tree --registry` is refused. The v1 feed remains unchanged so older clients never receive a transaction that requires registry support.

The client tries v2 first and uses v1 only when v2 is absent (HTTP 404 or a missing local file). Signature failures and other transport errors never trigger fallback. Once a signed v2 index is accepted, its protocol is recorded and a later missing v2 index cannot downgrade the client to v1.

## Check

`CheckForUpdate` compares the newest release with the disk:

1. Hashes actual file contents with at most four workers; it does not trust size and modification time as proof of unchanged contents.
2. Keeps the variant Setup installed for `ntoskrnl.exe` and `hal.dll`.
3. Treats a missing file as wanted when it is new compared with the recorded installed release. An unknown installation requires a verified `baseRelease`; the previous release is never assumed. Boot loaders are only replaced, never added.
4. Compares factory registry snapshots against the live registry. Changed local values stay untouched. Added parent keys are explicit operations; removed keys are deleted only when their remaining contents are also being removed. SYSTEM paths pin the selected control set when the transaction is prepared.

The registry baseline is recorded independently from file releases. A legacy installation requires a verified `registryBaseRelease`; a file-only update is never evidence that its registry was migrated. Verified snapshots are cached by hash under `registry/`. Identical signed snapshot references need no object download or parsing. Even a release whose desired values are already present advances this baseline when installed.

Checks, installs, recovery, cancellation and rollback share a machine-wide mutex. Downloaded metadata is limited to 16 MiB, signatures to 256 bytes, and objects to their signed length with a 512 MiB maximum. The data directory must have trusted ownership, no writable access for ordinary users and no reparse points.

## Install

`InstallUpdate` downloads and verifies every changed file into `tx/<id>/new/`, copies each file it will replace into `tx/<id>/backup/` and writes `tx/<id>/tx.json` with the absolute target path of every operation. Later stages use these paths and never map manifest paths again.

The active transaction is flushed to `state.json` before replacement starts. Recovery can restore a partly applied direct installation, including a client interrupted after its old EXE was renamed aside.

- **Direct** - when no target is in use (or only the client's own files, which are renamed aside), files are moved into place, verified by hash and restored from the backup when anything fails.
- **At the next restart** - when a target is in use or a registry change is required, the client writes `boot/plan.txt` and `boot/rollback.txt` and adds the recovery entry to `freeldr.ini`. Registry-capable SMSS runs the helper through `UpdateExecute`; file-only systems use `BootExecute`. Replacement copies into a temporary file on the target volume before a same-volume rename, so an EFI target can be on another volume.

## Boot

SMSS runs `luupboot \??\C:\ProgramData\LiberNTUpdate` before Win32 and `PendingFileRenameOperations`. Registry-capable builds initialize the hives before processing `UpdateExecute`, then continue with pending renames and KnownDLLs. Their volatile `HARDWARE\LiberNTUpdate\Version` capability is required by the native registry writer. Earlier `BootExecute` is unsuitable: the hives are not all loaded and `CmpNoWrite` can make a flush return success without writing to disk.

The boot plan also supports `G` operations with desired and expected binary registry records. Keys must be created through explicit operations before writing their values; rollback removes only empty keys. A changed value is preserved unless it matches the recorded expectation. Registry changes, including retries that already see the desired state, must pass a hive flush before reporting success.

| Found in `boot/` | luupboot does |
|---|---|
| `plan.txt` | applies it, renames it to `plan.done`, writes `trial` = 1 and `result.txt` / `plan.log` |
| `applying` | an interrupted installation; apply the rollback even if `plan.txt` was already renamed |
| `committed` | the service already committed cancellation or completion; do not run the old plans |
| `trial` without a plan | counts the boot; on the third unconfirmed boot applies `rollback.txt` and writes `rolledback` |
| boot entry with `/LNTUROLLBACK` | applies `rollback.txt` at once (or drops a plan that has not run) |

After applying a plan or a rollback, luupboot restarts the computer before smss continues. That boot still runs the previous kernel, smss and ntdll; starting csrss and the rest of the system from the replaced files on top of them crashed smss with a LiberNT release that changed 1546 files. The following boot starts entirely from the new files, and the service confirms it there because its kernel boot identity differs from the one that prepared the update. A durable `committed` marker disables boot actions before the plan directory is retired and `pending` is cleared.

## Rollback paths

| Failure | Who undoes it |
|---|---|
| a step of a direct install fails | `ApplyTx` restores the backups at once |
| a file does not match after the restart | the service builds a rollback transaction and restarts |
| the system never reaches the service | `luupboot.exe` on the third boot |
| the new kernel or HAL does not start | the recovery entry starts the previous ones with `/LNTUROLLBACK` |
| the new client does not keep running | the watchdog (a copy of the previous client) restores it |

A rollback touches only files whose hash is exactly what the update wrote and registry values matching the recorded new contents. Later local changes and nonempty new keys are preserved. Confirming a partial rollback records the restored registry baseline independently from conflicting files. A release that failed or was rolled back on a machine is not installed again automatically.

## Self-update

Before replacing the client, every install path snapshots the running EXE and registers `LiberNTUpdateGuard` from that snapshot. Direct updates start the snapshot with `/watch-self-update <tx>`; restart updates run it as an independent automatic service. The watchdog requires the new service to keep running for 30 seconds. A failed direct update is restored; a failed restart update requests boot rollback. The guard is removed after completion or recovery.

## Data folder

`%ProgramData%\LiberNTUpdate`:

| Path | Content |
|---|---|
| `config.json` | feed URL, automatic mode, interval, restart hour |
| `state.json` | installed release, pending transaction |
| `history.json` | every transaction and its result |
| `status.json` | last automatic check (read by the tray icon) |
| `update.log` | every step, mismatch, undo and boot-time failure |
| `tx/<id>/` | `tx.json`, downloaded files, backups (the last five transactions are kept) |
| `boot/` | plans and state files of `luupboot.exe` |
| `watchdog/` | copy of the running client for self-updates |

## Tray pin preference

The tray menu reads the current user's `Control Panel\NotifyIconSettings` entry for its own executable path and icon UID. It verifies both fields before changing `IsPromoted`, then recreates its own icon so Explorer reloads the preference. This uses LiberNT Explorer's settings format; the menu item is disabled when a matching entry is unavailable, and starting the tray does not overwrite the user's choice.

## Plan format

UTF-16LE, one operation per line:

```
LUUP 1
R<TAB><source><TAB><target><TAB><expected-current-file>
X<TAB><target><TAB><expected-current-file>
```

`R` replaces a file and `X` deletes it only when the target still matches the recorded contents. `-` means the target must be absent. An already restored target succeeds without another write. Sources remain available for retries. Legacy `M`, `C` and `D` plans are still readable. Failures remain pending and are recorded in the boot log.
