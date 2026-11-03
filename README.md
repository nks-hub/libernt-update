# LiberNT Update

[![CI](https://github.com/nks-hub/libernt-update/actions/workflows/ci.yml/badge.svg)](https://github.com/nks-hub/libernt-update/actions/workflows/ci.yml)
[![Platform](https://img.shields.io/badge/platform-LiberNT%20amd64-blue)](https://github.com/nks-hub/LiberNT)
[![C++](https://img.shields.io/badge/C%2B%2B-17-00599C)](client/build.cmd)
[![Python](https://img.shields.io/badge/python-3.12%2B-3776AB)](server/luup.py)
[![License](https://img.shields.io/badge/license-MIT-green)](LICENSE)

Updates an installed [LiberNT](https://github.com/nks-hub/LiberNT) system to a newer build file by file, with backups and rollback, instead of reinstalling it.

`server/luup.py` runs on the build machine: it reads the NTFS system partition of `LiberNT.img` (`ninja liberntimg`), hashes the serviced files under `Windows/`, `Program Files/` and `Program Files (x86)/`, and publishes a release. `client/` builds `LiberNTUpdate.exe` (Win32, C++17, static CRT), which runs inside LiberNT. Its user interface is in Czech.

## Features

- 🔏 **Signed feed** - `feed.json` is checked against the Ed25519 key compiled into the client; manifests and files are checked by SHA-256.
- 🔍 **Diff against the disk** - only files whose contents differ from the release are downloaded, whatever build the machine started from.
- 🧩 **Setup variants** - the kernel and HAL keep the variant Setup installed (`ntoskrnl.exe` copied from `ntkrnlmp.exe` on multiprocessor machines).
- 🔁 **Restart-time replacement** - when any changed file is in use, every changed file is replaced together at the next boot through `PendingFileRenameOperations`, so old and new core files never run side by side.
- ✅ **Boot verification** - the `LiberNTUpdate` service checks every replaced file after the restart and rolls the update back when one does not match; the rollback restores only the files that actually changed.
- ⏪ **Rollback and history** - each update keeps a backup of the files it replaced; the last update can be undone, a pending one cancelled, and every transaction is listed.
- 📦 **Releases only on change** - a commit whose image carries the same files as the previous release is not published again.

## Requirements

- Build machine: Debian 13 (or another Linux) with `python3` 3.12+, `python3-nacl`, `p7zip-full` and a configured LiberNT build tree.
- Client build: Visual Studio 2022 or newer with the x64 C++ tools.
- Target: LiberNT amd64 with a kernel from [nks-hub/LiberNT](https://github.com/nks-hub/LiberNT) branch `nks` (see [Limitations](#limitations)).

## Installation

Create the signing key once and put its public half into the client:

```sh
python3 server/luup.py keygen --key /opt/luup/signing.key     # prints the public key
```

Replace `kFeedPublicKeyHex` in `client/src/pubkey.h` with that key, then build the client on Windows:

```bat
client\build.cmd
```

Copy `client\out\LiberNTUpdate.exe` into LiberNT (for example to `C:\Program Files\LiberNT Update\`) and create `%ProgramData%\LiberNTUpdate\config.json`:

```json
{"feedUrl": "http://updates.example.org/", "root": "C:\\"}
```

`set LNTU_FEED_URL=http://updates.example.org/` before `build.cmd` compiles a default feed URL into the client instead.

## Usage

Publish a release from a finished build and check the feed:

```sh
python3 server/luup.py publish --key /opt/luup/signing.key --feed /srv/luup \
        --img output-Clang-amd64-debug/LiberNT.img --commit "$(git rev-parse HEAD)" --repo .
python3 server/luup.py verify  --feed /srv/luup --pub "$(cat /opt/luup/signing.pub)"
```

Serve `/srv/luup` over HTTP (`python3 -m http.server 8080 --directory /srv/luup` is enough on a LAN).

Inside LiberNT, `LiberNTUpdate.exe` without arguments opens the window. The command line does the same work:

| Command | Action |
|---|---|
| `/check` | compare the newest release with this system and list the changed files |
| `/install` | install it; files in use are replaced at the next restart |
| `/restart` | restart the computer |
| `/rollback` | undo the most recent update |
| `/cancel` | withdraw an update that waits for a restart |
| `/finalize` | finish a pending update (the service does this at boot) |
| `/history` | list past updates |
| `/selftest` | built-in checks |

The program is a GUI-subsystem executable, so command-line output also goes to `%ProgramData%\LiberNTUpdate\cli-output.txt`; from `cmd.exe` run it with `start "" /wait LiberNTUpdate.exe /check`.

## Automatic releases

`server/build-release.sh` fetches branch `nks` of nks-hub/LiberNT, runs `ninja liberntimg` and publishes the image when the commit is not in the feed yet. `server/systemd/luup-release.timer` runs it daily at 04:30 UTC, after the fork's GitHub workflow has merged upstream `main-nt10` into `nks` at 03:17 UTC.

```sh
install -m 755 server/build-release.sh /opt/luup/build-release.sh
cp server/luup.py /opt/luup/
cp server/systemd/luup-release.* /etc/systemd/system/
systemctl daemon-reload && systemctl enable --now luup-release.timer
```

`SRC`, `OUT`, `FEED`, `KEY` and `LUUP` override the default paths.

## Limitations

- Registry changes of a new build (new drivers or services in `hivesys.inf`) are not applied.
- The EFI partition (boot manager, `BCD`) is not updated.
- A build that does not boot cannot be rolled back from inside the system; take a VM snapshot first.
- Kernels without nks-hub/LiberNT commit `2f303f8` (`fix: mark every hive block a dirty registry cell spans`) write only the first 4 KB block of a large registry value to disk. A `PendingFileRenameOperations` list of several hundred files then reaches the next boot mixed with older entries and only part of the update lands. On such a system copy the new `ntkrnlmp.exe` (or `ntoskrnl.exe` on single-CPU machines) over `system32\ntoskrnl.exe`, restart, then update.

## Testing

```sh
python3 server/luup.py test      # publisher: diff, dedup, exclusions, variants, skipped empty release, signature
```

```bat
client\build.cmd
start "" /wait client\out\LiberNTUpdate.exe /selftest
type %ProgramData%\LiberNTUpdate\cli-output.txt
```

The client self-test covers SHA-256 and Ed25519 vectors, direct install, rollback (including a partly applied update), the restart path (scheduling and cancelling restart-time renames) and a full check against a signed local feed with kernel variants. CI runs both on every push.

## Contributing

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/my-change`)
3. Commit your changes (`git commit -m "feat: add my change"`)
4. Push the branch (`git push origin feature/my-change`)
5. Open a pull request

## Support

- 📧 **Email:** dev@nks-hub.cz
- 🐛 **Bug reports:** [GitHub Issues](https://github.com/nks-hub/libernt-update/issues)

## License

MIT License — see [LICENSE](LICENSE) for details. Monocypher (`client/third_party/monocypher`, BSD-2-Clause or CC0) and nlohmann/json (`client/third_party/nlohmann`, MIT) keep their licences.

---

<p align="center">Made with ❤️ by <a href="https://github.com/nks-hub">NKS Hub</a></p>
