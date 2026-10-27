# Changelog

## [Unreleased]

### Fixed

- Console-only commands now recognize the actual `console` element passed by
  this MTA's CRegisteredCommands, while retaining legacy false/nil invocation
  and rejecting player/other elements. The first real B0 run loaded Lua but
  rejected its own console command because the previous guard rejected every
  element. Both B0 and H1 commands share the corrected guard; vehicle/player
  preconditions remain intact. B0 records the actual command source type.
