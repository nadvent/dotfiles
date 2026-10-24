# 🌌 Dotfiles

> Personal dotfiles repository managed cleanly with [chezmoi](https://www.chezmoi.io/), configured for **Arch Linux / CachyOS**, featuring **Umbriel** Wayland compositor and **Noctalia** shell.

---

## 📑 Table of Contents

- [Overview](#-overview)
- [Tools & Applications](#-tools--applications)
  - [Desktop & Compositor](#desktop--compositor)
  - [Terminal & Shell](#terminal--shell)
  - [Development & Runtimes](#development--runtimes)
  - [Modern CLI Utilities](#modern-cli-utilities)
  - [Custom Scripts & Daemons](#custom-scripts--daemons)
- [Repository Structure](#-repository-structure)
- [Installation & Setup](#-installation--setup)
  - [1. Prerequisites](#1-prerequisites)
  - [2. Initialize with Chezmoi](#2-initialize-with-chezmoi)
  - [3. Run System Bootstrap](#3-run-system-bootstrap)
- [Services Setup](#-services-setup)
  - [Kanata (Keyboard Remapper)](#1-kanata-keyboard-remapper)
  - [Yazi D-Bus File Manager Service](#2-yazi-d-bus-filemanager1-service)
- [Scratchpad & Keybind Highlights](#-scratchpad--keybind-highlights)
- [Chezmoi Workflow & Maintenance](#-chezmoi-workflow--maintenance)

---

## 🔭 Overview

This repository contains my personal system configurations, custom helper scripts, and application profiles. Key goals of this setup:

- **Reproducibility**: Automated bootstrapping across fresh Arch / CachyOS installations via `bootstrap.sh`.
- **Separation of Concerns**: Non-interactive dotfiles are managed via Chezmoi, while repo-internal scripts, documentation, and sensitive files are filtered using `.chezmoiignore` and `.gitignore`.

---

## 🛠️ Tools & Applications

### Desktop & Compositor

- **Compositor**: [Umbriel](https://docs.noctalia.dev/umbriel/) (dynamic scrolling Wayland tiling window manager with scratchpad and HDR)
- **Shell & Widgets**: [Noctalia V5](https://docs.noctalia.dev/) (desktop bar, app launcher, notification daemon, control center, M3 palette generator)
- **Terminal**: [Ghostty](https://ghostty.org/) (native GPU-accelerated terminal with [GLSL cursor shaders](https://github.com/sahaj-b/cursor-shaders) and floating scratchpad profiles)
- **Browsers**: [Zen Browser](https://zen-browser.app/) (primary with profiles and containers), [Qutebrowser](https://qutebrowser.org/) (lightweight floating quick-search scratchpad with vim-like motions)
- **File Managers**: [Yazi](https://yazi-rs.github.io/) (blazing-fast terminal file manager with vim-motions, bookmarks and shortcuts)
- **Media & Audio**: [Astra](https://github.com/AstraPlayer/Astra) (audiophile music player), PipeWire / WirePlumber
- **Virtualization**: [Looking Glass](https://looking-glass.io/) (low-latency KVM frame-relay client)
- **Document Viewer**: [Zathura](https://pwmt.org/projects/zathura/) (with MuPDF backend)

### Terminal & Shell

- **Shell**: [Fish](https://fishshell.com/) 3.6+ with universal variables, aliases, and abbreviations
- **Prompts & Shell History**: [Fastfetch](https://github.com/fastfetch-cli/fastfetch), [Atuin](https://atuin.sh/) (shell history synchronization)
- **Editor**: [Neovim](https://neovim.io/) & [Zed](https://zed.dev/) (`zeditor`)

### Development & Runtimes

- **Rust**: Toolchain managed via `rustup` (`stable`), `rust-analyzer`, `clippy`, and `cargo-binstall`
- **Node.js**: Node runtime, `npm`, `pnpm`
- **Python**: [uv](https://astral.sh/uv) (fast Python package and environment manager)
- **Polyglot Manager**: [mise](https://mise.jdx.dev/) for language runtime versioning

### Modern CLI Utilities

| Tool                 | Drop-in / Purpose                                          |
| :------------------- | :--------------------------------------------------------- |
| `eza`                | Modern replacement for `ls` with Git status and file icons |
| `bat`                | Cat clone with syntax highlighting and Git integration     |
| `zoxide`             | Smarter, learning `cd` command                             |
| `ripgrep` (`rg`)     | Fast recursive regex search                                |
| `fd`                 | Fast, user-friendly alternative to `find`                  |
| `lazygit` (`lg`)     | Simple terminal UI for git commands                        |
| `lazydocker` (`ldo`) | Terminal UI for Docker containers                          |
| `gh`                 | Official GitHub CLI                                        |
| `dust`               | Graphical disk space usage viewer                          |
| `pydf`               | Colorized `df` disk filesystem usage                       |
| `wl-clipboard`       | Wayland clipboard utilities (`wl-copy`, `wl-paste`)        |

### Custom Scripts & Daemons

Custom scripts located in `~/.local/bin/` (tracked in `dot_local/bin/`):

- `escape-scratchpad`: Queries active Umbriel scratchpad surfaces via `umbriel windows --json` and dismisses them.
- `ghostty-scratchpad`: Dedicated instance with custom scratchpad configuration and window class.
- `qutebrowser-scratchpad`: Togglable floating browser window for quick searches.
- `toggle-audio.sh`: Quick audio output toggle between Desktop Speakers and Behringer UMC204HD audio interface with desktop notifications.
- `lg_vm.sh`: Background loop detecting shared memory `/dev/shm/looking-glass` to auto-launch Looking Glass client.
- `yazi-filemanager1`: Python D-Bus daemon implementing `org.freedesktop.FileManager1` for Yazi file choosing.

---

## 📂 Repository Structure

```text
~/.local/share/chezmoi/
├── .chezmoiignore
├── .gitignore
├── README.md
├── bootstrap.sh
├── scripts/
│   ├── install-yazi-plugins.sh  # Install/sync Yazi plugins from package.toml
│   └── manage-assets.sh         # Script to inventory, import, and run scripts & .desktop files
├── dot_config/
│   ├── fish/
│   ├── ghostty/
│   ├── git/
│   ├── kanata/
│   ├── noctalia/
│   ├── qutebrowser/
│   ├── systemd/user/
│   ├── umbriel/
│   ├── yazi/
│   └── zathura/
└── dot_local/
    ├── bin/
    └── share/applications/
```

---

## 🚀 Installation & Setup

### 1. Prerequisites

Ensure Arch Linux or CachyOS is installed with network access:

```bash
sudo pacman -S --needed git base-devel
```

### 2. Initialize with Chezmoi

```bash
# Install chezmoi if needed
sudo pacman -S --needed chezmoi

chezmoi init <your-repo-url>

# Review changes first
chezmoi diff

chezmoi apply
```

### 3. Run System Bootstrap

Run the included [`bootstrap.sh`](file:///home/nazhanys/.local/share/chezmoi/bootstrap.sh) to install all packages, language runtimes, fonts, and enable user services:

```bash
cd ~/.local/share/chezmoi

# Test first (optional)
./bootstrap.sh --dry-run

./bootstrap.sh --all
```

#### Selective Bootstrap Flags

```bash
./bootstrap.sh --cli      # CLI and tools
./bootstrap.sh --gui      # GUI apps, fonts
./bootstrap.sh --rust     # Rustup, components, and Kanata
./bootstrap.sh --node     # Node.js, NPM, and pnpm
./bootstrap.sh --yazi     # Yazi plugins and companion tools (ouch, glow)
./bootstrap.sh --assets   # Deploy scripts, desktop entries, and user services
```

---

## ⚙️ Services Setup

Two user systemd services run in the background to handle keyboard remapping and desktop file-manager D-Bus requests.

### 1. Kanata (Keyboard Remapper)

Remaps keys (such as CapsLock to Esc/Control) based on [kanata.kbd](file:///home/nazhanys/.local/share/chezmoi/dot_config/kanata/kanata.kbd).

1. Grant `uinput` permissions if not already present:
   ```bash
   sudo usermod -aG input,uinput "$USER"
   echo 'KERNEL=="uinput", MODE="0660", GROUP="uinput", OPTIONS+="static_node=uinput"' | sudo tee /etc/udev/rules.d/99-input.rules
   sudo udevadm control --reload && sudo udevadm trigger
   ```
2. Enable and start the service:
   ```bash
   systemctl --user daemon-reload
   systemctl --user enable --now kanata.service
   systemctl --user status kanata.service
   ```

### 2. Yazi D-Bus (FileManager1) Service

Allows browsers and applications (Zen, Chrome, Firefox) to trigger `Open Containing Folder` directly inside Yazi within a floating Ghostty window.

1. Ensure `python` and `python-gobject` are installed:
   ```bash
   sudo pacman -S --needed python python-gobject
   ```
2. Enable and start the service:
   ```bash
   systemctl --user daemon-reload
   systemctl --user enable --now yazi-filemanager1.service
   systemctl --user status yazi-filemanager1.service
   ```

---

## ⌨️ Scratchpad & Keybind Example

Some Umbriel keybindings. See [`dot_config/umbriel/cfg/keybinds.toml`](file:///home/nazhanys/.local/share/chezmoi/dot_config/umbriel/cfg/keybinds.toml):

| Keybinding             | Action / Command                                          |
| :--------------------- | :-------------------------------------------------------- |
| `Mod`                  | Toggle Noctalia Application Launcher                      |
| `Mod + Return`         | Toggle Terminal Scratchpad (`ghostty-scratchpad`)         |
| `Mod + Space`          | Toggle Quick Search Scratchpad (`qutebrowser-scratchpad`) |
| `Mod + N`              | Toggle Notes Scratchpad (`obsidian`)                      |
| `Mod + M`              | Toggle Music Scratchpad (`astra`)                         |
| `Mod + Escape`         | Dismiss / hide any currently visible scratchpad           |
| `Mod + B`              | Launch Zen Browser                                        |
| `Mod + Shift + E`      | Launch Zed Editor                                         |
| `Mod + E`              | Launch Yazi File Manager                                  |
| `Mod + F12`            | Toggle Audio Sink (Speakers ⟷ Behringer UMC204HD)         |
| `Ctrl + Shift + 1/2/3` | Screenshot (Region / Fullscreen / Annotate)               |

---

## 🔄 Chezmoi Workflow & Maintenance

### Daily Commands

```bash
# Edit a managed file (opens in $EDITOR)
chezmoi edit ~/.config/fish/config.fish

# Check differences between repo state and $HOME
chezmoi diff

# Apply pending changes from repo to $HOME
chezmoi apply

# Add a newly created configuration file to chezmoi
chezmoi add ~/.config/new-app/config.toml
```

### Managing Scripts & Desktop Entries

Use the asset management helper script:

```bash
# Check status of all referenced scripts and desktop entries
./scripts/manage-assets.sh status

# Re-import modified target files from $HOME into chezmoi
./scripts/manage-assets.sh import

# Deploy tracked assets from chezmoi repo to $HOME (~/.local/bin, etc.)
./scripts/manage-assets.sh deploy
```

---

## 📄 License

This repository is maintained for personal use and shared under the [MIT License](LICENSE). Feel free to adapt and borrow configurations.
