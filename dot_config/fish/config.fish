source /usr/share/cachyos-fish-config/cachyos-config.fish

# overwrite greeting
# potentially disabling fastfetch
function fish_greeting
#    # smth smth
end
#

if status is-interactive
    fastfetch

    atuin init fish | source
end

function y
    set tmp (mktemp -t "yazi-cwd.XXXXXX")
    command yazi $argv --cwd-file="$tmp"
    if read -z cwd <"$tmp"; and [ "$cwd" != "$PWD" ]; and test -d "$cwd"
        builtin cd -- "$cwd"
    end
    command rm -f -- "$tmp"
end

set -gx EDITOR nvim
set -gx VISUAL nvim
set prompt_symbol_color normal

set -q XDG_BIN_HOME; or set -gx XDG_BIN_HOME "$HOME/.local/bin"
set -gx PATH $XDG_BIN_HOME $PATH

zoxide init fish | source

