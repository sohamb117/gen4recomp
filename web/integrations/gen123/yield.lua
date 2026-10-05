-- Yieldable protected calls for extraction only. Gameplay uses native pcall.
local function pack(...) return { n = select('#', ...), ... } end
return function(fn, ...)
  local thread = coroutine.create(function(...) return fn(...) end)
  local args = pack(...)
  while true do
    local result = pack(coroutine.resume(thread, unpack(args, 1, args.n)))
    if not result[1] or coroutine.status(thread) == 'dead' then
      return unpack(result, 1, result.n)
    end
    args = pack(coroutine.yield(unpack(result, 2, result.n)))
  end
end
