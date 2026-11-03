# LiberNT Update

Updates an installed LiberNT system to a newer build without reinstalling it.

- `server/luup.py` runs on the build machine. It reads the NTFS system partition of `LiberNT.img`
  (`ninja liberntimg`), hashes the serviced files (`Windows/`, `Program Files/`) and publishes a release:
  a manifest with SHA-256 per file, file contents stored once per hash, and `feed.json` signed with Ed25519.
- `client/` builds `LiberNTUpdate.exe` (Win32, C++17, static CRT) that runs inside LiberNT.

## Features

- 🔏 **Signed feed** - `feed.json` is checked against the Ed25519 key compiled into the client; manifests and files are checked by SHA-256.
- 🔍 **Diff against the disk** - only files whose contents differ from the release are downloaded, whatever build the machine started from.
- 🧩 **Setup variants** - the kernel and HAL keep the variant Setup installed (`ntoskrnl.exe` from `ntkrnlmp.exe` on MP machines).
- 🔁 **Restart-time replacement** - when any changed file is in use, every changed file is replaced together at the next boot through `PendingFileRenameOperations`, so old and new core files never run side by side.
- ✅ **Boot verification** - the `LiberNTUpdate` service checks every replaced file after the restart and rolls the update back when one does not match.
- ⏪ **Rollback and history** - each update keeps a backup of the files it replaced; the last update can be undone, and every transaction is listed.

## Requirements

- Build machine: Debian 13 with `python3`, `python3-nacl` and `p7zip-full`.
- Client build: Visual Studio 2022 (MSVC x64).

## Usage

Build machine:

```sh
python3 server/luup.py keygen  --key /opt/luup/signing.key            # once; prints the public key
python3 server/luup.py publish --key /opt/luup/signing.key --feed /srv/luup \
        --img output-Clang-amd64-debug/LiberNT.img --commit "$(git rev-parse HEAD)" --repo .
python3 server/luup.py verify  --feed /srv/luup --pub "$(cat /opt/luup/signing.pub)"
```

Serve `/srv/luup` over HTTP. The client reads the feed URL and the system drive from
`%ProgramData%\LiberNTUpdate\config.json` (`{"feedUrl": "http://host:8080/", "root": "C:\\"}`).

Inside LiberNT, `LiberNTUpdate.exe` opens the window; `/check`, `/install`, `/rollback`, `/cancel`, `/finalize`, `/history`,
`/restart` and `/selftest` do the same from the command line and write their output to
`%ProgramData%\LiberNTUpdate\cli-output.txt`.

## Testing

```sh
python3 server/luup.py test                 # publisher: diff, dedup, exclusions, variants, signature
client\build.cmd && client\out\LiberNTUpdate.exe /selftest
```

The client self-test covers SHA-256 and Ed25519 vectors, direct install, rollback, the restart path
(scheduling and cancelling restart-time renames) and a full check against a signed local feed.

## Limitations

- Registry changes of a new build (new drivers or services in `hivesys.inf`) are not applied.
- The EFI partition (boot manager, `BCD`) is not updated.
- A build that does not boot cannot be rolled back from inside the system; take a VM snapshot first.
- Kernels before LiberNT commit `4691c52` (`fix: mark every hive block a dirty registry cell spans`) write only
  the first 4 KB block of a large registry value to disk, so a `PendingFileRenameOperations` list of several hundred
  files reaches the next boot mixed with older entries and only part of the update lands. On such a system copy the
  new `ntkrnlmp.exe` (or `ntoskrnl.exe` on single-CPU machines) over `system32\ntoskrnl.exe`, restart, then update.

## License

Monocypher (`client/third_party/monocypher`, BSD-2-Clause or CC0) and nlohmann/json (MIT) keep their licences.
