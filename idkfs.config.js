// idkfs.config.js — I Don't Know Filesystem config
// place this next to your image file or pass --config <path>

var default = {
  // disable features for max speed (set true to disable)
  disable: {
    journaling:  false,   // set true = no crash recovery, faster writes
    checksums:   false,   // set true = no integrity checks
    timestamps:  false,   // set true = no atime/mtime updates
    compression: true,    // disabled by default (expensive)
    encryption:  true,    // disabled by default
    dedup:       true,    // disabled by default
    prefetch:    false,
    lua_hooks:   false,
    sorting:     false,
  },

  // file patterns → speed tier
  // FAST  = O_DIRECT, no journal, no checksum
  // NORMAL = buffered, journaled
  // SLOW  = compressed, low priority, checksummed
  speed_tier: {
    fast:   ["*.so", "*.so.*", "*.bin", "*.o", "*.exe", "*.dylib"],
    normal: ["*.c", "*.rs", "*.js", "*.lua", "*.md", "*.txt", "*.json"],
    slow:   ["*.iso", "*.tar", "*.tar.*", "*.zip", "*.zst", "*.gz", "*.bz2", "*.img"],
  },

  // sorting algorithm for directory listings (lua will implement these)
  sorting: {
    algorithm: "btree",   // btree | hashmap | radix
  },
}
