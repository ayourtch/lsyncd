--============================================================================
-- lsyncd.lua   Live (Mirror) Syncing Demon
--
-- License: GPLv2 (see COPYING) or any later version
--
-- Authors: Axel Kittenberger <axkibe@gmail.com>
--
-- This is the "runner" part of lsyncd. It containts all its high-level logic.
-- It works closely together with the lsyncd core in lsyncd.c. This means it
-- cannot be runned directly from the standard lua interpreter.
--============================================================================

----
-- A security measurement.
-- Core will exit if version ids mismatch.
--
if lsyncd_version then
	-- checks if the runner is being loaded twice 
	lsyncd.log("Error",
		"You cannot use the lsyncd runner as configuration file!")
	lsyncd.terminate(-1) -- ERRNO
end
lsyncd_version = "2.0beta1"

-----
-- Hides the core interface from user scripts
--
_l = lsyncd
lsyncd = nil
local lsyncd = _l
_l = nil

----
-- Shortcuts (which user is supposed to be able to use them as well)
--
log  = lsyncd.log
terminate = lsyncd.terminate

--============================================================================
-- Coding checks, ensure termination on some easy to do coding errors.
--============================================================================

-----
-- The array objects are tables that error if accessed with a non-number.
--
local Array = (function()
	-- Metatable
	local mt = {}

	-- on accessing a nil index.
	mt.__index = function(t, k) 
		if type(k) ~= "number" then
			error("Key '"..k.."' invalid for Array", 2)
		end
		return rawget(t, k)
	end

	-- on assigning a new index.
	mt.__newindex = function(t, k, v)
		if type(k) ~= "number" then
			error("Key '"..k.."' invalid for Array", 2)
		end
		rawset(t, k, v)
	end

	-- creates a new object
	local function new()
		local o = {}
		setmetatable(o, mt)
		return o
	end

	-- objects public interface
	return {new = new}
end)()


-----
-- The count array objects are tables that error if accessed with a non-number.
-- Additionally they maintain their length as "size" attribute.
-- Lua's # operator does not work on tables which key values are not 
-- strictly linear.
--
local CountArray = (function()
	-- Metatable
	local mt = {}

	-----
	-- key to native table
	local k_nt = {}
	
	-----
	-- on accessing a nil index.
	mt.__index = function(t, k) 
		if type(k) ~= "number" then
			error("Key '"..k.."' invalid for CountArray", 2)
		end
		return t[k_nt][k]
	end

	-----
	-- on assigning a new index.
	mt.__newindex = function(t, k, v)
		if type(k) ~= "number" then
			error("Key '"..k.."' invalid for CountArray", 2)
		end
		-- value before
		local vb = t[k_nt][k]
		if v and not vb then
			t._size = t._size + 1
		elseif not v and vb then
			t._size = t._size - 1
		end
		t[k_nt][k] = v
	end

	-- TODO
	local function iwalk(self)
		return ipairs(self[k_nt])
	end

	------
	-- returns the count
	local function size(self)
		return self._size
	end

	----
	-- creates a new count array
	local function new()
		-- k_nt is native table, private for this object.
		local o = {_size = 0, iwalk = iwalk, size = size, [k_nt] = {} }
		setmetatable(o, mt)
		return o
	end

	-- objects public interface
	return {new = new}
end)()


-----
-- Metatable to limit keys to those only presented in their prototype
--
local meta_check_prototype = {
	__index = function(t, k) 
		if not t.prototype[k] then
			error("tables prototype doesn't have key '"..k.."'.", 2)
		end
		return rawget(t, k)
	end,
	__newindex = function(t, k, v)
		if not t.prototype[k] then
			error("tables prototype doesn't have key '"..k.."'.", 2)
		end
		rawset(t, k, v)
	end
}

-----
-- Sets the prototype of a table limiting its keys to a defined list.
--
local function set_prototype(t, prototype) 
	t.prototype = prototype
	for k, _ in pairs(t) do
		if not t.prototype[k] and k ~= "prototype" then
			error("Cannot set prototype, conflicting key: '"..k.."'.", 2)
		end
	end
	setmetatable(t, meta_check_prototype)
end

