function fish_title --description 'Format Ghostty tab title: folder: process' --argument-names last_command
    # 1. Extract folder directory name (~ for home directory)
    set -l folder (basename "$PWD")
    if test "$PWD" = "$HOME"
        set folder "~"
    end

    # 2. Extract process name
    if test -n "$last_command"
        set -l raw_cmd (string match -r '^\S+' -- $last_command)
        set -l proc (basename "$raw_cmd")
        echo "$folder: $proc"
    else
        # Idle at prompt
        echo "$folder: fish"
    end
end
