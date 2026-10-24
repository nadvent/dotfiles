fish_add_path ~/.local/bin
fish_add_path /opt/homebrew/bin
test -n "$CARGO_HOME"; and fish_add_path $CARGO_HOME/bin
fish_add_path ~/.cargo/bin
test -n "$GOPATH"; and fish_add_path $GOPATH/bin
fish_add_path ~/.go/bin