----
-- Locks globals,
-- no more globals can be created
--
local function globals_lock()
	local t = _G
	local mt = getmetatable(t) or {}
	mt.__index = function(t, k) 
		if (k~="_" and string.sub(k, 1, 2) ~= "__") then
			error("Access of non-existing global '"..k.."'", 2)
		else
			rawget(t, k)
		end
	end
	mt.__newindex = function(t, k, v) 
		if (k~="_" and string.sub(k, 1, 2) ~= "__") then
			error("Lsyncd does not allow GLOBALS to be created on the fly." ..
			      "Declare '" ..k.."' local or declare global on load.", 2)
		else
			rawset(t, k, v)
		end
	end
	setmetatable(t, mt)
end

-----
-- Holds information about a delayed event for one origin/target.
--
-- valid stati are:
--    delay
--    active
--    TODO
--
local Delay = (function()
	-----
	-- Creates a new delay.
	-- 
	-- @param TODO
	local function new(ename, pathname, alarm)
		local o = {
			ename = ename,
			alarm = alarm,
			pathname = pathname,
			status = "delay",
		}
		return o
	end

	return {new = new}
end)()

-----
-- Holds information about one observed directory inclusively subdirs.
--
local Sync = (function()
	----
	-- Creates a new origin
	--
	local function new(config) 
		local o = {
			config = config,
			delays = CountArray.new(),
			delayname = {},
			source = config.source,
			processes = CountArray.new(),
		}
		return o
	end

	-- public interface
	return {new = new}
end)()

-----
-- Puts an action on the delay stack.
--
function Sync.delay(self, ename, time, pathname, pathname2)
	log("Function", "delay(", self, ", ", ename, ", ", pathname, ")")
	local delays = self.delays
	local delayname = self.delayname

	if ename == "Move" and not self.config.move then
		-- if there is no move action defined, split a move as delete/create
		log("Debug", "splitting Move into Delete & Create")
		delay(self, "Delete", time, pathname,  nil)
		delay(self, "Create", time, pathname2, nil)
		return
	end

	-- creates the new action
	local alarm 
	-- TODO scope
	if time and self.config.delay then
		alarm = lsyncd.addto_clock(time, self.config.delay)
	else
		alarm = lsyncd.now()
	end
	local newd = Delay.new(ename, pathname, alarm)

	local oldd = delayname[pathname] 
	if oldd then
		-- if there is already a delay on this pathname.
		-- decide what should happen with multiple delays.
		if newd.ename == "MoveFrom" or newd.ename == "MoveTo" or
		   oldd.ename == "MoveFrom" or oldd.ename == "MoveTo" then
		   -- do not collapse moves
			log("Normal", "Not collapsing events with moves on ", pathname)
			-- TODO stackinfo
			return
		else
			local col = self.config.collapse_table[oldd.ename][newd.ename]
			if col == -1 then
				-- events cancel each other
				log("Normal", "Nullfication: ", newd.ename, " after ",
					oldd.ename, " on ", pathname)
				oldd.ename = "None"
				return
			elseif col == 0 then
				-- events tack
				log("Normal", "Stacking ", newd.ename, " after ",
					oldd.ename, " on ", pathname)
				-- TODO Stack pointer
			else
				log("Normal", "Collapsing ", newd.ename, " upon ",
					oldd.ename, " to ", col, " on ", pathname)
				oldd.ename = col
				return
			end
		end
		table.insert(delays, newd)
	else
		delayname[pathname] = newd
		table.insert(delays, newd)
	end
end

