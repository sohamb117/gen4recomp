"""Checked substitutions applied only inside the generated .love source archive."""
def replace_once(text, old, new):
    assert text.count(old) == 1, f'Patch drift: {old!r}'
    return text.replace(old, new, 1)

def patch(path, text):
    if path == 'src/core/Music.lua':
        text = replace_once(text, '  else\n    pcall(src.setLooping, src, wantLoop)\n  end',
                            '  elseif not isChip then\n    pcall(src.setLooping, src, wantLoop)\n  end')
    if path == 'src/core/game3/bg_affine.lua':
        text = replace_once(text, 'local ffi = require("ffi")\n', '')
        text = replace_once(text, 'return tonumber(ffi.new("float", x))',
                            'return (love.data.unpack("f", love.data.pack("string", "f", x)))')
    # Lua 5.1 has no goto. Each target is the tail of the current iteration.
    # Start repeats AFTER terminating breaks, retaining the original loop exit.
    loops = {
        'src/import/gba/map_tree.lua': [
            ('  for gi = 0, numGroups - 1 do', 'continue_group'),
            ('    for mi = 0, #mapNames - 1 do', 'continue_map')],
        'src/import/gba/extract_scripts.lua': [('  while #queue > 0 and processed < BFS_MAX do', 'continue')],
        'src/import/gba/encounters_extract.lua': [('  for _, e in ipairs(entries) do', 'continue')],
        'src/import/gba/extract_map_events.lua': [('    if typ == 0 then break end', 'continue')],
        'src/import/gba/door_anim_extract.lua': [('    if ptr_tiles == 0 then break end', 'continue')],
        'src/import/gba/battle_anim_extract.lua': [('  for idx = 0, anim.tag_count - 1 do', 'continue_tag')],
    }
    for start, label in loops.get(path, []):
        text = replace_once(text, start, start + '\n    repeat')
        text = replace_once(text, '::' + label + '::', 'until true')
        assert 'goto ' + label in text
        text = text.replace('goto ' + label, 'break')
    return text
