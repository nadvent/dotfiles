-- ==============================================================================
-- Yazi Initialization Script (init.lua)
-- ==============================================================================

-- ------------------------------------------------------------------------------
-- 1. Bookmarks Plugin Setup (dedukun/bookmarks)
-- ------------------------------------------------------------------------------
require("bookmarks"):setup({
	last_directory = { dir = "jump" }, -- Options: "jump", "mark", "dir"
	persist = "all",                   -- Options: "none", "all", "vim"
	notify = {
		enable = true,
		timeout = 2,
	},
})

-- ------------------------------------------------------------------------------
-- 2. Restore Plugin Setup (boydaihungst/restore)
-- ------------------------------------------------------------------------------
require("restore"):setup({
	show_confirm = true,
})
