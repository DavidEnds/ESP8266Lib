-- usage: dofile ("compile.lua")

local always = true;

local function compile (f)
	if always or nil == file.open (f .. ".lc") then
		print ("=== compiling " .. f .. ".lua");
		node.compile (f .. ".lua");
		collectgarbage ();
	else
		file.close ();
	end
end

compile ("main");
compile ("readTemp");
compile ("doWiFi");
compile ("doMQTT");
compile ("ds18b20");

resetRunCount = true
