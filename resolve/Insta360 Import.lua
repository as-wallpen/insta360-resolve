-- Insta360 Import: choose Insta360 X5 .insv clips and prepare them for editing.
--
-- For each clip the import tool (tools/ingest.py, run with Resolve's own Python) copies the two
-- lenses into separate files and reads and refines the lens calibration, showing a progress
-- window. This script then builds the clip in the open project: a compound clip "<name> 360"
-- in the "Insta360" bin, ready for the Insta360 Stitch effect.
--
-- Everything that touches the project happens here, inside Resolve, so no external scripting
-- setting is needed. @INGEST@ is filled in when the script is installed.
--
-- Windows: dialogs come from PowerShell, and file names travel in UTF-8 files rather than on
-- command lines, which Resolve's Lua can only pass in the ANSI code page.
local INGEST = [[@INGEST@]]
-- Windows installs write %ProgramData%\... here; expand such variables.
INGEST = INGEST:gsub("%%([%w_]+)%%", function(name) return os.getenv(name) end)
local BIN_NAME = "Insta360"
local SOURCE_BIN_NAME = "Source files"   -- inside BIN_NAME: the lens files the 360 clips are made from
local WINDOWS = package.config:sub(1, 1) == "\\"
local TEMP = WINDOWS and (os.getenv("TEMP") or os.getenv("TMP") or "C:\\Windows\\Temp") or "/tmp"
if WINDOWS and (TEMP:find("[^%w%s%p]") or TEMP:find("?", 1, true)) then
    -- A user folder with characters beyond ASCII ("Jürgen", "Łukasz"): Lua sees it only in the ANSI
    -- code page, PowerShell scripts are UTF-8. Work in a plain folder all users may create instead.
    TEMP = (os.getenv("ProgramData") or "C:\\ProgramData") .. "\\Insta360Stitch-temp"
    os.execute('mkdir "' .. TEMP .. '" 2>nul')
end
local RUN_ID = tostring(os.time()) .. "-" .. tostring(math.random(100000, 999999))

local PYTHONS
if WINDOWS then
    -- Resolve 21.1 and later bring a Python; otherwise one from python.org, for this user or all users.
    local programs = os.getenv("ProgramFiles") or "C:\\Program Files"
    local userPrograms = (os.getenv("LOCALAPPDATA") or "") .. "\\Programs\\Python"
    PYTHONS = {programs .. "\\Blackmagic Design\\DaVinci Resolve\\ResolvePython\\ResolvePython.exe"}
    for minor = 14, 9, -1 do
        table.insert(PYTHONS, userPrograms .. "\\Python3" .. minor .. "\\python.exe")
        table.insert(PYTHONS, programs .. "\\Python3" .. minor .. "\\python.exe")
    end
else
    PYTHONS = {
        "/Applications/DaVinci Resolve/DaVinci Resolve.app/Contents/Applications/ResolvePython",
        "/opt/homebrew/bin/python3",
        "/usr/local/bin/python3",
        "/usr/bin/python3",
    }
end

local function quote(s)
    if WINDOWS then
        return '"' .. s .. '"'
    end
    return "'" .. (s:gsub("'", "'\\''")) .. "'"
end

-- Runs a command line. cmd.exe drops the first and the last quote of what it is given, so on
-- Windows the whole line goes in one more pair.
local function run(command)
    if WINDOWS then
        return os.execute('"' .. command .. '"')
    end
    return os.execute(command)
end

local function exists(path)
    local f = io.open(path, "r")
    if f then
        f:close()
        return true
    end
    return false
end

local function writeFile(path, text)
    local f = io.open(path, "wb")
    if not f then
        return false
    end
    f:write(text)
    f:close()
    return true
end

-- Base64 of a string, for PowerShell's -EncodedCommand.
local function base64(data)
    local alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
    local out = {}
    for i = 1, #data, 3 do
        local a, b, c = data:byte(i, i + 2)
        local n = a * 65536 + (b or 0) * 256 + (c or 0)
        local chars = {}
        for k = 3, 0, -1 do
            local index = math.floor(n / 64 ^ k) % 64
            table.insert(chars, alphabet:sub(index + 1, index + 1))
        end
        if not b then
            chars[3], chars[4] = "=", "="
        elseif not c then
            chars[4] = "="
        end
        table.insert(out, table.concat(chars))
    end
    return table.concat(out)
end

-- Runs PowerShell commands without a console window (-WindowStyle Hidden also hides the console
-- of the cmd.exe that os.execute starts). Passed as -EncodedCommand rather than a script file,
-- which a group policy on script execution would refuse. The commands must be plain ASCII
-- (anything else comes from files or environment variables); -EncodedCommand wants UTF-16.
local function powershell(lines)
    local text = table.concat(lines, "\n")
    local utf16 = text:gsub(".", function(c) return c .. "\0" end)
    return run("powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden -EncodedCommand " .. base64(utf16))
end

-- A Lua string as a PowerShell single-quoted string.
local function psString(s)
    return "'" .. s:gsub("'", "''") .. "'"
end

local function dialog(text)
    if WINDOWS then
        -- The text goes through a file: it can hold any character, and PowerShell reads it as UTF-8.
        local path = TEMP .. "\\insta360-message-" .. RUN_ID .. ".txt"
        writeFile(path, text)
        powershell({
            "Add-Type -AssemblyName System.Windows.Forms",
            "$owner = New-Object System.Windows.Forms.Form -Property @{TopMost = $true; ShowInTaskbar = $false}",
            "$text = [IO.File]::ReadAllText(" .. psString(path) .. ", [Text.Encoding]::UTF8)",
            "[void][System.Windows.Forms.MessageBox]::Show($owner, $text, 'Insta360 Import')",
        })
        os.remove(path)
        return
    end
    run("osascript -e " .. quote('display dialog "' .. text:gsub("\\", "\\\\"):gsub('"', '\\"')
        .. '" with title "Insta360 Import" buttons {"OK"} default button 1') .. " >/dev/null 2>&1")
end

-- The clips the user picks, one path per line (UTF-8 on Windows, where the file dialog writes
-- them to a file). INSTA360_FILES (newline separated) skips the file dialog, for testing.
local listFile = TEMP .. (WINDOWS and "\\" or "/") .. "insta360-clips-" .. RUN_ID .. ".txt"
local list = os.getenv("INSTA360_FILES")
if list then
    writeFile(listFile, list)
elseif WINDOWS then
    powershell({
        "Add-Type -AssemblyName System.Windows.Forms",
        "$owner = New-Object System.Windows.Forms.Form -Property @{TopMost = $true; ShowInTaskbar = $false}",
        "$d = New-Object System.Windows.Forms.OpenFileDialog",
        "$d.Title = 'Choose Insta360 .insv clips to import'",
        "$d.Filter = 'Insta360 clips (*.insv)|*.insv|All files (*.*)|*.*'",
        "$d.Multiselect = $true",
        "if ($d.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) {",
        "    [IO.File]::WriteAllLines(" .. psString(listFile) .. ", $d.FileNames, (New-Object System.Text.UTF8Encoding $false))",
        "}",
    })
    local f = io.open(listFile, "rb")
    list = f and f:read("*a") or ""
    if f then
        f:close()
    end
else
    local script = {
        'set picked to choose file with prompt "Choose Insta360 .insv clips to import" with multiple selections allowed',
        'set out to ""',
        'repeat with f in picked',
        'set out to out & POSIX path of f & linefeed',
        'end repeat',
        'return out',
    }
    local cmd = "osascript"
    for _, line in ipairs(script) do
        cmd = cmd .. " -e " .. quote(line)
    end
    local pipe = io.popen(cmd .. " 2>/dev/null")
    list = pipe:read("*a")
    pipe:close()
    writeFile(listFile, list)
end
local count = 0
for path in list:gmatch("[^\r\n]+") do
    if path:lower():match("%.insv$") then
        count = count + 1
    end
end
if count == 0 then
    os.remove(listFile)
    print("Insta360 Import: no .insv files chosen")
    return
end

local resolveApp = resolve or (Resolve and Resolve()) or (bmd and bmd.scriptapp("Resolve"))
local project = resolveApp and resolveApp:GetProjectManager():GetCurrentProject()
if not project then
    os.remove(listFile)
    dialog("Open a project in DaVinci Resolve first.")
    return
end

local python
for _, candidate in ipairs(PYTHONS) do
    if exists(candidate) then
        python = candidate
        break
    end
end
if not python then
    os.remove(listFile)
    dialog(WINDOWS and "No Python found. Insta360 Import needs DaVinci Resolve 21.1 or later, which includes one, "
        .. "or Python 3 from python.org." or "No Python found. Insta360 Import needs DaVinci Resolve 21.1 or later, which includes one.")
    return
end

-- 1. Copy the lenses and work out the calibration (shows its own progress window).
local report = TEMP .. (WINDOWS and "\\" or "/") .. "insta360-report-" .. RUN_ID .. ".tsv"
local arguments = quote(INGEST) .. " --no-resolve --progress --report " .. quote(report) .. " --list " .. quote(listFile)
if WINDOWS then
    -- Without a console window (closing one would cut the import short); the progress window
    -- shows what is going on.
    powershell({
        "$p = Start-Process -FilePath " .. psString(python) .. " -ArgumentList " .. psString(arguments)
            .. " -WindowStyle Hidden -Wait -PassThru",
    })
else
    run(quote(python) .. " " .. arguments .. " >/dev/null 2>&1")
end
os.remove(listFile)

local clips, messages = {}, {}
local f = io.open(report, "r")
if f then
    for line in f:lines() do
        local fields = {}
        for field in (line .. "\t"):gmatch("([^\t]*)\t") do
            table.insert(fields, field)
        end
        if fields[1] == "FAILED" then
            table.insert(messages, fields[2])
        elseif #fields >= 6 then
            table.insert(clips, {name = fields[1], lensA = fields[2], lensB = fields[3], width = tonumber(fields[4]),
                                 height = tonumber(fields[5]), fps = fields[6], note = fields[7] or "",
                                 tag = fields[8] or ""})
        end
    end
    f:close()
end
os.remove(report)
if #clips == 0 and #messages == 0 then
    dialog("The import tool did not run. Check that the Insta360 Stitch plugin is installed.")
    return
end

-- 2. Build each prepared clip in the project.
local pool = project:GetMediaPool()
local folder
for _, sub in ipairs(pool:GetRootFolder():GetSubFolderList() or {}) do
    if sub:GetName() == BIN_NAME then
        folder = sub
    end
end
folder = folder or pool:AddSubFolder(pool:GetRootFolder(), BIN_NAME)

-- The bin shows only the "<name> 360" clips people edit with; the lens files they are made
-- from go into a sub-bin. Lens files left in the bin by earlier imports are tidied away too.
local sourceFolder
for _, sub in ipairs(folder:GetSubFolderList() or {}) do
    if sub:GetName() == SOURCE_BIN_NAME then
        sourceFolder = sub
    end
end
sourceFolder = sourceFolder or pool:AddSubFolder(folder, SOURCE_BIN_NAME)
local stray = {}
for _, item in ipairs(folder:GetClipList() or {}) do
    local name = item:GetName() or ""
    if name:match("_lens[AB]%.mov$") or name:match("_tag%.mov$") then
        table.insert(stray, item)
    end
end
if #stray > 0 then
    pool:MoveClips(stray, sourceFolder)
end
pool:SetCurrentFolder(folder)

local function basename(path)
    return path:match("([^/\\]+)$")
end

-- The clip's tag (see tools/clip_tag.py), a transparent movie as long as the clip, over it on a
-- third track, so the plugin can tell which clip it is working on.
local function addTagTrack(timeline, tagPath, start)
    -- (Lua cannot check files with names outside the ANSI code page on Windows; the import tool
    -- only reports a tag movie it has made.)
    if tagPath == "" or (not WINDOWS and not exists(tagPath)) then
        return {}, nil
    end
    local imported = pool:ImportMedia({tagPath}) or {}
    if not imported[1] then
        return {}, nil
    end
    timeline:AddTrack("video")
    local items = pool:AppendToTimeline({{mediaPoolItem = imported[1], trackIndex = 3, recordFrame = start}}) or {}
    for _, item in ipairs(items) do
        item:SetProperty("Scaling", 4)
    end
    return items, imported[1]
end

-- Diagnostics for layouts Resolve did not accept, in a file the user can send.
local LOG = WINDOWS and ((os.getenv("APPDATA") or TEMP) .. "\\Insta360Stitch\\Insta360Import.log")
    or ((os.getenv("HOME") or "/tmp") .. "/Library/Logs/Insta360Import.log")
-- How to name it in a dialog (on Windows the real path is in the ANSI code page, the dialog UTF-8).
local LOG_NAME = WINDOWS and "%APPDATA%\\Insta360Stitch\\Insta360Import.log" or LOG
local function log(text)
    local f = io.open(LOG, "a")
    if f then
        f:write(os.date("%Y-%m-%d %H:%M:%S  ") .. text .. "\n")
        f:close()
    end
end

-- Puts one lens on its half of the frame (see the comment in build) and checks that Resolve
-- really applied it, trying the all-at-once form if the one-by-one form was not taken.
local function placeLens(item, side, width)
    -- Pan units differ between Macs (on one, 1920 moved a lens 3840 pixels). Resolve limits Pan
    -- to 4 frame widths in its own units, so asking for far too much and reading back the limit
    -- tells how many units a frame width is; a quarter width is a sixteenth of the limit.
    item:SetProperty("Scaling", 4)
    item:SetProperty("Pan", 1e9)
    local limit = tonumber(item:GetProperty("Pan")) or 0
    local quarter = limit > 0 and limit / 16.0 or width / 4.0
    log(string.format("lens shift: Pan limit %s, frame width %d px, quarter width %.1f units", tostring(limit), width, quarter))
    local wanted = {Scaling = 4, ZoomGang = false, ZoomX = 0.5, ZoomY = 1.0, Pan = side * quarter}
    local function applied()
        for key, value in pairs(wanted) do
            local got = item:GetProperty(key)
            if type(value) == "number" then
                if type(got) ~= "number" or math.abs(got - value) > 1e-3 * math.max(1, math.abs(value)) then
                    return false, key .. "=" .. tostring(got)
                end
            elseif got ~= value and not (value == false and (got == 0 or got == nil)) then
                return false, key .. "=" .. tostring(got)
            end
        end
        return true
    end
    for _, key in ipairs({"Scaling", "ZoomGang", "ZoomX", "ZoomY", "Pan"}) do
        item:SetProperty(key, wanted[key])
    end
    local ok, problem = applied()
    if not ok then
        log("lens layout not taken one by one (" .. problem .. "); trying SetProperties")
        local all = {RetimeAndScalingEnabled = true, TransformEnabled = true}
        for key, value in pairs(wanted) do
            all[key] = value
        end
        if item.SetProperties then
            item:SetProperties(all)
        end
        ok, problem = applied()
    end
    if not ok then
        log("lens layout rejected by Resolve " .. tostring(resolveApp:GetVersionString()) .. ": " .. problem)
    end
    return ok, problem
end

local function build(clip)
    local compoundName = clip.name .. " 360"
    for _, existing in ipairs(folder:GetClipList() or {}) do
        if existing:GetName() == compoundName then
            return "already in the bin, skipped"
        end
    end

    if project:GetTimelineCount() == 0 then
        project:SetSetting("timelineFrameRate", clip.fps)
    end
    local imported = pool:ImportMedia({clip.lensA, clip.lensB}) or {}
    local a, b
    for _, item in ipairs(imported) do
        if item:GetName() == basename(clip.lensA) then a = item end
        if item:GetName() == basename(clip.lensB) then b = item end
    end
    if not a or not b then
        return "FAILED: Resolve did not import both lens files"
    end

    local timeline = pool:CreateEmptyTimeline(clip.name .. " dual")
    if not timeline then
        return "FAILED: could not create its timeline (name already used?)"
    end
    local width, height = 2 * clip.width, clip.height
    timeline:SetSetting("useCustomSettings", "1")
    timeline:SetSetting("timelineResolutionWidth", tostring(width))
    timeline:SetSetting("timelineResolutionHeight", tostring(height))
    if tonumber(timeline:GetSetting("timelineFrameRate")) ~= tonumber(clip.fps) then
        timeline:SetSetting("timelineFrameRate", clip.fps)
    end
    local gotW, gotH = tonumber(timeline:GetSetting("timelineResolutionWidth")), tonumber(timeline:GetSetting("timelineResolutionHeight"))
    log(string.format("%s: Resolve %s, lenses %dx%d, working timeline asked %dx%d got %sx%s",
        clip.name, tostring(resolveApp:GetVersionString()), clip.width, clip.height, width, height, tostring(gotW), tostring(gotH)))
    -- Resolve can make the timeline smaller than asked (seen on 21.1.2: half size). The layout
    -- only needs it to be 2:1, and the lens shift is worked out from the size it really has.
    if not gotW or not gotH or gotW <= 0 or math.abs(gotW - 2 * gotH) > 2 then
        return string.format("FAILED: Resolve would not make a 2:1 timeline for the lenses (asked %dx%d, got %sx%s). "
            .. "Please send " .. LOG_NAME, width, height, tostring(gotW), tostring(gotH))
    end
    width, height = gotW, gotH

    timeline:AddTrack("video")
    local start = timeline:GetStartFrame()
    local itemA = (pool:AppendToTimeline({{mediaPoolItem = a, trackIndex = 1, recordFrame = start}}) or {})[1]
    local itemB = (pool:AppendToTimeline({{mediaPoolItem = b, trackIndex = 2, recordFrame = start}}) or {})[1]
    if not itemA or not itemB then
        return "FAILED: could not place the lenses on its timeline"
    end
    -- Same layout as tools/ingest.py build(): each lens stretched over the frame, squeezed to
    -- half its width and moved a quarter of the width to its side, so lens A fills the left half
    -- and lens B the right half in a timeline of any shape.
    for _, placed in ipairs({{itemA, -1}, {itemB, 1}}) do
        local ok, problem = placeLens(placed[1], placed[2], width)
        if not ok then
            return "FAILED: Resolve did not accept the lens layout (" .. problem .. "). Please send " .. LOG_NAME
        end
    end
    local tagItems, tagMedia = addTagTrack(timeline, clip.tag, start)
    local parts = {itemA, itemB}
    for _, item in ipairs(tagItems) do
        table.insert(parts, item)
    end
    if not timeline:CreateCompoundClip(parts, {name = compoundName}) then
        return "FAILED: could not create the compound clip"
    end
    -- The working timeline is not needed once the compound clip exists (it keeps its own copy),
    -- and the lens files go out of sight into the sub-bin.
    pool:DeleteTimelines({timeline})
    pool:MoveClips(tagMedia and {a, b, tagMedia} or {a, b}, sourceFolder)
    pool:SetCurrentFolder(folder)
    return "ready"
end

local imported = 0
for _, clip in ipairs(clips) do
    local ok, result = pcall(build, clip)
    result = ok and result or ("FAILED: " .. tostring(result))
    if result == "ready" then
        imported = imported + 1
    end
    table.insert(messages, clip.name .. ": " .. result .. (clip.note ~= "" and (" (" .. clip.note .. ")") or ""))
end
if imported > 0 then
    table.insert(messages, "")
    table.insert(messages, "Use the '<name> 360' clips in the '" .. BIN_NAME .. "' bin: put one on a timeline and add the Insta360 Stitch effect. "
        .. "(The lens files they are made from are in '" .. SOURCE_BIN_NAME .. "'; leave them there.)")
end
print("Insta360 Import:\n" .. table.concat(messages, "\n"))
dialog(table.concat(messages, "\n"))
