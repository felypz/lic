lic.config = {
	host = "irc.libera.chat",
	port = 6697,
	tls = true,
	nick = "",
	user = "lic",
	realname = "Lunar IRC Client",
	history_size = 1024,
	channels = {},
}

lic.theme = {
	bar_bg    = 0,
	bar_fg    = 7,
	active_bg = 4,
	active_fg = 0,
	unread    = 2,
	mention   = 3,
	timestamp = 7,
	event     = 7,
	error     = 1,
	action    = 5,
	prompt    = 6,
}

lic.command("shrug", function(arg)
	local target = lic.active_buffer()
	local prefix = (arg ~= "" and (arg .. " ") or "")
	lic.privmsg(target, prefix .. "¯\\_(ツ)_/¯")
end)

lic.on("PRIVMSG", function(msg)
	local text = msg.params[2] or ""
	if text:find(lic.nick(), 1, true) then
		io.write("\a")
	end
end)
