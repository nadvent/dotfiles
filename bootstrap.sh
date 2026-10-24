#!/usr/bin/env bash
# ==============================================================================
# Arch Linux / CachyOS System Bootstrap Script
#
# Bootstraps development toolchains, shell utilities, pacman/AUR packages,
# Rust, Node.js/NPM, Python/uv, and systemd user services for Chezmoi dotfiles.
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DRY_RUN=0
INSTALL_CLI=1
INSTALL_GUI=1
INSTALL_RUST=1
INSTALL_NODE=1
INSTALL_PYTHON=1
INSTALL_YAZI=1
INSTALL_ASSETS=1
SETUP_SERVICES=1

# Color definitions
C_RESET='\033[0m'
C_BOLD='\033[1m'
C_BLUE='\033[34m'
C_GREEN='\033[32m'
C_YELLOW='\033[33m'
C_RED='\033[31m'
C_CYAN='\033[36m'
C_MAGENTA='\033[35m'

log_info()    { echo -e "${C_BLUE}${C_BOLD}[INFO]${C_RESET} $*"; }
log_success() { echo -e "${C_GREEN}${C_BOLD}[OK]${C_RESET} $*"; }
log_warn()    { echo -e "${C_YELLOW}${C_BOLD}[WARN]${C_RESET} $*"; }
log_error()   { echo -e "${C_RED}${C_BOLD}[ERROR]${C_RESET} $*"; }
log_step()    { echo -e "\n${C_MAGENTA}${C_BOLD}===>${C_RESET} ${C_BOLD}$*${C_RESET}"; }

run_cmd() {
    if [[ "${DRY_RUN}" -eq 1 ]]; then
        echo -e "${C_CYAN}[DRY-RUN]${C_RESET} $*"
    else
        "$@"
    fi
}

# ------------------------------------------------------------------------------
# 1. System & Privilege Checks
# ------------------------------------------------------------------------------
check_system() {
    log_step "Verifying System Compatibility"

    if [[ "${EUID}" -eq 0 ]]; then
        log_error "Please run this script as your regular user (not root). sudo will be invoked when needed."
        exit 1
    fi

    if [[ ! -f /etc/arch-release && ! -f /etc/cachyos-release ]] && ! grep -qi "arch\|cachyos" /etc/os-release 2>/dev/null; then
        log_warn "This script is tailored for Arch Linux and CachyOS. Proceeding on $(uname -s)..."
    else
        log_success "Detected Arch / CachyOS environment."
    fi

    # Ensure sudo credentials are fresh
    if [[ "${DRY_RUN}" -eq 0 ]]; then
        sudo -v
        # Keep sudo timestamp updated while script runs
        while true; do sudo -n true; sleep 60; kill -0 "$$" || exit; done 2>/dev/null &
    fi
}

# ------------------------------------------------------------------------------
# 2. Package Managers & AUR Helper
# ------------------------------------------------------------------------------
AUR_HELPER=""

detect_or_install_aur_helper() {
    log_step "Checking AUR Helper"

    if command -v paru >/dev/null 2>&1; then
        AUR_HELPER="paru"
        log_success "Found AUR helper: paru"
    elif command -v yay >/dev/null 2>&1; then
        AUR_HELPER="yay"
        log_success "Found AUR helper: yay"
    else
        log_info "Neither paru nor yay found. Attempting to install paru..."
        if [[ "${DRY_RUN}" -eq 0 ]]; then
            sudo pacman -S --needed --noconfirm base-devel git
            local tmpdir
            tmpdir="$(mktemp -d)"
            git clone https://aur.archlinux.org/paru-bin.git "${tmpdir}/paru-bin"
            (cd "${tmpdir}/paru-bin" && makepkg -si --noconfirm)
            rm -rf "${tmpdir}"
            AUR_HELPER="paru"
            log_success "Successfully installed paru."
        else
            AUR_HELPER="paru"
            log_info "[DRY-RUN] Would build and install paru-bin from AUR."
        fi
    fi
}

