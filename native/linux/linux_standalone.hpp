#pragma once

// TASK-074 (Linux): the game starts on its own, from game.ini, without environment variables or scripts.
//
// The Linux counterpart of native/standalone.hpp (TASK-097). A start without an ELF argument (no arguments, or only `--`
// options: gt4::standalone::is_standalone_start) reads `game.ini` from the user's config folder
// ($XDG_CONFIG_HOME/gt4recomp, else ~/.config/gt4recomp) and builds the command line and environment that
// scripts/linux/play.sh passes. A start with an ELF argument (play.sh, the benches, the pins) never reaches this code.
//
// The keys are the Windows launcher's (gt4::standalone::parse_config, so the names, values and refusals are the same),
// plus three Linux-only path keys the Windows install layout finds by itself:
//   elf=<reconstructed SCUS_973.28 ELF>   disc=<disc image>   memcard=<memory card file>
// A path may start with `~/`; a relative path is taken from the config folder.
//
// Order of every value: explicit argument or environment variable > game.ini > built-in default. The built-in defaults
// are play.sh's fixed ones (480p, 16:9 hold, automatic display aspect, vsync, CoreGS 2.0 at 3x with a shader cache);
// keys whose Windows meaning is "absent = off" (hw_postfx, trigger_full_range, ...) stay so, and the template written on
// the first start sets play.sh's choices for them explicitly.
//
// Pure logic over injected access, checked by the native_linux_standalone fixture; native/linux/linux_main.cpp supplies
// the real file system and environment.

#include "standalone.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gt4::linux_start {

using standalone::Environment;

struct Access {
  std::function<std::optional<std::string>(const std::string &)> environment;  // empty value counts as unset
  std::function<bool(const std::filesystem::path &)> is_file;
};

struct Folders {
  std::filesystem::path home;            // $HOME
  std::filesystem::path config_dir;      // $XDG_CONFIG_HOME/gt4recomp or ~/.config/gt4recomp: game.ini, controls.toml
  std::filesystem::path data_dir;        // $XDG_DATA_HOME/gt4recomp or ~/.local/share/gt4recomp: the default card
  std::filesystem::path cache_dir;       // $XDG_CACHE_HOME/gt4recomp or ~/.cache/gt4recomp: the shader cache
  std::filesystem::path executable_dir;  // the folder of gt4_linux_window
};

// None when neither HOME nor the XDG variables give a folder.
std::optional<Folders> find_folders(const std::filesystem::path &executable_dir, const Access &access);

struct Config {
  standalone::Config shared;            // the Windows keys
  std::optional<std::filesystem::path> elf, disc, memcard;
  std::vector<std::string> warnings;    // shared.warnings plus the path keys' own
};

// `key=value` lines and `#` comments, as on Windows; the path keys are taken out before the shared parser sees the rest.
Config parse_config(std::string_view text, const Folders &folders);

// The defaults of a Linux start (play.sh's fixed settings).
Environment default_environment(const Folders &folders);

// The ELF when game.ini names none: SCUS_973.28.elf beside the executable, then in ../linux074-ee-producer (the build
// folders of this checkout live side by side in private/).
std::optional<std::filesystem::path> find_elf(const Folders &folders, const Access &access);

struct Resolved {
  Environment set;                     // variables to put in the process environment (only those that were unset)
  std::vector<std::string> arguments;  // argv[1..] for the donor main: ELF first
  std::string problem;                 // non-empty: the start cannot go on (what is missing and what to do)
};

// Builds the environment (precedence above) and the command line
//   <elf> --ntsc --disc <iso> --memcard <card> --window
// User options replace their default (--disc, --memcard, --ntsc/--pal, --window/--fullscreen), `--no-window` leaves the
// window flag out (headless tests), and every other option is appended unchanged.
Resolved resolve(const Folders &folders, const std::optional<Config> &config, const std::vector<std::string> &user_arguments,
                 const Access &access);

// The commented game.ini written on the first start when none exists: every key with play.sh's choices, the ELF found
// by find_elf (else an empty value) and empty disc and memcard lines to fill in.
std::string template_text(const Folders &folders, const std::optional<std::filesystem::path> &elf);

} // namespace gt4::linux_start
