-- sorting.lua — Lua comparator for idkfs directory listings
-- Sort by file size first, then by type (dir/file/symlink/device/unknown),
-- and finally by a case-insensitive name order so listings stay deterministic.

local type_order = {
  dir = 0,
  file = 1,
  symlink = 2,
  device = 3,
  unknown = 4,
}

local function normalize_type(name)
  return type_order[name] or type_order.unknown
end

local function compare_entries(a, b)
  if a.size ~= b.size then
    return a.size < b.size and -1 or 1
  end

  local ta = normalize_type(a.type)
  local tb = normalize_type(b.type)
  if ta ~= tb then
    return ta < tb and -1 or 1
  end

  local an = a.name:lower()
  local bn = b.name:lower()
  if an == bn then
    return 0
  end
  return an < bn and -1 or 1
end

function compare(a, b)
  if not a or not b then
    return 0
  end
  return compare_entries(a, b)
end
