-- pokeweb browser adapter, GPL-3.0; upstream game sources remain untouched.
-- This love.js build cannot safely unwind a missing-file C exception through a coroutine.
local fsRead = love.filesystem.read
love.filesystem.read = function(path, ...)
  if type(path) == 'string' and not love.filesystem.getInfo(path, 'file') then return nil, 'File not found: ' .. path end
  return fsRead(path, ...)
end
local newImage = love.graphics.newImage
love.graphics.newImage = function(path, ...)
  if type(path) == 'string' and not love.filesystem.getInfo(path, 'file') then
    error('Image not found: ' .. path, 2)
  end
  return newImage(path, ...)
end
bit = require('bit')
-- LuaJIT accepts Lua 5.2's string/chunk/environment signature; stock 5.1 does not.
local originalLoad = load
load = function(chunk, name, mode, env)
  local fn, err
  if type(chunk) == 'string' then
    if mode == 't' and chunk:byte(1) == 27 then return nil, 'binary chunk refused' end
    fn, err = loadstring(chunk, name)
  else fn, err = originalLoad(chunk, name) end
  if fn and env then setfenv(fn, env) end
  return fn, err
end
local Json = require('src.link.Json')
local function emit(kind, fields)
  fields = fields or {}; fields.type = kind
  print('POKEWEB:' .. Json.encode(fields))
end
local function read(path)
  local f = io.open(path, 'rb'); if not f then return nil end
  local s = f:read('*a'); f:close(); return s
end
local config = Json.decode(assert(read('/session.json')))
local GameVersion = require('src.core.GameVersion')
GameVersion.set(config.game)
-- Every iframe owns one game and one fresh filesystem. No FFI mount required.
GameVersion.cachePrefix = function() return '' end
local CacheFs = require('src.import.CacheFs')
CacheFs.prefix = ''
local game, boot, held, lastCommand = nil, nil, {}, nil
local paused = false
local lastRequest = nil
local function bootGame()
  local generation = GameVersion.generation()
  if not config.cached then
  local manifest = require('src.import.RomManifest').decode(config.game)
  local Extractor = require(generation == 3 and 'src.import.RomExtractorGen3'
    or generation == 2 and 'src.import.RomExtractorGen2' or 'src.import.RomExtractor')
  local last = 0
  local extractor = Extractor.new(assert(read('/cartridge.bin')), manifest,
    function(progress, total, stage)
      local now = love.timer.getTime()
      if now - last > 0.008 then
        emit('progress', {message = stage or 'Importing cartridge', progress = progress, total = total})
        coroutine.yield(); last = love.timer.getTime()
      end
    end, config.sha1)
  local protectedCall = pcall
  if generation == 3 then pcall = require('yield') end
  extractor:run()
  pcall = protectedCall
  emit('cache', {root = love.filesystem.getSaveDirectory()})
  end
  local SaveData = require('src.core.SaveData')
  SaveData.setCart(nil, nil)
  local options = read('/options.bin')
  if options then
    assert(require('src.core.SaveSerializer').decode(options))
    love.filesystem.write('options.lua', options)
  end
  local filename = SaveData.saveFilename(config.game)
  local save = read('/save.bin')
  if save then
    local decoded = assert(require('src.core.SaveSerializer').decode(save))
    assert(decoded.version == config.game, 'Save belongs to another game')
    local parent = filename:match('^(.*)/'); if parent then love.filesystem.createDirectory(parent) end
    assert(love.filesystem.write(filename, save))
  end
  local originalSave = SaveData.save
  SaveData.save = function(...)
    local ok = originalSave(...)
    if ok then emit('save', {path = love.filesystem.getSaveDirectory() .. '/' .. SaveData.saveFilename(config.game), optionsPath = love.filesystem.getSaveDirectory() .. '/options.lua'}) end
    return ok
  end
  game = generation == 3 and require('src.core.Game3').new()
    or generation == 2 and require('src.core.Game2').new() or require('src.core.Game')
  local function exit() emit('exit') end
  game.returnToLauncher = exit
  game:load({onExit = exit})
  game.speedOverride = 1
  emit('ready')
end
function love.load()
  love.graphics.setDefaultFilter('nearest', 'nearest')
  love.keyboard.isDown = function(...)
    for _, key in ipairs({...}) do if held[key] then return true end end
    return false
  end
  boot = coroutine.create(bootGame)
end
function love.update(dt)
  if boot then
    local ok, err = coroutine.resume(boot)
    if not ok then error(debug.traceback(boot, err)) end
    if coroutine.status(boot) == 'dead' then boot = nil end
    return
  end
  local command = read('/command.json')
  if command and command ~= lastCommand then
    lastCommand = command
    local c = Json.decode(command)
    paused = c.paused == true
    game.speedOverride = c.speed == 2 and 2 or 1
    love.audio.setVolume(paused and 0 or (c.volume or 0))
    local nextHeld = {}
    for _, key in ipairs(c.keys or {}) do nextHeld[key] = true end
    for key in pairs(held) do if not nextHeld[key] then game:keyreleased(key) end end
    for key in pairs(nextHeld) do if not held[key] then game:keypressed(key) end end
    held = nextHeld
  end
  local request = read('/request.json')
  if request and request ~= lastRequest then
    lastRequest = request
    local r = Json.decode(request)
    local message
    if r.type == 'flush' then paused = true end
    if r.type == 'save' then
      if game.save and game.quickSaveAllowed and game:quickSaveAllowed() then
        local ok
      if game.saveGame then ok = game:saveGame() elseif game.writeSave then ok = game:writeSave() end
      if not ok then message = 'The game could not write this save. Try saving from its menu.' end
      else message = 'Save from the field after starting your journey.' end
    end
    emit('ack', {id = r.id, message = message})
  end
  if game and not paused then game:update(math.min(dt, 0.1)) end
end
function love.draw() if game and not boot then game:draw() end end
function love.resize(w, h) if game and game.resize then game:resize(w, h) end end
function love.errorhandler(message)
  emit('error', {message = tostring(message)})
  return function() return 1 end
end
