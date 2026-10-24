# XDG Base Directory Specification
set -q XDG_CONFIG_HOME; or set -gx XDG_CONFIG_HOME $HOME/.config
set -q XDG_DATA_HOME; or set -gx XDG_DATA_HOME $HOME/.local/share
set -q XDG_STATE_HOME; or set -gx XDG_STATE_HOME $HOME/.local/state
set -q XDG_CACHE_HOME; or set -gx XDG_CACHE_HOME $HOME/.cache

# Default applications
set -gx EDITOR nvim
set -gx VISUAL nvim
set -gx LANG en_US.UTF-8

# Application-specific XDG compliance
set -q GTK2_RC_FILES; or set -gx GTK2_RC_FILES $XDG_CONFIG_HOME/gtk-2.0/gtkrc
set -q CARGO_HOME; or set -gx CARGO_HOME $XDG_DATA_HOME/cargo
set -q RUSTUP_HOME; or set -gx RUSTUP_HOME $XDG_DATA_HOME/rustup
set -q GOPATH; or set -gx GOPATH $XDG_DATA_HOME/go
set -q NPM_CONFIG_USERCONFIG; or set -gx NPM_CONFIG_USERCONFIG $XDG_CONFIG_HOME/npm/npmrc
set -q PYTHON_HISTORY; or set -gx PYTHON_HISTORY $XDG_STATE_HOME/python/history
set -q NODE_REPL_HISTORY; or set -gx NODE_REPL_HISTORY $XDG_DATA_HOME/node_repl_history