-----
-- Syncs - a singleton
-- 
-- It maintains all configured directories to be synced.
--
local Syncs = (function()
	-- the list of all origins
	local list = Array.new()
	
	-----
	-- inheritly copies all non integer keys from
	-- @cd copy destination
	-- to
	-- @cs copy source
	-- all integer keys are treated as new copy sources
	--
	local function inherit(cd, cs)
		-- first copies from source all 
		-- non-defined non-integer keyed values 
		for k, v in pairs(cs) do
			if type(k) ~= "number" and not cd[k] then
				cd[k] = v
			end
		end
		-- first recurses into all integer keyed tables
		for i, v in ipairs(cs) do
			if type(v) == "table" then
				inherit(cd, v)
			end
		end
	end
	
	-----
	-- Adds a new directory to observe.
	--
	local function add(config)
		-----
		-- Creates a new config table and inherit all keys/values
		-- from integer keyed tables
		local uconfig = config
		config = {}
		inherit(config, uconfig)

		------
		-- raises an error if @param name isnt in opts
		local function require_opt(name)
			if not config[name] then
				local info = debug.getinfo(3, "Sl")
				log("Error", info.short_src, ":", info.currentline,
					": ", name, " missing from sync.")
				terminate(-1) -- ERRNO
			end
		end
		require_opt("source")

		-- absolute path of source
		local real_src = lsyncd.real_dir(config.source)
		if not real_src then
			log(Error, "Cannot access source directory: ", config.source)
			terminate(-1) -- ERRNO
		end
		config._source = config.source
		config.source = real_src

		if not config.action   and not config.onAttrib and
		   not config.onCreate and not config.onModify and
		   not config.onDelete and not config.onMove
		then
			local info = debug.getinfo(3, "Sl")
			log("Error", info.short_src, ":", info.currentline,
				": no actions specified, use e.g. 'config=default.rsync'.")
			terminate(-1) -- ERRNO
		end

		-- loads a default value for an option if not existent
		local function optional(name)
			if config[name] then
				return
			end
			config[name] = settings[name] or default[name]
		end

		optional("action")
		optional("maxProcesses")
		optional("collapseTable")
		local s = Sync.new(config)
		table.insert(list, s)
	end

	-- allows to walk through all origins
	local function iwalk()
		return ipairs(list)
	end

	-- returns the number of origins
	local size = function()
		return #list
	end

	-- public interface
	return {add = add, iwalk = iwalk, size = size}
end)()

-----
-- Interface to inotify, watches recursively subdirs and 
-- sends events.
--
-- All inotify specific implementation should be enclosed here.
-- So lsyncd can work with other notifications mechanisms just
-- by changing this.
--
local Inotifies = (function()
	-----
	-- A list indexed by inotifies watch descriptor.
	-- Contains a list of all origins observing this directory
	-- (directly or by recurse)
	local wdlist = CountArray.new()

	-----
	-- Adds watches for a directory including all subdirectories.
	--
	-- @param root+path  directory to observe
	-- @param recurse    true if recursing into subdirs or 
	--                   the relative path to root for recursed inotifies
	-- @param origin     link to the observer to be notified.
	--                   Note: Inotifies should handle this opaquely
	local function add(root, path, recurse, origin)
		log("Function", 
			"Inotifies.add(",root,", ",path,", ",recurse,", ",origin,")")
		-- registers watch 
		local wd = lsyncd.add_watch(root .. path);
		if wd < 0 then
			log("Error","Failure adding watch ",dir," -> ignored ")
			return
		end

		if not wdlist[wd] then
			wdlist[wd] = Array.new()
		end
		table.insert(wdlist[wd], {
			root = root,
			path = path,
			recurse = recurse, 
			origin = origin
		})

		-- registers and adds watches for all subdirectories 
		if recurse then
			local subdirs = lsyncd.sub_dirs(root .. path)
			for _, dirname in ipairs(subdirs) do
				add(root, path..dirname.."/", true, origin)
			end
		end
	end

	-----
	-- Called when an event has occured.
	--
	-- @param ename     "Attrib", "Mofify", "Create", "Delete", "Move")
	-- @param wd        watch descriptor (matches lsyncd.add_watch())
	-- @param isdir     true if filename is a directory
	-- @param time      time of event
	-- @param filename  string filename without path
	-- @param filename2 
	--
	function event(ename, wd, isdir, time, filename, filename2)
		local ftype;
		if isdir then
			ftype = "directory"
			filename = filename .. "/"
			if filename2 then
				filename2 = filename2 .. "/"
			end
		end
		if filename2 then
			log("Inotify", "got event ", ename, " ", filename, 
				" to ", filename2) 
		else 
			log("Inotify", "got event ", ename, " ", filename) 
		end

		local ilist = wdlist[wd]
		-- looks up the watch descriptor id
		if not ilist then
			-- this is normal in case of deleted subdirs
			log("Inotify", "event belongs to unknown watch descriptor.")
			return
		end
	
		-- works through all observers interested in this directory
		for _, inotify in ipairs(ilist) do
			local pathname = inotify.path .. filename
			local pathname2 
			if filename2 then
				pathname2 = inotify.path..filename2
			end
			Sync.delay(inotify.origin, ename, time, pathname, pathname2)
			-- adds subdirs for new directories
			if inotify.recurse and isdir then
				if ename == "Create" then
					add(inotify.root, pathname, true, inotify.origin)
				elseif ename == "Delete" then
					-- TODO
				end
			end
		end
	end

	-----
	-- Writes a status report about inotifies to a filedescriptor
	--
	local function status_report(f)
		f:write("Watching ",wdlist:size()," directories\n")
		for wd, v in wdlist:iwalk() do
			f:write("  ",wd,": ")
			local sep = ""
			for _, v in ipairs(v) do
				f:write(v.root,"/",v.path or "",sep)
				sep = ", "
			end
			f:write("\n")
		end
	end

	-----
	-- Returns the number of directories watched in total.
	local function size()
		return wdlist:size()
	end

	-- public interface
	return { 
		add = add, 
		size = size, 
		event = event, 
		status_report = status_report 
	}
