--- @sync entry
return {
	entry = function()
		local cwd = cx.active.current.cwd
		local url_str = tostring(cwd)
		local path_str = tostring(cwd.path or cwd)

		local is_trash = (cwd.spec and cwd.spec.scheme == "trash")
			or url_str:match("^trash:")
			or path_str:match("/%.local/share/Trash")
			or path_str:match("/%.Trash%-%d+")

		if is_trash then
			ya.emit("remove", { permanently = true })
		else
			ya.emit("remove", {})
		end
	end,
}
