function Log (...) mLog ("wifi", unpack(arg)) end
time_wifi = done_file (tmr.now())
used ()

local check_rate = 100			-- checks per second
local soft_limit = wifi_soft_limit*check_rate
local hard_limit = wifi_hard_limit*check_rate

local check_count = 0
local wifi_status = -1

local function resetWiFi ()
	Log ("resetWIFI")
	wifi.sta.disconnect()
	if clientIP then
		wifi.sta.autoconnect(0)
		wifi.sta.setip({ip=clientIP,netmask=netMask,gateway=netGW})
	end
	wifi.setmode(wifi.STATION)
	wifi.sta.config(ssid, passphrase)
	wifi.sta.connect()
end

local function haveConnection()
	time_wifi = tmr.now() - time_wifi
	Log ("WiFi   available after %.2f seconds, ip=%s",
		check_count/check_rate, wifi.sta.getip())
	if nil == runCount then
		if newRun then
			do_file ("first")
			return
		end
		runCount = incrementCounter(rtc_runCount_address)
	end
	time_First = 0
	do_file ("save")
end

local function waitforConnection()
	check_count = check_count + 1
	local new_status = wifi.sta.status()
	if new_status ~= wifi_status then
		Log ("status %d", new_status)
		wifi_status = new_status
	end
	if 5 ~= new_status then
		if 0 == check_count % check_rate then	-- announce once a second
			Log ("WiFi unavailable after %d seconds, status=%d",
				check_count/check_rate, wifi_status)
			if use_old_WiFi_setup and check_count >= soft_limit then
				resetWiFi ()
				use_old_WiFi_setup = false
				check_count = 0		-- restart counter
				incrementCounter(rtc_failSoft_address)
			elseif check_count >= hard_limit then
				tmr.stop(1)
				incrementCounter(rtc_failHard_address)
				Log ("giving up")
				doSleep()
			end
		end
	else
		tmr.stop(1)
		haveConnection()
	end
end

local function doWiFi()
	local status = wifi.sta.status()
	if 5 ~= status then
		if use_old_WiFi_setup then
			Log ("using old WiFi setup")
		elseif nil == ssid or nil == passphrase then
			print ("missing ssid/pass")
			return
		else
			Log ("Setting up WIFI...")
			resetWiFi ()
		end
		check_count = 0
		tmr.alarm(1, 1000/check_rate, 1, waitforConnection)
	else
		Log ("Keeping active connection")
		haveConnection()
	end
end

doWiFi()
