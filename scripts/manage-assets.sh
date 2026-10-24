#!/usr/bin/env bash
# ==============================================================================
# manage-assets.sh - Manage Custom Scripts, Desktop Files, and Services
#
# Inspects, imports into Chezmoi, or deploys custom scripts and .desktop files
# referenced by Umbriel, Noctalia, Fish, Ghostty, and systemd configs.
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHEZMOI_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
TARGET_HOME="${HOME}"

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
# Inventory of referenced assets
# Format: "TYPE|SOURCE_REL_PATH|TARGET_PATH|IS_EXECUTABLE|DESCRIPTION"
# ------------------------------------------------------------------------------
ASSETS=(
    # Custom binaries & scripts in ~/.local/bin
    "bin|dot_local/bin/executable_escape-scratchpad|${TARGET_HOME}/.local/bin/escape-scratchpad|1|Closes active scratchpads in Umbriel"
    "bin|dot_local/bin/executable_ghostty-scratchpad|${TARGET_HOME}/.local/bin/ghostty-scratchpad|1|Dedicated Ghostty scratchpad launcher"
    "bin|dot_local/bin/executable_lg_vm.sh|${TARGET_HOME}/.local/bin/lg_vm.sh|1|Looking Glass client monitor loop for VM"
    "bin|dot_local/bin/executable_qutebrowser-scratchpad|${TARGET_HOME}/.local/bin/qutebrowser-scratchpad|1|Dedicated Qutebrowser quick-search scratchpad"
    "bin|dot_local/bin/executable_toggle-audio.sh|${TARGET_HOME}/.local/bin/toggle-audio.sh|1|Switches audio between Speakers and Behringer Interface"
    "bin|dot_local/bin/executable_toggle-scratchpad-browser.sh|${TARGET_HOME}/.local/bin/toggle-scratchpad-browser.sh|1|Quick Search scratchpad toggle helper"
    "bin|dot_local/bin/executable_toggle-scratchpad-ghostty.sh|${TARGET_HOME}/.local/bin/toggle-scratchpad-ghostty.sh|1|Terminal scratchpad toggle helper"
    "bin|dot_local/bin/executable_yazi-filemanager1|${TARGET_HOME}/.local/bin/yazi-filemanager1|1|D-Bus FileManager1 daemon for Yazi in Ghostty"
    "bin|dot_local/bin/env.fish|${TARGET_HOME}/.local/bin/env.fish|0|Fish PATH initialization helper"

    # Desktop Applications in ~/.local/share/applications
    "desktop|dot_local/share/applications/astra.desktop|${TARGET_HOME}/.local/share/applications/astra.desktop|0|Astra audiophile music player desktop entry"
    "desktop|dot_local/share/applications/ghostty-scratchpad.desktop|${TARGET_HOME}/.local/share/applications/ghostty-scratchpad.desktop|0|Ghostty scratchpad desktop entry"
    "desktop|dot_local/share/applications/yazi.desktop|${TARGET_HOME}/.local/share/applications/yazi.desktop|0|Yazi file manager desktop entry"

    # Systemd User Services in ~/.config/systemd/user
    "systemd|dot_config/systemd/user/kanata.service|${TARGET_HOME}/.config/systemd/user/kanata.service|0|Kanata keyboard remapper user daemon"
    "systemd|dot_config/systemd/user/yazi-filemanager1.service|${TARGET_HOME}/.config/systemd/user/yazi-filemanager1.service|0|Yazi D-Bus FileManager1 service"

    # Additional application configs referenced in scripts
    "config|dot_config/qutebrowser/config.py|${TARGET_HOME}/.config/qutebrowser/config.py|0|Main Qutebrowser configuration"
    "config|dot_config/qutebrowser/scratchpad.py|${TARGET_HOME}/.config/qutebrowser/scratchpad.py|0|Qutebrowser quick-search scratchpad config"
)