end)()

--============================================================================
-- lsyncd runner plugs. These functions will be called from core. 
--============================================================================

-----
-- true after lsyncd_initalized()
--
local running = false

-----
-- Called from core whenever a lua failed.
--
function lsyncd_call_error(message)
	log("Error", "IN LUA: ", message)
	-- prints backtrace
	local level = 2
	while true do
		local info = debug.getinfo(level, "Sl")
		if not info then
			terminate(-1) -- ERRNO
		end
		log("Error", "Backtrace ", level - 1, " :", 
			info.short_src, ":", info.currentline)
		level = level + 1
	end
end

-----
-- Called from code whenever a child process finished and 
-- zombie process was collected by core.
--
function lsyncd_collect_process(pid, exitcode) 
	local delay = nil
	local origin = nil
	for _, s in Syncs.iwalk() do
		delay = s.processes[pid]
		if delay then
			origin = s
			break
		end
	end
	if not delay then
		return
	end
	log("Debug", "collected ",pid, ": ",delay.ename," of ",
		origin.source,delay.pathname," = ",exitcode)
	origin.processes[pid] = nil
end

-----
-- User interface to grap events
--
-- inlet_control is the Luas runner part to control the interface
-- hidden from the user.
--
local Inlet, inlet_control = (function()
	-- lua runner controlled variables
	local origin  = true
	local delay   = true

	-- event to be passed to the user
	local event = {}

	-----
	-- Interface for the user to get fields.
	--
	local event_fields = {
		config = function()
			return origin.config
		end,

		etype = function()
			return delay.ename
		end,

		name = function()
			return string.match(delay.pathname, "[^/]+/?$")
		end,
		
		basename = function()
			return string.match(delay.pathname, "([^/]+)/?$")
		end,

		pathname = function()
			return delay.pathname
		end,
		
		pathbasename = function()
			if string.byte(delay.pathname, -1) == 47 then
				return string.sub(delay.pathname, 1, -2)
			else 
				return delay.pathname
			end
		end,
		
		root = function()
			return origin.source
		end,
	}
	local event_meta = {
		__index = function(t, k)
			local f=event_fields[k]
			if not f then
				error("event does not have field '"..k.."'", 2)
			end
			return f()
		end
	}
	setmetatable(event, event_meta)

	-----	
	-- Interface for lsyncd runner to control what
	-- the inlet will present the user.
	--
	local function control(set_origin, set_delay)
		origin = set_origin
		delay  = set_delay
	end

	-----
	-- Gets the next event from queue.
	--
	local function get_event()
		-- TODO actually aquire here
		return event
	end

	------
	-- Returns the configuration table specified by sync{}
	--
	local function get_config()
		-- TODO give a readonly handler only.
		return origin.config
	end

	------
	-- public interface
	return {get_event = get_event, get_config = get_config}, control
end)()

-----
-- TODO
--
--
local function invoke_action(origin, delay)
	if delay.ename == "None" then
		-- a removed action
		return
	end

	inlet_control(origin, delay)
	local pid = origin.config.action(Inlet)
	if pid and pid > 0 then
		delay.status = "active"
		origin.processes[pid] = delay
	end
end
	

