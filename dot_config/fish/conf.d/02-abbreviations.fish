if status is-interactive

    # =========================================================================
    # 1. Neovim Ergonomics
    # =========================================================================
    abbr -a v 'nvim'
    abbr -a vi 'nvim'
    abbr -a vim 'nvim'
    abbr -a v. 'nvim .'                                 # Open Neovim in current directory
    abbr -a vconf 'nvim ~/.config/nvim'                 # Jump straight to Nvim configuration
    abbr -a vfish 'nvim ~/.config/fish/conf.d'          # Edit Fish modular configs
    abbr -a vdiff 'nvim -d'                             # Side-by-side file diff in Neovim

    # =========================================================================
    # 2. CachyOS & Arch Package Management (Pacman & Paru/Yay)
    # =========================================================================
    # Native Pacman operations
    abbr -a pac 'sudo pacman -S'                        # Install
    abbr -a pacs 'pacman -Ss'                           # Search
    abbr -a pacr 'sudo pacman -Rns'                     # Remove + dependencies
    abbr -a pacq 'pacman -Qe'                           # List 
    abbr -a pacinfo 'pacman -Si'                        # View detailed package metadata

    # System updates & CachyOS maintenance
    abbr -a up 'sudo pacman -Syu'                       # Update
    abbr -a upa 'paru -Syu'                             # Update + AUR
    abbr -a cachy-mirrors 'sudo cachyos-rate-mirrors'   # Optimize CachyOS package download speeds
    abbr -a pac-clean 'sudo pacman -Sc'                 # Clear package cache
    abbr -a pac-orphans 'sudo pacman -Rns (pacman -Qtdq)' # Safely wipe all orphaned packages

    # =========================================================================
    # 3. Umbriel & App Management
    # =========================================================================
    abbr -a um 'umbriel'
    abbr -a ums 'umbriel status'
    abbr -a umc 'umbriel config validate'
    abbr -a umr 'umbriel msg '

    # =========================================================================
    # 4. Systemd & CachyOS Diagnostics
    # =========================================================================
    abbr -a sc 'sudo systemctl'
    abbr -a scu 'systemctl --user'
    abbr -a sc-status 'systemctl status'
    abbr -a sc-restart 'sudo systemctl restart'
    abbr -a jc 'journalctl -xeu'                        # Follow unit logs with explanations
    abbr -a jc-boot 'journalctl -b -p 3'                # View errors/critical logs from current boot

    abbr -a df 'pydf'

    abbr -a du 'dust'

    # =========================================================================
    # 5. Git Essentials
    # =========================================================================
    abbr -a g 'git'
    abbr -a gst 'git status -sb'                        # Short, branch-aware status
    abbr -a ga 'git add'
    abbr -a gaa 'git add --all'
    abbr -a gc 'git commit -m'
    abbr -a gca 'git commit --amend'
    abbr -a gco 'git checkout'
    abbr -a gcb 'git checkout -b'
    abbr -a gp 'git push'
    abbr -a gpf 'git push --force-with-lease'           # Safe force-push
    abbr -a gl 'git pull'
    abbr -a gd 'git diff'
    abbr -a glog 'git log --oneline --graph --decorate'

    abbr -a lg 'lazygit'
    abbr -a ldo 'lazydocker'

    abbr -a prc 'gh pr create --web'
    abbr -a prv 'gh pr view --web'
    abbr -a prco 'gh pr checkout'

    # =========================================================================
    # 6. Navigation & Safety Defaults
    # =========================================================================
    abbr -a .. 'cd ..'
    abbr -a ... 'cd ../..'
    abbr -a .... 'cd ../../..'
    abbr -a mkdir 'mkdir -p'                            # Create parents automatically
    abbr -a conf 'cd ~/.config/'
    abbr -a rm 'rm -I'                                  # Prompt when deleting >3 files or recursively
    abbr -a cp 'cp -i'                                  # Confirm before overwriting
    abbr -a mv 'mv -i'                                  # Confirm before overwriting

    # Modern CLI drop-ins (fallbacks to standard if not installed)
    if type -q eza
        abbr -a ls 'eza --icons'
        abbr -a ll 'eza -la --icons --git'
        abbr -a tree 'eza --tree --icons'
    else
        abbr -a ll 'ls -lah'
    end

    if type -q bat
        abbr -a cat 'bat --paging=never'
    end

    # zoxide (replaces cd)
    abbr -a cd 'z'
    abbr -a cdi 'zi' # Interactive search via fzf

    # ripgrep & fd
    abbr -a rgh 'rg --hidden --glob "!.git/*"'
    abbr -a fdh 'fd --hidden --exclude .git'


    # =========================================================================
    # 7. Fish 3.6+ "Anywhere" Positional Expansions
    # =========================================================================
    # Expands anywhere in a command, not just at the start
    abbr -a -p anywhere L '| less'
    abbr -a -p anywhere C '| wl-copy'                   # Fast copy to Wayland clipboard
    abbr -a -p anywhere B '| bat'
    abbr -a -p anywhere G '| rg'
    abbr -a -p anywhere J '| jq'
    abbr -a -p anywhere NE '2>/dev/null'

    abbr -a -p anywhere .conf '/home/nazhanys/.config/'


    # Quick Navigations

    abbr -a fishconf '$EDITOR ~/.config/fish/config.fish'
    abbr -a reload 'source ~/.config/fish/config.fish'
    abbr -a viconf 'cd ~/.config/nvim/'
end
