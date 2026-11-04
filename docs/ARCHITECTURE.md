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
                                                 luupboot.exe (BootExecute)
```

## Feed

| Path | Content |
|---|---|
| `feed.json` | list of releases: id, LiberNT commit, client version, manifest hash, notes |
| `feed.json.sig` | Ed25519 signature of `feed.json` (hex) |
| `releases/<id>/manifest.json` | every serviced file of the release: path, SHA-256, size; kernel and HAL variants |
| `objects/<aa>/<sha256>` | file contents, stored once per hash |

The client trusts only the public key compiled into it. It checks the signature, then the manifest against `manifestSha256`, then every downloaded object against its SHA-256, so the transport can be plain HTTP.

A release lists the files under `Windows/`, `Program Files/` and `Program Files (x86)/` of the image's NTFS partition, the boot loaders of the image's boot partition under `BootPartition/`, and the client under `Program Files/LiberNT Update/`. Registry hives, logs, caches and `unattend.inf` are not serviced.

`publish --img ... --registry` can also attach a hashed snapshot of factory registry defaults. It records keys and typed values from SYSTEM's selected Control and Services branches, SOFTWARE and DEFAULT; it excludes SAM, SECURITY and registry links. The input must be a trusted factory image, not an installed machine backup; `--tree --registry` is refused. This metadata does not yet make the client apply registry changes.

## Check

`CheckForUpdate` compares the newest release with the disk:

1. Hashes actual file contents with at most four workers; it does not trust size and modification time as proof of unchanged contents.
2. Keeps the variant Setup installed for `ntoskrnl.exe` and `hal.dll`.
3. Treats a missing file as wanted when it is new compared with the recorded installed release. An unknown installation requires a verified `baseRelease`; the previous release is never assumed. Boot loaders are only replaced, never added.

Checks, installs, recovery, cancellation and rollback share a machine-wide mutex. Downloaded metadata is limited to 16 MiB, signatures to 256 bytes, and objects to their signed length with a 512 MiB maximum. The data directory must have trusted ownership, no writable access for ordinary users and no reparse points.

## Install

`InstallUpdate` downloads and verifies every changed file into `tx/<id>/new/`, copies each file it will replace into `tx/<id>/backup/` and writes `tx/<id>/tx.json` with the absolute target path of every operation. Later stages use these paths and never map manifest paths again.

The active transaction is flushed to `state.json` before replacement starts. Recovery can restore a partly applied direct installation, including a client interrupted after its old EXE was renamed aside.

- **Direct** - when no target is in use (or only the client's own files, which are renamed aside), files are moved into place, verified by hash and restored from the backup when anything fails.
- **At the next restart** - when a target is in use, the client writes `boot/plan.txt` and `boot/rollback.txt`, registers `luupboot.exe` in `BootExecute` and adds the recovery entry to `freeldr.ini`. Replacement copies into a temporary file on the target volume before a same-volume rename, so an EFI target can be on another volume.

## Boot

smss runs `luupboot \??\C:\ProgramData\LiberNTUpdate` before any Win32 process and before `PendingFileRenameOperations`.

The boot plan also supports `G` operations with desired and expected binary registry records. Keys must be created through explicit operations before writing their values; rollback removes only empty keys. A changed value is preserved unless it matches the recorded expectation. Registry changes, including retries that already see the desired state, must pass a hive flush before reporting success.

| Found in `boot/` | luupboot does |
|---|---|
| `plan.txt` | applies it, renames it to `plan.done`, writes `trial` = 1 and `result.txt` / `plan.log` |
| `applying` | an interrupted installation; apply the rollback even if `plan.txt` was already renamed |
| `committed` | the service already committed cancellation or completion; do not run the old plans |
| `trial` without a plan | counts the boot; on the third unconfirmed boot applies `rollback.txt` and writes `rolledback` |
| boot entry with `/LNTUROLLBACK` | applies `rollback.txt` at once (or drops a plan that has not run) |

The first service start verifies the disk but retains recovery. The next boot must have a different kernel boot identity before the service confirms it. A durable `committed` marker disables boot actions before the plan directory is retired and `pending` is cleared.

## Rollback paths

| Failure | Who undoes it |
|---|---|
| a step of a direct install fails | `ApplyTx` restores the backups at once |
| a file does not match after the restart | the service builds a rollback transaction and restarts |
| the system never reaches the service | `luupboot.exe` on the third boot |
| the new kernel or HAL does not start | the recovery entry starts the previous ones with `/LNTUROLLBACK` |
| the new client does not keep running | the watchdog (a copy of the previous client) restores it |

A rollback touches only files whose hash is exactly what the update wrote; anything else is logged and left alone. A release that failed or was rolled back on a machine is not installed again automatically.

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

## Plan format

UTF-16LE, one operation per line:

```
LUUP 1
R<TAB><source><TAB><target><TAB><expected-current-file>
X<TAB><target><TAB><expected-current-file>
```

`R` replaces a file and `X` deletes it only when the target still matches the recorded contents. `-` means the target must be absent. An already restored target succeeds without another write. Sources remain available for retries. Legacy `M`, `C` and `D` plans are still readable. Failures remain pending and are recorded in the boot log.