# ------------------------------------------------------------------------------
# Action: status
# ------------------------------------------------------------------------------
check_status() {
    log_step "Checking Asset Status"
    local total=0
    local target_found=0
    local chezmoi_found=0

    printf "%-32s %-12s %-12s %s\n" "Asset / Target" "In System?" "In Chezmoi?" "Notes"
    printf "%s\n" "--------------------------------------------------------------------------------"

    for entry in "${ASSETS[@]}"; do
        IFS="|" read -r type chezmoi_rel target_path is_exec desc <<< "${entry}"
        total=$((total + 1))
        local target_name
        target_name="$(basename "${target_path}")"
        local chezmoi_abs="${CHEZMOI_DIR}/${chezmoi_rel}"

        local sys_stat="MISSING"
        if [[ -e "${target_path}" ]]; then
            if [[ "${is_exec}" -eq 1 && ! -x "${target_path}" ]]; then
                sys_stat="NOT +X"
            else
                sys_stat="PRESENT"
            fi
            target_found=$((target_found + 1))
        fi

        local cm_stat="MISSING"
        if [[ -e "${chezmoi_abs}" ]]; then
            cm_stat="TRACKED"
            chezmoi_found=$((chezmoi_found + 1))
        fi

        printf "%-32s %-12s %-12s %s\n" "${target_name}" "${sys_stat}" "${cm_stat}" "${desc}"
    done

    echo ""
    log_info "Summary: ${target_found}/${total} found on system, ${chezmoi_found}/${total} tracked in chezmoi source repository."
}

# ------------------------------------------------------------------------------
# Action: import
# ------------------------------------------------------------------------------
import_to_chezmoi() {
    log_step "Importing Assets into Chezmoi Repository"

    for entry in "${ASSETS[@]}"; do
        IFS="|" read -r type chezmoi_rel target_path is_exec desc <<< "${entry}"
        local chezmoi_abs="${CHEZMOI_DIR}/${chezmoi_rel}"
        local dest_dir
        dest_dir="$(dirname "${chezmoi_abs}")"

        if [[ -e "${target_path}" ]]; then
            mkdir -p "${dest_dir}"
            cp -p "${target_path}" "${chezmoi_abs}"
            if [[ "${is_exec}" -eq 1 ]]; then
                chmod +x "${chezmoi_abs}"
            fi
            log_success "Imported [${type}] $(basename "${target_path}") -> ${chezmoi_rel}"
        else
            log_warn "Target file does not exist, skipping: ${target_path}"
        fi
    done

    log_success "Import complete! Chezmoi now tracks all custom scripts and desktop entries."
}

# ------------------------------------------------------------------------------
# Action: deploy / install
# ------------------------------------------------------------------------------
deploy_assets() {
    log_step "Deploying Assets to Target System Directories"

    for entry in "${ASSETS[@]}"; do
        IFS="|" read -r type chezmoi_rel target_path is_exec desc <<< "${entry}"
        local chezmoi_abs="${CHEZMOI_DIR}/${chezmoi_rel}"
        local target_dir
        target_dir="$(dirname "${target_path}")"

        if [[ -e "${chezmoi_abs}" ]]; then
            mkdir -p "${target_dir}"
            cp -p "${chezmoi_abs}" "${target_path}"
            if [[ "${is_exec}" -eq 1 ]]; then
                chmod +x "${target_path}"
            fi
            log_success "Deployed: ${chezmoi_rel} -> ${target_path}"
        elif [[ -e "${target_path}" ]]; then
            if [[ "${is_exec}" -eq 1 && ! -x "${target_path}" ]]; then
                chmod +x "${target_path}"
                log_success "Set executable flag on: ${target_path}"
            fi
        else
            log_warn "Asset missing in both repo and destination: ${target_path}"
        fi
    done

    # Refresh desktop database
    if command -v update-desktop-database >/dev/null 2>&1; then
        log_info "Updating user desktop database..."
        update-desktop-database "${TARGET_HOME}/.local/share/applications" 2>/dev/null || true
    fi

    # Reload systemd user daemon
    if command -v systemctl >/dev/null 2>&1 && [[ -n "${DBUS_SESSION_BUS_ADDRESS:-}" || -n "${XDG_RUNTIME_DIR:-}" ]]; then
        log_info "Reloading systemd user daemon..."
        systemctl --user daemon-reload 2>/dev/null || true
    fi

    log_success "Asset deployment completed."
}

# ------------------------------------------------------------------------------
# Main dispatcher
# ------------------------------------------------------------------------------
usage() {
    cat <<EOF
Usage: $(basename "$0") <command>

Commands:
  status   Check current status of referenced scripts, apps, and services
  import   Copy existing target scripts and desktop files into Chezmoi repository
  deploy   Deploy/sync tracked assets from Chezmoi into target locations (~/.local/bin, etc.)
  help     Show this help message
EOF
}

case "${1:-status}" in
    status|check)
        check_status
        ;;
    import)
        import_to_chezmoi
        ;;
    deploy|install)
        deploy_assets
        ;;
    help|--help|-h)
        usage
        ;;
    *)
        log_error "Unknown command: $1"
        usage
        exit 1
        ;;
esac
