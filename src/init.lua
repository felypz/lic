lic.config = {
	host = "irc.libera.chat",
	port = 6697,
	tls = true,
	nick = "lic_test",
	user = "lic",
	realname = "Lunar IRC Client",
	history_size = 1024,
	channels = { "#lic-test" },
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
