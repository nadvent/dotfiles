config.load_autoconfig(False)

# Disable session auto-saving and tab restoration
c.auto_save.session = False
c.session.lazy_restore = False

# Enforce single tab behavior without a tab bar
c.tabs.show = "never"
c.tabs.last_close = "close"
c.tabs.tabs_are_windows = True
c.tabs.new_position.unrelated = "last"  # Valid options: 'prev', 'next', 'first', 'last'

# Updated ad/host blocking option name
c.content.blocking.enabled = True

# Default landing/search URL
c.url.start_pages = ["https://google.com"]
c.url.default_page = "https://google.com"


c.url.searchengines = {
    "DEFAULT": "https://google.com/search?hl=en&q={}",
    "aw": "https://wiki.archlinux.org/?search={}",
    "yt": "https://youtube.com{}",
}