----
-- Writes a status report file at most every [statusintervall] seconds.
--
--
local StatusFile = (function() 
	-----
	-- Timestamp when the status file has been written.
	local lastWritten = false

	-----
	-- Timestamp when a statusfile should be written
	local alarm = false

	-----
	-- Returns when the statusfile should be written
	--
	local function getAlarm()
		return alarm
	end

	-----
	-- Called to check if to write a status file.
	--
	local function write(now)
		log("Function", "write(", now, ")")

		-- some logic to not write too often
		if settings.statusIntervall > 0 then
			-- already waiting
			if alarm and lsyncd.earlier(now, alarm) then
				log("Statusfile", "waiting")
				return
			end
			-- when a next Write will be possible
			if not alarm then
				local nextWrite = lastWritten and
					lsyncd.addto_clock(now, settings.statusIntervall)
				if nextWrite and lsyncd.earlier(now, nextWrite) then
					log("Statusfile", "setting alarm", nextWrite)
					alarm = nextWrite
					return
				end
			end
			lastWritten = now
			alarm = false
		end

		log("Statusfile", "writing now")
		local f, err = io.open(settings.statusfile, "w")
		if not f then
			log("Error", "Cannot open statusfile '"..settings.statusfile..
				"' :"..err)
			return
		end
		f:write("Lsyncd status report at ", os.date(), "\n\n")
		Inotifies.status_report(f)
		f:close()
	end

	-- public interface
	return {write = write, getAlarm = getAlarm}
end)()

----
-- Called from core everytime a masterloop cycle runs through.
-- This happens in case of 
--   * an expired alarm.
--   * a returned child process.
--   * received inotify events.
--   * received a HUP or TERM signal.
--
-- @param now   the current kernel time (in jiffies)
--
function lsyncd_cycle(now)
	-- goes through all targets and spawns more actions
	-- if possible
	if settings.statusfile then
		StatusFile.write(now)
	end
	for _, s in Syncs.iwalk() do
		if s.processes:size() < s.config.max_processes then
			local delays = s.delays
			local d = delays[1]
			if d and lsyncd.before_eq(d.alarm, now) then
				invoke_action(s, d)
				table.remove(delays, 1)
				s.delayname[d.pathname] = nil -- TODO grab from stack
			end
		end
	end
end


-----
-- Called by core before anything is "-help" or "--help" is in
-- the arguments.
--
function lsyncd_help()
	io.stdout:write(
[[
USAGE: 
  run a config file:
    lsyncd [OPTIONS] [CONFIG-FILE]

  default rsync behaviour:
    lsyncd [OPTIONS] -rsync [SOURCE] [TARGET1]  [TARGET2] ...

OPTIONS:
  -help               Shows this
  -log    all         Logs everything
  -log    scarce      Logs errors only
  -log    [Category]  Turns on logging for a debug category
  -runner FILE        Loads lsyncds lua part from FILE 

LICENSE:
  GPLv2 or any later version.

SEE:
  `man lsyncd` for further information.

]])
	os.exit(-1) -- ERRNO
end


-----
-- Called from core to parse the command line arguments
-- @returns a string as user script to load.
--          or simply 'true' if running with rsync bevaiour
-- terminates on invalid arguments
--
function lsyncd_configure(args)
	-- a list of all valid --options
	local options = {
		-- log is handled by core already.
		log = {1},

	}
	-- filled with all args that were non --options
	local nonopts = {}
	local i = 1
	while i <= #args do
		local a = args[i]
		if a:sub(1, 1) ~= "-" then
			table.insert(nonopts, args[i])
		else
			if a:sub(1, 2) == "--" then
				a = a:sub(3)
			else
				a = a:sub(2)
			end
			local o = options[a]
			if o then
				-- TODO --
				i = i + o[1]
			end
		end
		i = i + 1
	end

	if #nonopts == 0 then
		lsyncd_help(args[0])
	elseif #nonopts == 1 then
		return nonopts[1]
	else 
		-- TODO
		return true
	end
end


----
-- Called from core on init or restart after user configuration.
-- 
function lsyncd_initialize()
	-- creates settings if user didnt
	settings = settings or {}

	-- From this point on, no globals may be created anymore
	globals_lock()

	-- all valid settings, first value is 1 if it needs a parameter 
	local configure_settings = {
		statusfile = {1, nil},
	}

	-- check all entries in the settings table
	for c, p in pairs(settings) do
		local cs = configure_settings[c]
		if not cs then
			log("Error", "unknown setting '", c, "'")	
			terminate(-1) -- ERRNO
		end
		if cs[1] == 1 and not p then
			log("Error", "setting '", c, "' needs a parameter")	
		end
		-- calls the check function if its not nil
		if cs[2] then
			cs[2](p)
		end
		lsyncd.configure(c, p)
	end

	-- makes sure the user gave lsyncd anything to do 
	if Syncs.size() == 0 then
		log("Error", "Nothing to watch!")
		log("Error", "Use sync(SOURCE, TARGET, BEHAVIOR) in your config file.");
		terminate(-1) -- ERRNO
	end

	-- set to true if at least one origin has a startup function
	local have_startup = false
	-- runs through the origins table filled by user calling directory()
	for _, s in Syncs.iwalk() do
		if s.config.onStartup then
			have_startup = true
		end
		-- adds the dir watch inclusively all subdirs
		Inotifies.add(s.source, "", true, o)
	end

	-- from now on use logging as configured instead of stdout/err.
	running = true;
	lsyncd.configure("running");
	
	if have_startup then
		log("Normal", "--- startup ---")
		local pids = { }
		for _, s in Syncs.iwalk() do
			local pid
			if s.config.onStartup then
				local pid = s.config.onStartup(s.config)
				table.insert(pids, pid)
			end
		end
		lsyncd.wait_pids(pids, "startup_collector")
		log("Normal", "- Entering normal operation with ",
			Inotifies.size(), " monitored directories -")
	else
		log("Normal", "- Warmstart into normal operation with ",
			Inotifies.size(), " monitored directories -")
	end
