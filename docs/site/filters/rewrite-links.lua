local function rewrite_target(target)
  if target:match('^#') or target:match('^[a-zA-Z][a-zA-Z0-9+.-]*:') or target:match('^//') then
    return target
  end

  local path, anchor = target:match('^(.-)#(.*)$')
  if not path then
    path = target
    anchor = nil
  end

  if path:match('README%.md$') then
    path = path:gsub('README%.md$', 'index.html')
  elseif path:match('%.md$') then
    path = path:gsub('%.md$', '.html')
  end

  if anchor and anchor ~= '' then
    return path .. '#' .. anchor
  end
  return path
end

function Link(el)
  el.target = rewrite_target(el.target)
  return el
end
