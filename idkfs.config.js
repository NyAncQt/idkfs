


var config = {

  disable: {
    journaling:  false,
    checksums:   false,
    timestamps:  false,
    compression: true,
    encryption:  true,
    dedup:       true,
    prefetch:    false,
    lua_hooks:   false,
    sorting:     false,
  },





  speed_tier: {
    fast:   ["*.so", "*.so.*", "*.bin", "*.o", "*.exe", "*.dylib"],
    normal: ["*.c", "*.rs", "*.js", "*.lua", "*.md", "*.txt", "*.json"],
    slow:   ["*.iso", "*.tar", "*.tar.*", "*.zip", "*.zst", "*.gz", "*.bz2", "*.img"],
  },


  sorting: {
    algorithm: "btree",
  },
}
