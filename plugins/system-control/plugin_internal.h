#pragma once

#include <optional>
#include <string>
#include <vector>

// Parses a "volume" capability payload. "get" -> std::nullopt (query the current level, don't
// set it). "set <0-100>" -> that level. Anything else (bad verb, out-of-range, non-numeric,
// empty) -> std::nullopt, treated by the caller as "not a set" and reported as an error there —
// kept pure and side-effect-free so it's unit-testable without a live audio backend.
std::optional<int> parseVolumeArgument(const std::string& payload);

// Builds the argv for `pactl set-sink-volume @DEFAULT_SINK@ <level>%`. Returned as a vector of
// separate argv entries (never a single shell string) so plugin.cpp can pass it straight to
// execvp — no shell is ever invoked, so there is no command-injection surface here regardless
// of what `level` is.
std::vector<std::string> buildVolumeArgv(int level);

// Builds the argv for `systemctl poweroff`. No arguments — kept as a function (not a literal)
// so shutdown's argv construction lives in the same tested, single place as volume's.
std::vector<std::string> buildShutdownArgv();
