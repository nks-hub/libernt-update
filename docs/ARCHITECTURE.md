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

## Check

`CheckForUpdate` compares the newest release with the disk:

1. Hashes every file the manifest lists (a cache keyed by size and modification time skips unchanged files).
2. Keeps the variant Setup installed for `ntoskrnl.exe` and `hal.dll`.
3. Treats a missing file as wanted only when it is new compared with the installed release (or the previous one), so files Setup left out are not added; boot loaders are only replaced, never added.

## Install

`InstallUpdate` downloads and verifies every changed file into `tx/<id>/new/`, copies each file it will replace into `tx/<id>/backup/` and writes `tx/<id>/tx.json` with the absolute target path of every operation. Later stages use these paths and never map manifest paths again.

- **Direct** - when no target is in use (or only the client's own files, which are renamed aside), files are moved into place, verified by hash and restored from the backup when anything fails.
- **At the next restart** - when a target is in use, nothing changes on disk yet. The client writes `boot/plan.txt` (the moves) and `boot/rollback.txt` (their undo), registers `luupboot.exe` in `BootExecute` and adds the recovery entry to `freeldr.ini`.

## Boot

smss runs `luupboot \??\C:\ProgramData\LiberNTUpdate` before any Win32 process and before `PendingFileRenameOperations`.

| Found in `boot/` | luupboot does |
|---|---|
| `plan.txt` | applies it, renames it to `plan.done`, writes `trial` = 1 and `result.txt` / `plan.log` |
| `trial` without a plan | counts the boot; on the third unconfirmed boot applies `rollback.txt` and writes `rolledback` |
| boot entry with `/LNTUROLLBACK` | applies `rollback.txt` at once (or drops a plan that has not run) |

The `LiberNTUpdate` service then verifies every target, confirms the update (removes `trial`, the plans and the recovery entry) or rolls it back.

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

The service copies the exe it runs from to `watchdog/<hash>.exe` when it starts. When an update replaces `LiberNTUpdate.exe`, the running exe is renamed to `*.old-<tx>` and the new one takes its name. The service then starts the watchdog copy with `/watch-self-update <tx>` and stops. The watchdog restarts the service and requires it to stay running for 30 seconds; otherwise it rolls the transaction back and starts the service with the previous client.

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
M<TAB>\??\C:\ProgramData\LiberNTUpdate\tx\<id>\new\0.bin<TAB>\??\C:\Windows\explorer.exe
D<TAB>\??\C:\Windows\old.dll
```

`M` moves a file over the target (clearing read-only first when needed), `D` deletes it. luupboot keeps going after a failed line and lists every failure in `plan.log`.