install_packages() {
    local target_list=("$@")
    if [[ ${#target_list[@]} -eq 0 ]]; then return; fi

    local to_install=()
    for pkg in "${target_list[@]}"; do
        if ! pacman -Qi "${pkg}" >/dev/null 2>&1; then
            to_install+=("${pkg}")
        fi
    done

    if [[ ${#to_install[@]} -gt 0 ]]; then
        log_info "Installing missing packages: ${to_install[*]}"
        run_cmd "${AUR_HELPER}" -S --needed --noconfirm "${to_install[@]}"
    else
        log_success "All packages in this set are already installed."
    fi
}

# ------------------------------------------------------------------------------
# 3. CLI & System Packages
# ------------------------------------------------------------------------------
setup_cli_packages() {
    log_step "Installing CLI, Shell & Development Core Packages"

    local CORE_PKGS=(
        # Build essentials & system utilities
        base-devel git curl wget rsync jq unzip
        # Wayland, clipboard, notifications & audio
        wl-clipboard libnotify pipewire-pulse
        # Python bindings for D-Bus (used by yazi-filemanager1)
        python python-gobject
        # Fish Shell & Navigation
        fish fastfetch atuin zoxide
        # Modern CLI alternatives (eza, bat, ripgrep, fd, dust, pydf)
        eza bat ripgrep fd dust pydf
        # Terminal Editors & Managers (+ companion previewers ouch, glow)
        neovim yazi ouch glow lazygit lazydocker github-cli
    )

    install_packages "${CORE_PKGS[@]}"

    # Ensure fish is in /etc/shells and set as login shell if requested
    local FISH_PATH
    FISH_PATH="$(command -v fish || true)"
    if [[ -n "${FISH_PATH}" && "${SHELL}" != "${FISH_PATH}" ]]; then
        if ! grep -qxF "${FISH_PATH}" /etc/shells 2>/dev/null; then
            log_info "Adding fish to /etc/shells..."
            run_cmd sudo sh -c "echo '${FISH_PATH}' >> /etc/shells"
        fi
        log_info "Setting default shell to ${FISH_PATH}..."
        run_cmd chsh -s "${FISH_PATH}" "${USER}" || log_warn "Could not change login shell automatically (run: chsh -s ${FISH_PATH})"
    fi
}

# ------------------------------------------------------------------------------
# 4. GUI Applications & Fonts
# ------------------------------------------------------------------------------
setup_gui_packages() {
    log_step "Installing GUI Applications, Media & Fonts"

    local GUI_PKGS=(
        # Terminal & Document Viewers
        ghostty zathura zathura-pdf-mupdf
        # Editors & File Managers
        zed nautilus
        # Browsers & Media
        qutebrowser obs-studio
        # KDE framework daemon (Umbriel autostart dependency)
        kded6
    )

    local AUR_PKGS=(
        # Browsers & Utilities from AUR
        zen-browser-bin
        astra-music-bin
        neovide
        obsidian
        looking-glass
    )

    install_packages "${GUI_PKGS[@]}"
    install_packages "${AUR_PKGS[@]}"

    # Font recommendations (Liga SFMono Nerd Font / Maple Mono)
    local FONT_PKGS=(
        ttf-nerd-fonts-symbols
        ttf-jetbrains-mono-nerd
    )
    install_packages "${FONT_PKGS[@]}" || log_warn "Some fonts might need manual installation or AUR lookup."
}

# ------------------------------------------------------------------------------
# 5. Rust Toolchain & Cargo Tools
# ------------------------------------------------------------------------------
setup_rust() {
    log_step "Configuring Rust Toolchain & Cargo Binaries"

    # Ensure rustup is installed
    if ! command -v rustup >/dev/null 2>&1; then
        log_info "Installing rustup..."
        install_packages rustup
    fi

    # Setup stable toolchain and standard components
    log_info "Updating Rust stable toolchain..."
    run_cmd rustup default stable
    run_cmd rustup component add rust-src rust-analyzer clippy

    # Cargo bin path
    local CARGO_BIN="${CARGO_HOME:-$HOME/.cargo}/bin"
    mkdir -p "${CARGO_BIN}"

    # Install cargo-binstall for fast pre-compiled binary installs
    if ! command -v cargo-binstall >/dev/null 2>&1; then
        log_info "Installing cargo-binstall..."
        run_cmd cargo install cargo-binstall --locked || log_warn "cargo-binstall failed to install, falling back to cargo install."
    fi

    # Install Kanata (keyboard remapper daemon used in kanata.service)
    if ! command -v kanata >/dev/null 2>&1 && [[ ! -x "${CARGO_BIN}/kanata" ]]; then
        log_info "Installing Kanata (keyboard remapper)..."
        if command -v cargo-binstall >/dev/null 2>&1; then
            run_cmd cargo-binstall --no-confirm kanata || run_cmd cargo install kanata --locked
        else
            run_cmd cargo install kanata --locked
        fi
        log_success "Kanata installed successfully."
    else
        log_success "Kanata binary is already installed."
    fi
}

# ------------------------------------------------------------------------------
# 6. Node.js, NPM & Runtime Managers
# ------------------------------------------------------------------------------
setup_node() {
    log_step "Configuring Node.js, NPM & Runtime Managers"

    # Install Node.js & npm if missing
    if ! command -v node >/dev/null 2>&1 || ! command -v npm >/dev/null 2>&1; then
        install_packages nodejs npm
    fi

    log_success "Node.js $(node --version 2>/dev/null || echo '') and NPM $(npm --version 2>/dev/null || echo '') verified."

    # Install pnpm and yarn if desired
    if ! command -v pnpm >/dev/null 2>&1; then
        log_info "Installing pnpm..."
        run_cmd sudo npm install -g pnpm 2>/dev/null || run_cmd npm install -g pnpm || log_warn "pnpm install skipped."
    fi

    # Ensure mise is installed (polyglot runtime manager)
    if ! command -v mise >/dev/null 2>&1 && [[ ! -x "${HOME}/.local/bin/mise" ]]; then
        log_info "Installing mise..."
        if command -v curl >/dev/null 2>&1; then
            run_cmd sh -c "curl https://mise.run | sh"
        fi
    fi
}

# ------------------------------------------------------------------------------
# 7. Python & UV
# ------------------------------------------------------------------------------
setup_python() {
    log_step "Configuring Python & UV"

    # Install uv (fast Python package manager)
    if ! command -v uv >/dev/null 2>&1 && [[ ! -x "${HOME}/.local/bin/uv" ]]; then
        log_info "Installing uv..."
        if command -v curl >/dev/null 2>&1; then
            run_cmd sh -c "curl -LsSf https://astral.sh/uv/install.sh | sh"
        fi
    else
        log_success "uv is already installed."
    fi
}

# ------------------------------------------------------------------------------
# 8. Yazi Plugins & Dependencies
# ------------------------------------------------------------------------------
setup_yazi_plugins() {
    log_step "Installing & Updating Yazi Plugins (package.toml)"

    local YAZI_SCRIPT="${SCRIPT_DIR}/scripts/install-yazi-plugins.sh"
    if [[ -x "${YAZI_SCRIPT}" ]]; then
        run_cmd "${YAZI_SCRIPT}" --discard
    elif command -v ya >/dev/null 2>&1; then
        run_cmd ya pkg install --discard
    else
        log_warn "Yazi CLI 'ya' not available, skipping plugin installation."
    fi
}

# ------------------------------------------------------------------------------
# 9. Manage Custom Scripts, Desktop Files & Systemd Units
# ------------------------------------------------------------------------------
setup_assets_and_services() {
    log_step "Deploying Custom Scripts, Desktop Entries & User Services"

    local MANAGE_SCRIPT="${SCRIPT_DIR}/scripts/manage-assets.sh"
    if [[ -x "${MANAGE_SCRIPT}" ]]; then
        run_cmd "${MANAGE_SCRIPT}" deploy
    else
        log_warn "Asset management script not found at ${MANAGE_SCRIPT}"
    fi

    if [[ "${SETUP_SERVICES}" -eq 1 ]]; then
        log_info "Enabling systemd user services..."
        if command -v systemctl >/dev/null 2>&1; then
            run_cmd systemctl --user daemon-reload || true
            run_cmd systemctl --user enable kanata.service || true
            run_cmd systemctl --user enable yazi-filemanager1.service || true
            log_success "User services enabled: kanata.service, yazi-filemanager1.service"
        fi
    fi
}

# ------------------------------------------------------------------------------
# Main Execution & Argument Parsing
# ------------------------------------------------------------------------------
show_help() {
    cat <<EOF
Arch Linux / CachyOS Dotfiles Bootstrap Script

Usage: $(basename "$0") [OPTIONS]

Options:
  --all            Run complete bootstrap (default)
  --cli            Install only CLI packages and tools (skips heavy GUI apps)
  --gui            Install only GUI applications
  --rust           Setup only Rust toolchain and cargo packages
  --node           Setup only Node.js and npm
  --python         Setup only Python and uv
  --yazi           Install only Yazi plugins and companion tools
  --assets         Deploy only custom scripts and desktop entries
  --dry-run        Print commands that would be executed without running them
  -h, --help       Show this help message

Examples:
  ./bootstrap.sh --all
  ./bootstrap.sh --cli --assets
  ./bootstrap.sh --dry-run
EOF
}

parse_args() {
    local explicit=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --all)
                INSTALL_CLI=1
                INSTALL_GUI=1
                INSTALL_RUST=1
                INSTALL_NODE=1
                INSTALL_PYTHON=1
                INSTALL_YAZI=1
                INSTALL_ASSETS=1
                SETUP_SERVICES=1
                shift
                ;;
            --cli)
                if [[ "${explicit}" -eq 0 ]]; then
                    INSTALL_CLI=1; INSTALL_GUI=0; INSTALL_RUST=0; INSTALL_NODE=0; INSTALL_PYTHON=0; INSTALL_YAZI=0; INSTALL_ASSETS=0; SETUP_SERVICES=0; explicit=1
                else
                    INSTALL_CLI=1
                fi
                shift
                ;;
            --gui)
                if [[ "${explicit}" -eq 0 ]]; then
                    INSTALL_CLI=0; INSTALL_GUI=1; INSTALL_RUST=0; INSTALL_NODE=0; INSTALL_PYTHON=0; INSTALL_YAZI=0; INSTALL_ASSETS=0; SETUP_SERVICES=0; explicit=1
                else
                    INSTALL_GUI=1
                fi
                shift
                ;;
            --rust)
                if [[ "${explicit}" -eq 0 ]]; then
                    INSTALL_CLI=0; INSTALL_GUI=0; INSTALL_RUST=1; INSTALL_NODE=0; INSTALL_PYTHON=0; INSTALL_YAZI=0; INSTALL_ASSETS=0; SETUP_SERVICES=0; explicit=1
                else
                    INSTALL_RUST=1
                fi
                shift
                ;;
            --node)
                if [[ "${explicit}" -eq 0 ]]; then
                    INSTALL_CLI=0; INSTALL_GUI=0; INSTALL_RUST=0; INSTALL_NODE=1; INSTALL_PYTHON=0; INSTALL_YAZI=0; INSTALL_ASSETS=0; SETUP_SERVICES=0; explicit=1
                else
                    INSTALL_NODE=1
                fi
                shift
                ;;
            --python)
                if [[ "${explicit}" -eq 0 ]]; then
                    INSTALL_CLI=0; INSTALL_GUI=0; INSTALL_RUST=0; INSTALL_NODE=0; INSTALL_PYTHON=1; INSTALL_YAZI=0; INSTALL_ASSETS=0; SETUP_SERVICES=0; explicit=1
                else
                    INSTALL_PYTHON=1
                fi
                shift
                ;;
            --yazi)
                if [[ "${explicit}" -eq 0 ]]; then
                    INSTALL_CLI=0; INSTALL_GUI=0; INSTALL_RUST=0; INSTALL_NODE=0; INSTALL_PYTHON=0; INSTALL_YAZI=1; INSTALL_ASSETS=0; SETUP_SERVICES=0; explicit=1
                else
                    INSTALL_YAZI=1
                fi
                shift
                ;;
            --assets)
                if [[ "${explicit}" -eq 0 ]]; then
                    INSTALL_CLI=0; INSTALL_GUI=0; INSTALL_RUST=0; INSTALL_NODE=0; INSTALL_PYTHON=0; INSTALL_ASSETS=1; SETUP_SERVICES=1; explicit=1
                else
                    INSTALL_ASSETS=1; SETUP_SERVICES=1
                fi
                shift
                ;;
            --dry-run)
                DRY_RUN=1
                shift
                ;;
            -h|--help)
                show_help
                exit 0
                ;;
            *)
                log_error "Unknown option: $1"
                show_help
                exit 1
                ;;
        esac
    done
}

