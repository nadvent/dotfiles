#!/usr/bin/env bash
# ==============================================================================
# install-yazi-plugins.sh - Install and sync Yazi plugins & companion tools
#
# Installs external helper tools (ouch, glow) and runs `ya pkg install`
# based on the dependencies pinned in ~/.config/yazi/package.toml.
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHEZMOI_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
YAZI_CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/yazi"
PACKAGE_TOML="${YAZI_CONFIG_DIR}/package.toml"

# Colors
C_RESET='\033[0m'
C_BOLD='\033[1m'
C_BLUE='\033[34m'
C_GREEN='\033[32m'
C_YELLOW='\033[33m'
C_RED='\033[31m'
C_CYAN='\033[36m'

log_info()    { echo -e "${C_BLUE}${C_BOLD}[INFO]${C_RESET} $*"; }
log_success() { echo -e "${C_GREEN}${C_BOLD}[OK]${C_RESET} $*"; }
log_warn()    { echo -e "${C_YELLOW}${C_BOLD}[WARN]${C_RESET} $*"; }
log_error()   { echo -e "${C_RED}${C_BOLD}[ERR]${C_RESET} $*"; }
log_step()    { echo -e "\n${C_CYAN}${C_BOLD}==>${C_RESET} ${C_BOLD}$*${C_RESET}"; }

# ------------------------------------------------------------------------------
# 1. Verify Prerequisites & Companion Utilities
# ------------------------------------------------------------------------------
check_prerequisites() {
    log_step "Checking Yazi & Companion Tools"

    if ! command -v yazi >/dev/null 2>&1 || ! command -v ya >/dev/null 2>&1; then
        log_error "Yazi CLI ('yazi' and 'ya') not found. Please install yazi first."
        exit 1
    fi
    log_success "Found $(yazi --version 2>/dev/null || echo 'yazi') and $(ya --version 2>/dev/null || echo 'ya')."

    # Companion previewers and compressors referenced in yazi.toml & plugins
    local missing_tools=()
    for tool in ouch glow; do
        if ! command -v "${tool}" >/dev/null 2>&1; then
            missing_tools+=("${tool}")
        else
            log_success "Found companion utility: ${tool}"
        fi
    done

    if [[ ${#missing_tools[@]} -gt 0 ]]; then
        log_warn "Missing companion tools used by Yazi plugins: ${missing_tools[*]}"
        if command -v pacman >/dev/null 2>&1; then
            log_info "Attempting to install missing tools via pacman..."
            sudo pacman -S --needed --noconfirm "${missing_tools[@]}" || log_warn "Please install ${missing_tools[*]} manually."
        fi
    fi
}

# ------------------------------------------------------------------------------
# 2. Install / Sync Plugins via `ya pkg`
# ------------------------------------------------------------------------------
install_plugins() {
    local force_flag="${1:-}"

    log_step "Installing Plugins from package.toml"

    if [[ ! -f "${PACKAGE_TOML}" ]]; then
        # If not in target ~/.config/yazi yet, check chezmoi source repo
        local repo_pkg="${CHEZMOI_DIR}/dot_config/yazi/package.toml"
        if [[ -f "${repo_pkg}" ]]; then
            log_info "Deploying package.toml from chezmoi source to ${YAZI_CONFIG_DIR}..."
            mkdir -p "${YAZI_CONFIG_DIR}"
            cp -p "${repo_pkg}" "${PACKAGE_TOML}"
        else
            log_error "package.toml not found at ${PACKAGE_TOML} or ${repo_pkg}."
            exit 1
        fi
    fi

    log_info "Found package manifest: ${PACKAGE_TOML}"

    local ya_args=("pkg" "install")
    if [[ "${force_flag}" == "--discard" || "${force_flag}" == "--force" ]]; then
        ya_args+=("--discard")
        log_info "Running: ya pkg install --discard (overwriting any local modifications)"
    else
        log_info "Running: ya pkg install"
    fi

    if ya "${ya_args[@]}"; then
        log_success "All packages from package.toml installed successfully!"
    else
        log_warn "ya pkg install failed or detected local modifications."
        echo ""
        log_info "If you have uncommitted modifications in plugin directories, run:"
        log_info "  $0 --discard"
        return 1
    fi
}

# ------------------------------------------------------------------------------
# 3. List Installed Plugins
# ------------------------------------------------------------------------------
list_plugins() {
    log_step "Installed Yazi Plugins & Flavors"

    echo -e "${C_BOLD}Active Plugins in ${YAZI_CONFIG_DIR}/plugins:${C_RESET}"
    if [[ -d "${YAZI_CONFIG_DIR}/plugins" ]]; then
        for plugin in "${YAZI_CONFIG_DIR}/plugins"/*; do
            if [[ -d "${plugin}" ]]; then
                local bname
                bname="$(basename "${plugin}")"
                if [[ -f "${plugin}/main.lua" ]]; then
                    echo -e "  • ${C_GREEN}${bname}${C_RESET} (custom/local)"
                elif [[ -f "${plugin}/readonly_main.lua" ]]; then
                    echo -e "  • ${C_CYAN}${bname}${C_RESET} (package.toml managed)"
                else
                    echo -e "  • ${bname}"
                fi
            fi
        done
    else
        echo "  (No plugins directory found)"
    fi

    echo ""
    echo -e "${C_BOLD}Active Flavors in ${YAZI_CONFIG_DIR}/flavors:${C_RESET}"
    if [[ -d "${YAZI_CONFIG_DIR}/flavors" ]]; then
        for flavor in "${YAZI_CONFIG_DIR}/flavors"/*; do
            if [[ -d "${flavor}" ]]; then
                echo -e "  • $(basename "${flavor}")"
            fi
        done
    else
        echo "  (No flavors directory found)"
    fi
}

# ------------------------------------------------------------------------------
# Main
# ------------------------------------------------------------------------------
main() {
    local action="${1:-install}"
    case "${action}" in
        install|--install)
            check_prerequisites
            install_plugins "${2:-}"
            list_plugins
            ;;
        --discard|--force)
            check_prerequisites
            install_plugins "--discard"
            list_plugins
            ;;
        list|--list)
            list_plugins
            ;;
        -h|--help|help)
            cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Options:
  install           Install plugins from package.toml (default)
  --discard         Install plugins and discard local modifications
  list              List currently installed Yazi plugins and flavors
  -h, --help        Show this help message
EOF
            ;;
        *)
            check_prerequisites
            install_plugins "${action}"
            list_plugins
            ;;
    esac
}

main "$@"
