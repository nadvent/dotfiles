--- @sync entry
return {
	entry = function()
		local cwd = tostring(cx.active.current.cwd.path or cx.active.current.cwd)
		if cwd:match("^trash:") or cwd:match("/%.local/share/Trash") then
			ya.emit("remove", { permanently = true })
		else
			ya.emit("remove", {})
		end
	end,
}