end

----
-- Called by core to query soonest alarm.
--
-- @return false ... no alarm, core can in untimed sleep, or
--         times ... the alarm time (only read if number is 1)
--
function lsyncd_get_alarm()
	local alarm = false
	for _, s in Syncs.iwalk() do
		-- TODO better handling of stati.
		if s.delays[1] and 
		   s.processes:size() < s.config.max_processes then
			if alarm then
				alarm = lsyncd.earlier(alarm, s.delays[1].alarm)
			else
				alarm = s.delays[1].alarm
			end
		end
	end
	local sa = StatusFile.getAlarm()
	if sa then
		alarm = lsyncd.earlier(sa, alarm) 
	end

	log("Debug", "lysncd_get_alarm returns: ",alarm)
	return alarm
end

lsyncd_inotify_event = Inotifies.event

-----
-- Collector for every child process that finished in startup phase
--
-- Parameters are pid and exitcode of child process
--
-- Can return either a new pid if one other child process 
-- has been spawned as replacement (e.g. retry) or 0 if
-- finished/ok.
--
function startup_collector(pid, exitcode)
	if exitcode ~= 0 then
		log("Error", "Startup process", pid, " failed")
		terminate(-1) -- ERRNO
	end
	return 0
end


--============================================================================
-- lsyncd user interface
--============================================================================

-----
-- Main utility to create new observations.
--
function sync(opts)
	if running then
		error("Cannot add syncs while running!")
	end
	Syncs.add(opts)
end

----
-- Called by core when an overflow happened.
--
function default_overflow()
	log("Error", "--- OVERFLOW on inotify event queue ---")
	terminate(-1) -- TODO reset instead.
end
overflow = default_overflow

-----
-- Spawns a child process using bash.
--
function shell(command, ...)
	return lsyncd.exec("/bin/sh", "-c", command, "/bin/sh", ...)
end

function exec(...)
	return lsyncd.exec(...)
end

--============================================================================
-- lsyncd default settings
--============================================================================

-----
-- lsyncd classic - sync with rsync
--
local default_rsync = {
	----
	-- Called for every sync/target pair on startup
	startup = function(source, target) 
		log("Normal", "startup recursive rsync: ", source, " -> ", target)
		return exec("/usr/bin/rsync", "-ltrs", 
			source, target)
	end,

	default = function(source, target, path)
		return exec("/usr/bin/rsync", "--delete", "-ltds",
			source.."/"..path, target.."/"..path)
	end
}

-----
-- The default table for the user to access 
--   TODO make readonly
-- 
default = {
	-----
	-- Default action
	-- TODO desc
	--
	action = function(inlet)
		local event = inlet.get_event()
		local config = inlet.get_config()
		local func = config["on".. event.etype]
		if func then
			-- TODO Moves?
			return func(config, event)
		else 
			return -1
		end
	end,

	-----
	-- TODO
	--
	maxProcesses = 1,

	------
	-- Minimum seconds between two writes of a status file.
	--
	statusIntervall = 60,

	------
	-- TODO
	--
	collapseTable = {
		Attrib = { Attrib = "Attrib", Modify = "Modify", Create = "Create", Delete = "Delete" },
		Modify = { Attrib = "Modify", Modify = "Modify", Create = "Create", Delete = "Delete" },
		Create = { Attrib = "Create", Modify = "Create", Create = "Create", Delete = -1       },
		Delete = { Attrib = "Delete", Modify = "Delete", Create = "Modify", Delete = "Delete" },
	},

	rsync = default_rsync
}


