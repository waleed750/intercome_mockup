# Dev machine setup (Ubuntu, same LAN as the panel)

This sets up a **fresh Ubuntu machine** for working on `intercom_mockup`
(the `mock_door` Python simulator + the `packages/intercom_core` /
`packages/intercom` Flutter/Dart packages, including the `flutterpi/` native
plugin consumed by the `syncn_smarthome_panel` repo). The machine should be on
the **same local network as the physical panel** so `mock_door` can be
discovered by it and so you can SSH in to test/deploy against real hardware.

Everything below is automated by [`scripts/setup_ubuntu_dev.sh`](scripts/setup_ubuntu_dev.sh).
Read this once, then just run the script.

## What the script installs

| Area | What | Why |
|---|---|---|
| Base tooling | `build-essential`, `git`, `curl`, `unzip`, `openssh-server`, `ufw` | general dev + SSH in/out of this box |
| Python | `python3`, `pip`, `venv`, `ffmpeg`, `portaudio19-dev`, `libgl1` | runs `mock_door` (fake door station: video via ffmpeg, mic/speaker via `sounddevice`, video via `opencv-python`) |
| `mock_door` deps | installed into `mock_door/.venv-linux` from `mock_door/requirements.txt` | isolated venv, doesn't touch system Python |
| flutter-pi build deps | `cmake`, `ninja-build`, `clang`, GStreamer **dev** headers (`libgstreamer1.0-dev` + base/good/bad plugins, `gstreamer1.0-libav`), `libdrm-dev`, `libgbm-dev`, EGL/GLES dev libs, `libinput-dev`, `libudev-dev`, `libsystemd-dev` | needed to compile/test the `packages/intercom_core/flutterpi/` native audio/video plugin (see that folder's README) — the panel's runtime packages are a separate, smaller set installed *on the panel itself* |
| Flutter SDK | pinned to `3.44.3` (matches `flutter: ">=3.22.0"` in this repo's `pubspec.yaml` files and the version `syncn_smarthome_panel` builds with) | build/test the Dart packages |
| GitHub CLi (`gh`) | latest | clone the private `syncn_smarthome_panel` repo, open PRs |
| Claude Code CLI | latest, via the official installer | AI-assisted dev in this repo |
| OpenCode CLI | latest, via the official installer | AI-assisted dev, alternate agent |
| Git identity | prompts once if `user.name`/`user.email` aren't set globally | commit authorship |
| SSH | generates an `ed25519` key if missing, adds a `Host panel` alias to `~/.ssh/config` | passwordless SSH to the physical panel once you run `ssh-copy-id panel` |

The script is **idempotent** — safe to re-run; it skips anything already installed.

## Panel SSH access

The panel this dev machine talks to is the same one `syncn_smarthome_panel`'s
own `scripts/setup_ubuntu_dev.sh` targets:

- Host: `192.168.100.180`
- User: `linaro`

The script saves this as an alias in `~/.ssh/config`:

```
Host panel
  HostName 192.168.100.180
  User linaro
  StrictHostKeyChecking accept-new
```

If the panel's IP or user has changed, override before running:

```bash
PANEL_USER=linaro PANEL_HOST=192.168.100.xxx ./scripts/setup_ubuntu_dev.sh
```

After the script finishes, enable passwordless login once (needs the panel's
password interactively):

```bash
ssh-copy-id panel
ssh panel   # should now log in with no password
```

From then on, anything in this repo or `syncn_smarthome_panel`'s own scripts
(`deploy_to_panel.sh`, `journalctl -u syncnhome.service -f`, etc.) that shells
out to `linaro@192.168.100.180` will work unattended.

## Usage

```bash
git clone <this-repo-url> intercom_mockup
cd intercom_mockup
chmod +x scripts/setup_ubuntu_dev.sh
./scripts/setup_ubuntu_dev.sh
source ~/.bashrc   # pick up PATH changes (Flutter, claude, opencode)
```

### Manual steps after the script (things it can't do non-interactively)

1. `ssh-copy-id panel` — panel password required once.
2. `claude` — run once and follow the login prompt.
3. `opencode auth login` — authenticate OpenCode with your model provider/API key.
4. `gh auth login` — authenticate GitHub CLI (needed for private repo clones).

### Verifying everything works

```bash
flutter doctor -v                                   # Flutter/Dart toolchain
cd packages/intercom_core && flutter test            # Dart unit tests (protocol/codec)
source mock_door/.venv-linux/bin/activate
python mock_door/mock_door.py                        # starts the fake door station on this LAN
ssh panel "echo ok"                                   # confirms passwordless SSH to the real panel
claude --version
opencode --version
```

`mock_door` replies to the same UDP discovery (port 8089) / TCP call (port
8189) protocol the real door station uses, so a panel build on this network
should be able to discover and call it — see the root [`README.md`](README.md)
and [`mock_door/PLAN.md`](mock_door/PLAN.md) for the wire protocol details.

## Notes / things to double check

- The GStreamer dev-header list mirrors what
  [`packages/intercom_core/flutterpi/README.md`](packages/intercom_core/flutterpi/README.md)
  says the *app repo's* CI needs to add for compiling the plugin — it is **not**
  sufficient to actually run flutter-pi standalone on this x86 dev box (that
  needs `flutter_embedder.h`, fetched by flutter-pi's own CMake configure step,
  plus DRM/KMS hardware); it's here so you can compile-check the plugin changes
  before pushing.
- If `ssh panel` fails after `ssh-copy-id`, check the panel is powered on and
  actually on `192.168.100.180` on this same subnet (`ip a` / `arp -a` on this
  machine, or the panel's own display if it shows an IP).
- The script does not touch anything on the panel itself — panel-side package
  installs live in `syncn_smarthome_panel/scripts/install_debian_panel_remote.sh`
  in the other repo.
