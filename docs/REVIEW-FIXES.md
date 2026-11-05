# Review findings and fixes

Baseline: commit `c6f6fed`. Fixes from 4 November 2026. This overview separates code changes from verification by a real system boot.

| Finding | Fix |
|---|---|
| R01: kernel confirmed too early | Recovery stays in place after the first boot. Confirmation needs another boot with a different running kernel identity. |
| R02: service, GUI and CLI race | One named mutex covers checking, state writes, installation and recovery. |
| R03: interrupted installation | The active transaction is saved before any file changes. Recovery handles a direct install and the separate marker of an interrupted boot. |
| R04: EFI on another volume | The boot program copies to a temporary file on the target volume and renames it there. |
| R05: wrongly guessed baseline | An unknown installation requires a verified `baseRelease`. The R1 to R3 test includes a library added in R2. |
| R06: manual self-update without protection | The shared install path prepares the old watchdog. A separate Guard service also protects an update applied at restart. |
| R07: service restart and cancelling a transaction | A service restart keeps the plan. `/cancel` refuses an installation that has started. A durable marker disables a cancelled plan before the transaction is released. |
| R08: boot rollback overwrites other changes | Every new operation checks where the target content came from. A conflict keeps the file and the backup for another attempt. |
| R09: partial rollback reports the old release | A rollback with conflicts has the state `partial-rollback` and does not confirm the old release. |
| R10: unlimited HTTP responses | Metadata is limited to 16 MiB and the signature to 256 bytes. An object must have a signed length of at most 512 MiB. The limits also apply to a local feed. |
| R11: signature parser string out of scope | The string stays valid until the pointer returned by `strtol` is checked. |
| R12: different mapping of offline EFI | Checking and installing call the same target mapping function. |
| R13: first EFI partition found | The partition named by Setup or an explicit `bootRoot` is used. An ambiguous result is refused. |
| R14: repeated publishing skips verification | Feed verification runs before every transfer, including the `already published` path. |
| R15: unverified client cache | Checksums are verified on every cache use and before the client is placed into the image. |
| R16: dirty source tree | The build refuses uncommitted changes and checks the tree and the commit again before publishing. |
| R17: key permissions set after writing | The file is created with an exclusive open and mode `0600`; an existing key cannot be overwritten. |

A follow-up review added a durable confirmation of the boot plan, recovery of an EXE moved aside during self-update and keeping the watchdog when a recovery fails. The watchdog must see the service actually `RUNNING` and keep running for 30 more seconds.

The data folder is checked before stored plans are trusted. An unknown owner, write access for normal users or a reparse point leads to a refusal. That alone does not prove correct permissions on every installation; they have to be checked on the target system.

## Verification

- The client build and the built-in regression tests ran on Windows and on LiberNT. The set has 58 checks, including clean-up of old backups after the boot is confirmed. The EXE checksum on LiberNT matched the final local build: `e10e75b22398c106708d82027be7d0fb3048d68f0748dc0a87925de75e927e65`.
- The publisher test also checks a damaged cache and exclusive key creation.
- Four tests of the real `build-release.sh` ran on Linux: verification before transfer, refusing an invalid feed, a damaged cache and a dirty tree.
- The [VM test](../client/tests/boot_vm.cpp) replaced test files on the system NTFS volume and on a separate FAT volume. At that time the first boot kept the transaction pending and the second, with a different boot identity, confirmed it. Since `fb4b214` the native helper restarts right after the replacement, so the first boot that runs Windows confirms the update.
- Another real restart recovered a prepared interrupted state (`applying` + `plan.done` without `trial`). The original contents on both volumes were verified and the pending transaction was cleared.
- On LiberNT the service test passed with a service staying several seconds in `START_PENDING`, and a service stuck in that state was refused. A separate [test service](../client/tests/service_vm.cpp) was used, not a change of the production client.
- On the tested installation the check found untrusted write access to the data folder. The new client refused it. Before using it in production the permissions must be fixed and the origin of the old data verified; taking over those plans automatically would not be safe.
- After the test the original `BootExecute` was restored and the test boot program removed. The original service ran and its check reported no changes.

These tests do not replace booting a different upstream kernel, a real UEFI boot or a full release of a broken client with its automatic return; those were verified later (see [progress](PROGRESS.md)). The interrupted boot test uses a prepared transaction state; it is not a power-loss measurement.

No new client release or server change is part of that commit. Separating the build from the signing key needs a separate change of the operating procedure. The review found no way around Ed25519 verification; the client verifies with Monocypher and the production signature is made with PyNaCl.
