# Goals, process and status

Status as of 5 November 2026. This document is updated as work goes on. Code in progress, a passed test and a published release are separate states.

## Goals

- Remove the five README limitations: registry updates, recovery after a crashing boot driver, undoing a broken boot loader, finding and verifying the EFI partition, and a usable tray icon.
- Keep local settings and user data; never restore whole factory hives over a live installation.
- Keep the interface plain; check new behaviour in the real application and then update the screenshots.
- Measure speed and stability on LiberNT; back performance claims with measurements.
- Keep source synchronization, the OS build, the client release and the signed feed separate.

## Process

1. Find the cause in the affected code and confirm the current behaviour.
2. Make the smallest complete change and prepare the way back on failure.
3. Have the change reviewed and add a test for the original problem.
4. Run the needed tests on LiberNT over RemoteCmd; compare the SHA-256 of the build and of the tested file after each transfer.
5. Check boot-time changes with real restarts; calling boot code from a running process is not the same evidence.
6. Commit each verified unit separately and check its CI.
7. Update this overview, the operations guide and, if needed, the screenshots to the state actually reached.

## Done and verified

- `01007be`: 17 review findings fixed; 58 regression checks, three real restarts and service restart tests. Details are in the [review fixes](REVIEW-FIXES.md).
- OS fork: the automatic upstream synchronization runs again; the workflow only merges sources and disables inherited jobs. The OS is still built in the existing build environment.
- `7750899`: the publisher can attach a signed reference to the factory registry state; the exporter has three regression tests.
- `b453f3b`: native registry operations check the expected content, keep later local edits and require a flush. The 70 checks of that revision also passed on LiberNT; installation was not wired up yet.
- OS `5fccfb5`: SMSS provides the `UpdateExecute` phase after registry initialization and before KnownDLLs and Win32. The Clang build and the switch over two real restarts passed; the running system reported the phase.
- A combined file and SOFTWARE update and its rollback passed four more real restarts. The original `BootExecute` and `UpdateExecute` values were restored and the temporary native helper removed.
- The client at `f9974a7` passed 95 checks on LiberNT, including keeping local changes, confirming the registry without writes, rollback and refusing a downgrade to the old feed. SHA-256 of the tested EXE: `388de44e8c9723cce134d42b253587ba36efbbec734f0db440b617e2790c6c61`.
- The native helper used for the real restarts had SHA-256 `f9fb10540bd9b5b2d663c669a2e86a3c0d8bfad1b3b22dfd54286ad98e1aac69`. This is evidence for that artifact; newer builds do not inherit it.
- `f9974a7`: registry servicing and the separate v2 feed are committed; [CI 41926092184](https://github.com/nks-hub/libernt-update/actions/runs/41926092184) passed for client and publisher. All five Linux tests of the build script passed separately.
- Identical signed registry references skip downloading and parsing the factory snapshot. 96 checks passed on LiberNT, including a removed object and cache; client SHA-256 `62e7c79b8bf633b7f587bc8b2e7efcd23a45230891ab7b5b82fbb8eb3c0f9890`.
- `21ae6e8`: the registry optimization is pushed; [CI 41927512019](https://github.com/nks-hub/libernt-update/actions/runs/41927512019) passed.
- The tray menu pins and unpins only its own icon. LiberNT confirmed moving it between the hidden icons and the taskbar, keeping the choice after exiting the application and after a full restart, and leaving other icons unchanged.
- The menu fits above the taskbar; the [screenshot](SCREENSHOTS.md#tray-pin-preference) comes from the LiberNT test. The test process was closed and its icon unpinned again.
- The client with this interface change passed all 96 checks on LiberNT. SHA-256 of the tested EXE: `81e83629ca58af4b8023e3d51c467e75c67f6711a5330a814df941ef119e9a71`. This run does not replace the earlier evidence for boot-time native operations.
- `d102bde`: pinning, the menu fix and the screenshot are pushed; [CI 41934024700](https://github.com/nks-hub/libernt-update/actions/runs/41934024700) passed.
- Nine HTTP checks passed on LiberNT: falling back to the old feed only on 404 before v2 was accepted, refusing 403/500, a missing or wrong signature and invalid JSON, accepting v2, refusing a later return to the old protocol, and the response size limit. The test uses a temporary port on `127.0.0.1` and synthetic data only.
- The HTTP tests were reviewed and run in CI. SHA-256 of the tested helper: `4bb1f43264385f27b599f07e88b1bb83077dc872305945f17094ac55cc1e7147`; later builds are in [CI](https://github.com/nks-hub/libernt-update/actions/workflows/ci.yml).
- `1ff805e`: the HTTP regressions and their CI step are pushed; [CI 41934672654](https://github.com/nks-hub/libernt-update/actions/runs/41934672654) passed, including all nine HTTP checks on Windows.
- Client release channels: a version with a suffix, such as `v0.3.0-rc.1`, is a GitHub prerelease and does not replace the latest stable release. The publisher refuses drafts and unrequested prereleases; a separate test feed can name an exact `CLIENT_TAG`. Nine Linux regressions of the publisher script passed.
- `9353df0`: client channels are pushed; [CI 41996328546](https://github.com/nks-hub/libernt-update/actions/runs/41996328546) passed.
- Migrating the old data folder passed 106 checks on LiberNT, including refusing a damaged feed, protecting a recovery in progress, keeping the original bytes and going back after a failed move. Client SHA-256: `c2909a570b2c045db1665255ada9f510cc74ab4704ef7fce0e58d5c82b5f75a0`.
- A real older installation went through `/migrate-legacy`, installing the tested client, registering the service, `/check` and a normal restart. The service ran with the same SHA-256, the new folder passed the permission check and `pending` was empty. Automatic installation stayed off.
- The old prepared plan of 1,194 files was not applied. The kernel hash after the restart stayed `047e21f4200a0cf11ca1e0e363b1f3915377f43f27e76814df1fd06ce5d1422d`; the archived `state.json`, `history.json` and `boot/plan.txt` match the original files byte for byte.
- The last review fix also refuses a directory in place of the recovery flag and an inaccessible boot folder; the check runs again after the service stops. The extended set passed 108 checks on LiberNT. Client SHA-256: `5cd0e7038013ad86d8d4d44be3eb10da46ccd9c19066b0372439a244fb32eb96`.
- `18b6a96`: the migration is pushed and [CI 42005500867](https://github.com/nks-hub/libernt-update/actions/runs/42005500867) passed. The service in the VM also ran this final client and passed a 30-second running check.
- Automatic EFI detection checks the current firmware, the physical disk and partition, the ESP type, FAT and the loader file. A missing Setup value is accepted; a conflicting or invalid value or a reference to the OS partition is refused. Eight checks of these cases passed on a real UEFI LiberNT.
- Review found that a local alias could shadow a physical disk name. The inventory now opens disks in the global namespace; an incomplete or ambiguous read never leads to an automatic choice.
- Both EFI copies of the loader and a file on NTFS went through a joint update and rollback by the native helper over four real restarts. The test changed the unused DOS stub and recomputed the PE checksum to prove the bytes changed without changing loader code. Both copies had SHA-256 `e7cb92765a55135b70f93b475f67d90d604e34980b31836f9c8574be8b60c8bc` before the test and after rollback, and `4799fc3ba2ec262087182bef1ceb2dd4c319ac5c18bd074989427eb2ec8ff8c7` while updated.
- That UEFI test used OVMF, the PC machine type, an MBR disk and Secure Boot off. The native helper had SHA-256 `d4cf0ec21f4a3859bbbc887036bbc59a56b61476063d8669acf49a3d4b5cba48`. The original boot commands were restored and the temporary helper and test file removed.
- The client with the new EFI detection passed all 108 checks on the original test installation. SHA-256: `6dc9eeb5bc14450da278ec3be8c32041af6b921fdb3f12ff1eb60d331ca0ec58`.
- An exact clone of the whole test disk was compared byte for byte before attaching it. With two identical ESPs the updater refused to choose one, although volume enumeration showed only the first copy. The clone was detached afterwards and all eight detection checks passed again. The copy used for later recovery tests has its own disk identity.
- `602c662`: EFI verification is pushed and [CI 42020229612](https://github.com/nks-hub/libernt-update/actions/runs/42020229612) passed.

## Independent recovery

- `/recover-offline` runs from another system against an explicitly chosen target volume and transaction; it refuses the running OS and does not write its listing into the target data folder.
- Boot driver crash: a test transaction added a driver, its service in SYSTEM and a SOFTWARE value change. The first test service was named differently from its file, so the kernel did not start it and the `0xE2` crash had to be started by hand. The system had meanwhile confirmed the transaction; recovery therefore also selects a confirmed install whose history still says it waits for a restart.
- Recovery from a separate LiberNT on a second disk removed the driver and the service, restored the SOFTWARE value and kept an unrelated control value. The target disk then booted by itself with no pending transaction.
- Broken boot loader: a direct transaction replaced both EFI copies of the loader with 4 KiB of invalid content. The firmware found no bootable option. This test showed that recovery did not accept a completed direct install; the fix added a regression check.
- The fixed client on the separate system restored both copies to SHA-256 `e7cb92765a55135b70f93b475f67d90d604e34980b31836f9c8574be8b60c8bc`. BCD had the same hash before and after recovery; the updater does not change it. The target then booted normally with no pending transaction.
- Review after the test found three more defects: the service would restart the computer every minute during the configured hour after an incomplete offline recovery, a repeated `--list` could offer an older install, and two refusals had no error text. The fixes have regression checks. The documentation states that boot plans of a transaction no longer run once offline recovery has started.
- A fresh LiberNT `5fccfb5` installation gives system files to TrustedInstaller with read-only access for SYSTEM and administrators. The updater could not replace them: the boot plan ended with `STATUS_ACCESS_DENIED` and rolled back safely. The older test installation had `Everyone:F`, which hid the problem. `SeRestorePrivilege` does not grant DELETE on LiberNT. The updater now takes ownership of a refused file, replaces it and gives the new file the original owner and access rules.
- Updated boot driver crash: a transaction replaced the protected `disk.sys` with a test driver. The native helper replaced it at boot and kept its rules; the next boot crashed with `0xE2` in `Phase1Initialization`, before SMSS. Recovery from the second system restored the original `disk.sys` with its owner and rules and the target booted normally.
- New boot driver crash: the LiberNT kernel took a boot driver's service name from its file name (`IopInitializeBuiltinDriver`). With the service named after its file, the update added the driver and its service, the next boot crashed in `Phase1Initialization` before SMSS, and recovery from the second system removed the driver, the service and the SOFTWARE change while keeping an unrelated value.
- The protected and loaded `beep.sys` went through update, confirmation, rollback and confirmation over four real restarts; hash, owner and access rules matched at every step.
- GPT: an exact copy of the test disk was converted to GPT, and its BCD and `MountedDevices` were rewritten to the partition GUIDs. LiberNT booted from it and the updater chose the ESP by its GPT type; the eight detection checks, an update and rollback of both loaders over four restarts, and loader damage with recovery from the second system all passed. The copy stays as a detached disk of the test VM.
- Secure Boot with signed loaders: `bootmgfw.efi`/`bootx64.efi` and `winload.efi` signed with a test key were installed as a normal update (the protected `winload.efi` kept its TrustedInstaller rules). Firmware with that key in `db` and Secure Boot on started the system; an update to another signed variant and its rollback passed with real restarts. An unsigned loader was refused by the firmware, and recovery from the second system restored the signed one, after which the system booted again. The test variables and key exist only in the VM; it has its original EFI disk and files back.
- Unsigned loaders as shipped: OVMF with the default keys refused them ("Access Denied") and LiberNT did not start at all, so updates cannot run in that mode until the OS build signs its loaders.
- Review of the protected-file fix found a read-only attribute not cleared after taking ownership, a source file left with foreign rules after a failed rename, and a lost error code. All are fixed and the self-test also checks a failure after the unlock.
- Tests ran in VMs with OVMF on MBR and GPT disks. Physical UEFI hardware is not tested. The test driver and the loader damage live only in the test helper, not in the normal build.

## Boot-time replacement restart

- The first real installation of LiberNT release `ebe3308` (1,546 files including the kernel, smss and ntdll) with client `v0.3.0` failed. luupboot replaced every file without an error, but the boot then continued with the old kernel, smss and ntdll on top of the new files, and smss crashed (critical process failure; the debug build stopped in the debugger). The deletion of the `applying` marker had not reached the disk, so the next boot rolled the whole update back.
- Ruled out: the system call tables did not change between the two releases, and replacing a mapped image by rename does not corrupt the running process. LiberNT allows that rename, which Windows refuses, but the process keeps reading the original content.
- Fix `fb4b214`: luupboot restarts right after applying a plan or a rollback, so nothing runs on mixed files and the shutdown flushes the plan state. The same installation then succeeded at the first attempt and the service confirmed it in the next boot. The UEFI update and rollback test passed again with the automatic restart.
- Clients `af80b3dd8361356cdc4839febcde44c0313cdceb0344dd434430af30079a878a` (fix build) passed 128 checks and 9 HTTP checks on both test installations.

## Registry performance

The test VM had 2 CPUs and 2 GiB RAM. The real factory export had 6,884,571 bytes, 16,382 keys and 26,947 values. Two runs took 565-567 ms to read and parse the JSON and 861-872 ms to compare identical baselines. The command is `boot_vm.exe registry-snapshot <file>`.

With identical signed hashes this work is now skipped. The numbers cover registry processing only, not the network, the file comparison or a whole installation.

## Release status

- Client `v0.3.0` was released as stable (`LiberNTUpdate.exe` `a50d8a62614f28c3795dbb55896cd6d9fbd0846bf781edfaeb634f96e6b50455`, `luupboot.exe` `d4ec7524f0ab45c0821944e0b60a97eebe130082371c5b492354c232c5c01d2f`). It lacks the boot-time restart and must not be used for OS updates.
- Client `v0.3.1` is the current stable release with the restart fix (`LiberNTUpdate.exe` `4978e9ba97734bccddeb6e44a12e23c8a41275fef513498294705b59511bf7b4`, `luupboot.exe` `929fe3604ababcd2617473e8b0daef6930d3c2efbe83311d1900e3633eb26f2d`).
- The feed carries release `20261105-201529-ebe3308cdd`: LiberNT from `nks` with the kernel fix and client `v0.3.1`. A test installation running `v0.3.0` updated itself to `v0.3.1` from the feed by a direct install of the two client files. The registry v2 feed is not public yet.
- The older test installation was reinstalled from the `5fccfb5` image; older installations are to be reinstalled as described in the README.

## Findings from the tests

The original `BootExecute` runs before `NtInitializeRegistry`. SOFTWARE is not loaded yet, and `CmFlushKey` can report success without writing because of `CmpNoWrite`. The first real test therefore correctly refused the write and restored the original files. Loading the hive temporarily would not fix the cause, so the `UpdateExecute` phase and a check for it are required; the marker lives in the volatile `HARDWARE` hive so it does not survive a return to an older SMSS.

Cppcheck with the paths and switches of the real build found no problem in the new `update.c`. It reports an existing `uninitvar` at `sminit.c:957`, also present in the previous revision; that unrelated code was not changed.

The first UEFI test of the plain factory image `5fccfb5` on Q35 crashed before the updater with `0x7B / 0xC0000034`. The debugger confirmed the ARC path to the second partition and that `storahci` and `disk.sys` were loaded, but no disk device was created. The Q35 profile had two AHCI controllers and the disk sat behind more PCI bridges. The same image booted to the desktop through OVMF on the simpler PC profile. The cause of the Q35 difference is not confirmed and the crash state including RAM is kept for analysis.

The kernel writes `FirmwareBootDevice` from the loader's partition and `SystemBootDevice` from the Windows partition at every boot. The first value helps to find the EFI partition, but when the UEFI loader cannot find it, it substitutes the OS partition without any flag. Reading this value alone is therefore not enough to update the loader safely; the partition and the expected loader must be verified and, on a conflict, `bootRoot` is still required.

## Remaining work

- The private v2 test feed (16 releases) passed signature and object verification. The first registry migration plan from factory baseline `9d41077` to `5fccfb5` was computed read-only on an older installation: 11 new keys and values in SOFTWARE (an OpenGL driver and font substitutes), one local change kept, nothing to remove. A full client check with a separate data folder gave the same result, with the v2 signature, the manifests, both registry objects and SMSS `UpdateExecute` support verified. The plan has not been applied and the v2 feed is not public.
- Verify EFI updates and independent recovery on physical UEFI hardware; they passed in VMs on MBR and GPT.
- LiberNT kernel fix `[NTOS:IO]` (commit `25861e4`) takes the boot driver service path from the loader's list. It is on branch `nks-fix/ntos-io-boot-driver-registry-path` of the fork, ready for an upstream pull request, and merged into `nks`. The earlier OS changes have their own branches as well: `nks-feat/smss-update-execute` (`UpdateExecute`) and `nks-fix/cmlib-dirty-cell-blocks` (flushing multi-block registry cells). Our branches in the fork start with `nks-`; the others are copies of upstream branches. In the VM the updater installed the patched kernel (`ntkrnlmp.exe`) over two restarts and a driver whose service name differs from its file started.
- Secure Boot needs the OS build to sign its loaders; the updater works with signed loaders in the VM.
