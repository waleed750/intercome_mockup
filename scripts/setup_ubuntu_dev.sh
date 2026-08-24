#!/usr/bin/env bash
# Bootstrap a fresh Ubuntu dev machine for intercom_mockup work, on the same LAN
# as the physical SyncN panel. Installs: base build tooling, Python3 + mock_door
# deps (ffmpeg/portaudio/opencv runtime libs), the Flutter SDK (pinned to match
# packages/*/pubspec.yaml), flutter-pi's native build dependencies (GStreamer dev
# headers + CMake/EGL/DRM libs, for building/testing the flutterpi/ plugin), the
# Claude Code CLI, the OpenCode CLI, GitHub CLI, and an SSH alias to reach the
# panel over the local network.
#
# Usage: chmod +x scripts/setup_ubuntu_dev.sh && ./scripts/setup_ubuntu_dev.sh
#
# Panel connection defaults to linaro@192.168.100.180 (same panel the
# syncn_smarthome_panel repo's scripts/setup_ubuntu_dev.sh targets -- override
# via PANEL_USER / PANEL_HOST env vars before running if it has moved).
set -euo pipefail

PANEL_USER="${PANEL_USER:-linaro}"
PANEL_HOST="${PANEL_HOST:-192.168.100.180}"
FLUTTER_VERSION="${FLUTTER_VERSION:-3.44.3}"

REPO_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"

log() { echo -e "\n\033[1;32m==> $*\033[0m"; }
warn() { echo -e "\033[1;33m!! $*\033[0m" >&2; }

if [[ $EUID -eq 0 ]]; then
  echo "Run this as a normal user with sudo privileges, not as root." >&2
  exit 1
fi

log "Updating apt and installing base packages"
sudo apt-get update -y
sudo apt-get install -y \
  build-essential \
  curl \
  wget \
  git \
  unzip \
  xz-utils \
  zip \
  ca-certificates \
  gnupg \
  software-properties-common \
  openssh-server \
  net-tools \
  htop \
  ufw

log "Enabling and starting SSH server (so this machine is reachable too)"
sudo systemctl enable ssh
sudo systemctl start ssh
sudo ufw allow OpenSSH || true

log "Installing Python 3 + mock_door runtime dependencies"
sudo apt-get install -y \
  python3 \
  python3-pip \
  python3-venv \
  ffmpeg \
  libportaudio2 \
  portaudio19-dev \
  libgl1 \
  libglib2.0-0
python3 --version

log "Creating a venv for mock_door and installing its Python requirements"
MOCK_DOOR_VENV="$REPO_DIR/mock_door/.venv-linux"
if [[ ! -d "$MOCK_DOOR_VENV" ]]; then
  python3 -m venv "$MOCK_DOOR_VENV"
fi
"$MOCK_DOOR_VENV/bin/pip" install --upgrade pip
"$MOCK_DOOR_VENV/bin/pip" install -r "$REPO_DIR/mock_door/requirements.txt"
log "mock_door venv ready. Activate with: source mock_door/.venv-linux/bin/activate"

log "Installing flutter-pi native build dependencies (GStreamer dev headers, EGL/DRM/input libs)"
sudo apt-get install -y \
  cmake \
  ninja-build \
  clang \
  pkg-config \
  libgstreamer1.0-dev \
  libgstreamer-plugins-base1.0-dev \
  libgstreamer-plugins-good1.0-dev \
  libgstreamer-plugins-bad1.0-dev \
  gstreamer1.0-libav \
  gstreamer1.0-alsa \
  libdrm-dev \
  libgbm-dev \
  libegl1-mesa-dev \
  libgles2-mesa-dev \
  libinput-dev \
  libudev-dev \
  libxkbcommon-dev \
  libsystemd-dev

FLUTTER_DIR="$HOME/development"
log "Installing Flutter SDK $FLUTTER_VERSION (pinned to match this repo's pubspec.yaml constraints)"
mkdir -p "$FLUTTER_DIR"
if [[ ! -d "$FLUTTER_DIR/flutter" ]]; then
  git clone https://github.com/flutter/flutter.git -b stable "$FLUTTER_DIR/flutter"
fi
git -C "$FLUTTER_DIR/flutter" checkout "$FLUTTER_VERSION"
export PATH="$PATH:$FLUTTER_DIR/flutter/bin"
flutter --version

FLUTTER_PATH_LINE='export PATH="$PATH:'"$FLUTTER_DIR"'/flutter/bin"'
for rcfile in "$HOME/.bashrc" "$HOME/.zshrc"; do
  if [[ -f "$rcfile" ]] && ! grep -qF "$FLUTTER_PATH_LINE" "$rcfile"; then
    echo "$FLUTTER_PATH_LINE" >> "$rcfile"
  fi
done

log "Running flutter precache and doctor"
flutter precache --linux
flutter config --enable-linux-desktop
flutter doctor -v || true

