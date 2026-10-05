-- Experimental browser-only love.graphics adapter. No upstream files are edited.
local Json = require('src.link.Json')
local g, commands, serial = {}, {}, 0
local function emit(...) commands[#commands+1] = {...} end
local function flush()
  if #commands > 0 then print('POKEWEB_CANVAS:' .. Json.encode(commands)); commands = {} end
end
local function id() serial=serial+1; return serial end
local function copy(t) local r={} for k,v in pairs(t) do r[k]=v end return r end
local state={color={1,1,1,1}, background={0,0,0,1}, transform=love.math.newTransform(), blend={'alpha','alphamultiply'}, width=1}
local stack={}
local width,height=720,480
emit('resize',width,height)
local function matrix(t)
  local a,c,_,e,b,d,_,f=t:getMatrix()
  return {a,b,c,d,e,f}
end
local Object={}
Object.__index=Object
function Object:type() return self.kind end
function Object:typeOf(kind) return kind==self.kind or kind=='Object' or kind=='Drawable' or (kind=='Texture' and (self.kind=='Image' or self.kind=='Canvas')) end
function Object:getDimensions() return self.w,self.h end
function Object:getWidth() return self.w end
function Object:getHeight() return self.h end
Object.getPixelDimensions=Object.getDimensions
Object.getPixelWidth=Object.getWidth
Object.getPixelHeight=Object.getHeight
function Object:getDPIScale() return 1 end
function Object:setFilter(min,mag) self.filter=min; emit('filter',self.id,min,mag or min) end
function Object:getFilter() return self.filter or 'nearest',self.filter or 'nearest',1 end
function Object:setWrap(x,y) self.wrap={x,y or x}; emit('wrap',self.id,x,y or x) end
function Object:getWrap() return unpack(self.wrap or {'clamp','clamp'}) end
function Object:getFormat() return 'rgba8' end
function Object:release() emit('release',self.id); self.released=true end
function Object:isReadable() return true end
function Object:getTextureType() return '2d' end
function Object:getMipmapCount() return 1 end
function Object:getMSAA() return 0 end
function Object:renderTo(fn) local old=g.getCanvas();g.setCanvas(self);fn();g.setCanvas(old) end
local function object(kind,w,h)
  return setmetatable({id=id(),kind=kind,w=w,h=h},Object)
end
local function pixels(image,data)
  assert(data:getFormat()=='rgba8','Canvas adapter requires rgba8 assets')
  local path='/canvas-image-'..id()
  local f=assert(io.open(path,'wb'));f:write(data:getString());f:close()
  emit('image',image.id,image.w,image.h,path)
end
function g.newImage(source)
  local data=type(source)=='string' and love.image.newImageData(source) or source
  local image=object('Image',data:getDimensions());image.data=data;pixels(image,data);return image
end
function Object:replacePixels(data) self.data=data;pixels(self,data) end
function Object:newImageData(...)
  if self.data then return self.data:clone() end
  local args={...};local x,y,w,h=0,0,self.w,self.h
  if #args==6 then x,y,w,h=args[3],args[4],args[5],args[6] end
  local path='/canvas-readback-'..id()
  emit('readback',self.id,x,y,w,h,path);flush()
  local f=assert(io.open(path,'rb'));local raw=f:read('*a');f:close();os.remove(path)
  return love.image.newImageData(w,h,'rgba8',raw)
end
function g.newCanvas(w,h,settings)
  local canvas=object('Canvas',w or width,h or height);emit('canvas',canvas.id,canvas.w,canvas.h);return canvas
end
function g.newQuad(x,y,w,h,sw,sh)
  local q=object('Quad',w,h);q.viewport={x,y,w,h,sw,sh}
  function q:getViewport() return unpack(self.viewport,1,4) end
  function q:setViewport(...) self.viewport={...};self.w=self.viewport[3];self.h=self.viewport[4] end
  function q:getTextureDimensions() return self.viewport[5],self.viewport[6] end
  return q
end
local function drawArgs(...)
  local a={...};local q=false
  if type(a[1])=='table' and a[1].kind=='Quad' then q=a[1].viewport;table.remove(a,1) end
  local t=love.math.newTransform()
  if type(a[1])=='userdata' then t=a[1]:clone()
  else t:setTransformation(a[1] or 0,a[2] or 0,a[3] or 0,a[4] or 1,a[5] or a[4] or 1,a[6] or 0,a[7] or 0,a[8] or 0,a[9] or 0) end
  return q,t
end
function g.draw(image,...)
  local q,t=drawArgs(...);local transform=state.transform:clone();transform:apply(t)
  emit('draw',image.id,q,matrix(transform))
end
function g.newSpriteBatch(image,size,usage)
  local batch=object('SpriteBatch',image.w,image.h);batch.count=0;emit('batch',batch.id,image.id)
  function batch:add(...) self.count=self.count+1;self:set(self.count,...);return self.count end
  function batch:set(i,...) local q,t=drawArgs(...);emit('batchSet',self.id,i,q,matrix(t),self.color or {1,1,1,1}) end
  function batch:setColor(...) local a={...};if type(a[1])=='table' then a=a[1] end self.color={a[1],a[2],a[3],a[4] or 1} end
  function batch:clear() self.count=0;emit('batchClear',self.id) end
  function batch:flush() end
  function batch:getCount() return self.count end
  function batch:getBufferSize() return math.max(size or 1000,self.count) end
  function batch:setBufferSize(n) size=n end
  function batch:setTexture(image) emit('batchTexture',self.id,image.id) end
  function batch:getTexture() return image end
  function batch:setDrawRange(start,count) emit('batchRange',self.id,start or 1,count or false) end
  return batch
end
function g.setColor(...) local a={...};if type(a[1])=='table' then a=a[1] end state.color={a[1] or 1,a[2] or 1,a[3] or 1,a[4] or 1};emit('color',state.color) end
function g.getColor() return unpack(state.color) end
function g.setBackgroundColor(...) state.background={...} end
function g.getBackgroundColor() return unpack(state.background) end
function g.setCanvas(canvas)
  if type(canvas)=='table' and not canvas.kind then canvas=canvas[1] end
  state.canvas=canvas;emit('target',canvas and canvas.id or 0)
end
function g.getCanvas() return state.canvas end
function g.setShader(shader) state.shader=shader;emit('shader',shader and shader.id or 0) end
function g.getShader() return state.shader end
function g.newShader(source)
  -- Only implemented pixel effects may be advertised as supported.
  local kind
  if source:find('extern vec3 c0',1,true) then kind='palette'
  elseif source:find('extern vec3 pal0',1,true) then kind='gbcPalette'
  elseif source:find('extern Image idxTex;',1,true) then kind='gbaBG'
  elseif source:find('extern vec2 frameOrigin;',1,true) then kind='gbaOBJ'
  elseif source:find('extern vec4 bgPrio;',1,true) then kind='gbaCompose'
  elseif source:find('p.a = 0.0',1,true) and source:find('p.r > 0.83',1,true) then kind='colorKey'
  elseif require('canvas_shaders')[source:gsub('/%*.-%*/',''):gsub('//[^\n]*',''):gsub('%s+','')] then kind='generic'
  else error('Canvas renderer: unsupported shader',2) end
  local shader=object('Shader');shader.source=source
  emit('newShader',shader.id,kind,source:find('0.0 : p.a',1,true)~=nil or source:find('shade < 0.5',1,true)~=nil,source)
  local function snapshot(v)
    if type(v)~='table' then return v end
    if v.kind then return {texture=v.id} end
    local result={} for k,value in pairs(v) do result[k]=snapshot(value) end return result
  end
  function shader:send(name,...) emit('uniform',self.id,name,snapshot({...})) end
  shader.sendColor=shader.send
  function shader:hasUniform(name) return self.source:find(name,1,true)~=nil end
  function shader:getWarnings() return '' end
  return shader
end
function g.validateShader() return false,'Generic GLSL is not supported by the Canvas renderer' end
function g.setBlendMode(mode,alpha) state.blend={mode or 'alpha',alpha or 'alphamultiply'};emit('blend',unpack(state.blend)) end
function g.getBlendMode() return unpack(state.blend) end
function g.setScissor(x,y,w,h) state.scissor=x and {x,y,w,h} or nil;emit('scissor',state.scissor or false) end
function g.getScissor() if state.scissor then return unpack(state.scissor) end end
function g.intersectScissor(x,y,w,h)
  if not state.scissor then return g.setScissor(x,y,w,h) end
  local a,b,c,d=unpack(state.scissor);local x2,y2=math.min(x+w,a+c),math.min(y+h,b+d)
  x,y=math.max(x,a),math.max(y,b);g.setScissor(x,y,math.max(0,x2-x),math.max(0,y2-y))
end
function g.push(mode)
  local s=copy(state);s.transform=state.transform:clone();s.all=mode=='all';stack[#stack+1]=s
end
function g.pop()
  local s=assert(table.remove(stack),'Unbalanced graphics.pop')
  if s.all then state=s;g.setColor(s.color);g.setCanvas(s.canvas);g.setShader(s.shader);g.setBlendMode(unpack(s.blend));g.setScissor(unpack(s.scissor or {}));g.setLineWidth(s.width) else state.transform=s.transform end
end
function g.origin() state.transform=love.math.newTransform() end
function g.translate(x,y) state.transform:translate(x,y) end
function g.scale(x,y) state.transform:scale(x,y or x) end
function g.rotate(a) state.transform:rotate(a) end
function g.shear(x,y) state.transform:shear(x,y) end
function g.applyTransform(t) state.transform:apply(t) end
function g.replaceTransform(t) state.transform=t:clone() end
function g.transformPoint(x,y) return state.transform:transformPoint(x,y) end
function g.inverseTransformPoint(x,y) return state.transform:inverseTransformPoint(x,y) end
function g.clear(...)
  local a={...};if type(a[1])=='table' then a=a[1] end
  emit('clear',{a[1] or 0,a[2] or 0,a[3] or 0,a[4] or 0})
end
function g.setLineWidth(w) state.width=w;emit('lineWidth',w) end
function g.getLineWidth() return state.width end
for _,name in ipairs({'rectangle','circle','ellipse','polygon','line','arc','points'}) do
  g[name]=function(...) emit('shape',name,{...},matrix(state.transform)) end
end
function g.setDefaultFilter() end
function g.getDefaultFilter() return 'nearest','nearest',1 end
function g.getDimensions() return width,height end
g.getPixelDimensions=g.getDimensions
function g.getWidth() return width end
function g.getHeight() return height end
function g.getDPIScale() return 1 end
function g.getRendererInfo() return 'Canvas 2D','experimental','browser','software-compatible' end
function g.getSupported() return {canvas=true,shader=false} end
function g.isActive() return true end
function g.isCreated() return true end
function g.present() emit('present');flush() end
function g.flushBatch() flush() end
function g.newFont() error('Canvas renderer: native vector fonts are not implemented',2) end
love.window={
 setMode=function(w,h) width,height=w,h;emit('resize',w,h);return true end,
 getMode=function() return width,height,{fullscreen=false,vsync=1,highdpi=false} end,
 getDPIScale=function() return 1 end, getSafeArea=function() return 0,0,width,height end,
 setVSync=function() end, getVSync=function() return 1 end, hasFocus=function() return true end,
 setFullscreen=function() return false end,
}
love.graphics=g
return g