main() {
    parse_args "$@"

    echo -e "${C_CYAN}${C_BOLD}"
    cat << "EOF"
  ____              _       _                   
 | __ )  ___   ___ | |_ ___| |_ _ __ __ _ _ __  
 |  _ \ / _ \ / _ \| __/ __| __| '__/ _` | '_ \ 
 | |_) | (_) | (_) | |_\__ \ |_| | | (_| | |_) |
 |____/ \___/ \___/ \__|___/\__|_|  \__,_| .__/ 
                                         |_|    
EOF
    echo -e "${C_RESET}"
    log_info "Initializing system bootstrap..."

    check_system
    detect_or_install_aur_helper

    if [[ "${INSTALL_CLI}" -eq 1 ]]; then
        setup_cli_packages
    fi

    if [[ "${INSTALL_GUI}" -eq 1 ]]; then
        setup_gui_packages
    fi

    if [[ "${INSTALL_RUST}" -eq 1 ]]; then
        setup_rust
    fi

    if [[ "${INSTALL_NODE}" -eq 1 ]]; then
        setup_node
    fi

    if [[ "${INSTALL_PYTHON}" -eq 1 ]]; then
        setup_python
    fi

    if [[ "${INSTALL_YAZI}" -eq 1 ]]; then
        setup_yazi_plugins
    fi

    if [[ "${INSTALL_ASSETS}" -eq 1 ]]; then
        setup_assets_and_services
    fi

    log_step "Bootstrap Complete!"
    log_success "System is ready. If you changed your shell or added group permissions, log out and log back in."
}

main "$@"