log "Fetching Dart/Flutter package dependencies for this repo's packages"
for pkg in "$REPO_DIR/packages/intercom_core" "$REPO_DIR/packages/intercom"; do
  if [[ -f "$pkg/pubspec.yaml" ]]; then
    (cd "$pkg" && flutter pub get) || warn "flutter pub get failed in $pkg"
  fi
done

log "Installing GitHub CLI (gh) -- needed to clone the private syncn_smarthome_panel repo"
if ! command -v gh >/dev/null 2>&1; then
  sudo install -m 0755 -d /etc/apt/keyrings
  curl -fsSL https://cli.github.com/packages/githubcli-archive-keyring.gpg | sudo gpg --dearmor -o /etc/apt/keyrings/githubcli-archive-keyring.gpg
  sudo chmod a+r /etc/apt/keyrings/githubcli-archive-keyring.gpg
  echo "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/githubcli-archive-keyring.gpg] https://cli.github.com/packages stable main" | \
    sudo tee /etc/apt/sources.list.d/github-cli.list > /dev/null
  sudo apt-get update -y
  sudo apt-get install -y gh
else
  log "gh already installed"
fi

log "Installing Claude Code CLI"
if ! command -v claude >/dev/null 2>&1; then
  curl -fsSL https://claude.ai/install.sh | bash
  export PATH="$PATH:$HOME/.local/bin"
  CLAUDE_PATH_LINE='export PATH="$PATH:$HOME/.local/bin"'
  for rcfile in "$HOME/.bashrc" "$HOME/.zshrc"; do
    if [[ -f "$rcfile" ]] && ! grep -qF "$CLAUDE_PATH_LINE" "$rcfile"; then
      echo "$CLAUDE_PATH_LINE" >> "$rcfile"
    fi
  done
else
  log "claude CLI already installed"
fi

log "Installing OpenCode CLI"
if ! command -v opencode >/dev/null 2>&1; then
  curl -fsSL https://opencode.ai/install | bash
  export PATH="$PATH:$HOME/.opencode/bin"
  OPENCODE_PATH_LINE='export PATH="$PATH:$HOME/.opencode/bin"'
  for rcfile in "$HOME/.bashrc" "$HOME/.zshrc"; do
    if [[ -f "$rcfile" ]] && ! grep -qF "$OPENCODE_PATH_LINE" "$rcfile"; then
      echo "$OPENCODE_PATH_LINE" >> "$rcfile"
    fi
  done
else
  log "opencode CLI already installed"
fi

log "Configuring local git identity (skipped if already set)"
if [[ -z "$(git config --global user.name || true)" ]]; then
  read -rp "Git user.name: " GIT_NAME
  git config --global user.name "$GIT_NAME"
fi
if [[ -z "$(git config --global user.email || true)" ]]; then
  read -rp "Git user.email: " GIT_EMAIL
  git config --global user.email "$GIT_EMAIL"
fi
git config --global init.defaultBranch main
git config --global pull.rebase false

log "Generating SSH key (if missing) and saving an alias for the panel"
mkdir -p "$HOME/.ssh"
chmod 700 "$HOME/.ssh"
if [[ ! -f "$HOME/.ssh/id_ed25519" ]]; then
  ssh-keygen -t ed25519 -f "$HOME/.ssh/id_ed25519" -N "" -C "$USER@$(hostname)"
fi
SSH_CONFIG="$HOME/.ssh/config"
touch "$SSH_CONFIG"
chmod 600 "$SSH_CONFIG"
if ! grep -q "^Host panel$" "$SSH_CONFIG" 2>/dev/null; then
  {
    echo ""
    echo "Host panel"
    echo "  HostName $PANEL_HOST"
    echo "  User $PANEL_USER"
    echo "  StrictHostKeyChecking accept-new"
  } >> "$SSH_CONFIG"
  log "Added 'Host panel' alias -> $PANEL_USER@$PANEL_HOST to $SSH_CONFIG"
else
  log "'Host panel' alias already present in $SSH_CONFIG"
fi

log "Done. Open a new shell (or run 'source ~/.bashrc') so PATH changes take effect."
log "Remaining manual steps:"
echo "  1. Run 'ssh-copy-id panel' once (panel password required) for passwordless SSH."
echo "     Panel target: $PANEL_USER@$PANEL_HOST (override with PANEL_USER/PANEL_HOST env vars if it changed)."
echo "     After that: ssh panel"
echo "  2. Run 'claude' once and follow the login prompt to authenticate Claude Code."
echo "  3. Run 'opencode auth login' to authenticate OpenCode with your model provider."
echo "  4. Run 'gh auth login' to authenticate the GitHub CLI (needed to clone private repos)."
log "Verifying the install:"
command -v ffmpeg >/dev/null && echo "  ffmpeg: OK"
command -v flutter >/dev/null && flutter --version | head -1
command -v claude >/dev/null && claude --version || echo "  claude CLI: run 'source ~/.bashrc' then retry"
command -v opencode >/dev/null && opencode --version || echo "  opencode CLI: run 'source ~/.bashrc' then retry"
command -v gh >/dev/null && gh --version | head -1
log "Machine IP addresses for SSH access into this dev box:"
hostname -I || true
