#include "linux/linux_standalone.hpp"

#include <algorithm>
#include <cctype>

namespace gt4::linux_start {
namespace {

std::string trim(std::string_view text) {
  std::size_t first = 0, last = text.size();
  while (first < last && std::isspace(static_cast<unsigned char>(text[first])) != 0) ++first;
  while (last > first && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0) --last;
  return std::string(text.substr(first, last - first));
}

std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string value_of(const Access &access, const std::string &name) {
  if (!access.environment) return {};
  return access.environment(name).value_or("");
}

// `~/x` -> $HOME/x; a relative path is taken from the config folder.
std::filesystem::path expand_path(const std::string &value, const Folders &folders) {
  if (value == "~") return folders.home;
  if (value.rfind("~/", 0) == 0) return folders.home / value.substr(2);
  const std::filesystem::path path(value);
  return path.is_absolute() ? path : folders.config_dir / path;
}

// $<variable>/gt4recomp, else $HOME/<fallback>/gt4recomp.
std::optional<std::filesystem::path> xdg_folder(const Access &access, const char *variable, const char *fallback) {
  const std::string xdg = value_of(access, variable);
  if (!xdg.empty() && std::filesystem::path(xdg).is_absolute()) return std::filesystem::path(xdg) / "gt4recomp";
  const std::string home = value_of(access, "HOME");
  if (home.empty()) return std::nullopt;
  return std::filesystem::path(home) / fallback / "gt4recomp";
}

} // namespace

std::optional<Folders> find_folders(const std::filesystem::path &executable_dir, const Access &access) {
  const auto config = xdg_folder(access, "XDG_CONFIG_HOME", ".config");
  const auto data = xdg_folder(access, "XDG_DATA_HOME", ".local/share");
  const auto cache = xdg_folder(access, "XDG_CACHE_HOME", ".cache");
  if (!config || !data || !cache) return std::nullopt;
  Folders folders;
  folders.home = value_of(access, "HOME");
  folders.config_dir = *config;
  folders.data_dir = *data;
  folders.cache_dir = *cache;
  folders.executable_dir = executable_dir;
  return folders;
}

Config parse_config(std::string_view text, const Folders &folders) {
  Config config;
  std::string shared_text;
  std::size_t position = 0;
  while (position <= text.size()) {
    std::size_t end = text.find('\n', position);
    if (end == std::string_view::npos) end = text.size();
    const std::string_view raw = text.substr(position, end - position);
    position = end + 1;
    const std::string line = trim(raw);
    const std::size_t equals = line.find('=');
    if (line.empty() || line[0] == '#' || equals == std::string::npos) {
      shared_text.append(raw).push_back('\n');  // comments, blank lines and malformed lines: the shared parser's rules
      continue;
    }
    const std::string key = lower(trim(std::string_view(line).substr(0, equals)));
    const std::string value = trim(std::string_view(line).substr(equals + 1));
    std::optional<std::filesystem::path> *slot = key == "elf"       ? &config.elf
                                                 : key == "disc"    ? &config.disc
                                                 : key == "memcard" ? &config.memcard
                                                                    : nullptr;
    if (!slot) {
      shared_text.append(raw).push_back('\n');
      continue;
    }
    if (value.empty()) continue;  // the template's empty lines: not chosen yet
    *slot = expand_path(value, folders);
  }
  config.shared = standalone::parse_config(shared_text);
  config.warnings = config.shared.warnings;
  return config;
}

Environment default_environment(const Folders &folders) {
  return {
      {"GT4_RENDERER", "hw"},           // CoreGS 2.0, the Linux renderer
      {"GT4_HW_CORE", "2"},
      {"GT4_HW_SCALE", "3"},            // play.sh's internal resolution
      {"GT4_HW_SHADER_CACHE", (folders.cache_dir / "shaders").string()},
      {"GT4_FORCE_480P", "1"},          // the race display at 480p
      {"GT4_FORCE_WIDESCREEN", "1"},    // 16:9 hold
      {"GT4_DISPLAY_ASPECT", "auto"},   // the display aspect from the window
      {"GT4_PRESENT_MODE", "fifo"},     // vsync
  };
}

std::optional<std::filesystem::path> find_elf(const Folders &folders, const Access &access, const Game &game) {
  for (const std::filesystem::path &candidate :
       {folders.executable_dir / game.elf_name,
        folders.executable_dir.parent_path() / game.elf_folder / game.elf_name}) {
    if (access.is_file && access.is_file(candidate)) return candidate;
  }
  return std::nullopt;
}

Resolved resolve(const Folders &folders, const std::optional<Config> &config, const std::vector<std::string> &user_arguments,
                 const Access &access, const Game &game) {
  Resolved out;
  const std::filesystem::path ini = folders.config_dir / game.config_file;

  // Layers, highest first. A name already set in the process environment, or by a higher layer, is left alone.
  Environment layers;
  if (config) {
    // CoreGS 2.0's keys first: hw_present_mode wins over present_mode (as on Windows).
    const Environment hw = standalone::coregs20_environment(config->shared);
    layers.insert(layers.end(), hw.begin(), hw.end());
    const Environment keys = standalone::config_environment(config->shared);
    layers.insert(layers.end(), keys.begin(), keys.end());
  }
  const std::filesystem::path controls = folders.config_dir / "controls.toml";
  if (access.is_file && access.is_file(controls)) layers.emplace_back("GT4_CONTROLS_FILE", controls.string());
  const Environment defaults = default_environment(folders);
  layers.insert(layers.end(), defaults.begin(), defaults.end());
  for (const auto &[name, value] : layers) {
    if (!value_of(access, name).empty()) continue;
    const bool known = std::any_of(out.set.begin(), out.set.end(), [&](const auto &entry) { return entry.first == name; });
    if (!known) out.set.emplace_back(name, value);
  }

  // The command line play.sh passes, with the user's options replacing their default.
  std::optional<std::filesystem::path> elf = config && config->elf ? config->elf : find_elf(folders, access, game);
  std::optional<std::filesystem::path> disc = config ? config->disc : std::nullopt;
  std::string card = (config && config->memcard ? *config->memcard : folders.data_dir / game.default_card).string();
  std::string region = "--ntsc", window = "--window";
  std::optional<std::string> disc_argument;
  std::vector<std::string> extra;
  for (std::size_t i = 0; i < user_arguments.size(); ++i) {
    const std::string &argument = user_arguments[i];
    const bool has_value = i + 1 < user_arguments.size();
    if (argument == "--disc" && has_value) disc_argument = user_arguments[++i];
    else if (argument == "--memcard" && has_value) card = user_arguments[++i];
    else if (argument == "--ntsc" || argument == "--pal") region = argument;
    else if (argument == "--window" || argument == "--fullscreen") window = argument;
    else if (argument == "--no-window") window.clear();
    else extra.push_back(argument);
  }
  if (disc_argument) disc = std::filesystem::path(*disc_argument);

  if (!elf || !access.is_file || !access.is_file(*elf)) {
    out.problem = elf ? "The game executable named in " + std::string(game.config_file) + " does not exist:\n" + elf->string()
                      : "No game executable (" + std::string(game.elf_name) + ") was found beside the game.";
    out.problem += "\n\nSet elf=<path> in " + ini.string();
    return out;
  }
  if (!disc || !access.is_file(*disc)) {
    out.problem = disc ? "The disc image named in " + std::string(game.config_file) + " does not exist:\n" + disc->string()
                       : std::string("No disc image is chosen yet.");
    out.problem += "\n\nSet disc=<path to your " + std::string(game.display_name) + " disc image> in " + ini.string();
    return out;
  }
  out.arguments = {elf->string(), region, "--disc", disc->string(), "--memcard", card};
  if (!window.empty()) out.arguments.push_back(window);
  out.arguments.insert(out.arguments.end(), extra.begin(), extra.end());
  return out;
}

std::string template_text(const Folders &folders, const std::optional<std::filesystem::path> &elf, const Game &game) {
  std::string text = "# GT4Recomp on Linux, " + std::string(game.display_name) +
      ": the settings of a direct start (start gt4_linux_window without arguments).\n"
      "# key=value lines; '#' starts a comment. A variable set in the environment (GT4_*) or an option on the\n"
      "# command line (--disc, --memcard, --fullscreen, ...) wins over this file. Paths may start with ~/.\n"
      "version=1\n"
      "\n"
      "# The game files. disc must be set before the first start.\n";
  text += "elf=" + (elf ? elf->string() : std::string()) + "\n";
  text += "disc=\n";
  text += "# The memory card; a missing file starts as a new card the game offers to format. Default: " +
          (folders.data_dir / game.default_card).string() + "\n";
  text += "memcard=\n";
  text +=
      "\n"
      "# Display. fullscreen: 0 window, 1 fullscreen (F11 or Alt+Enter switches). output_size: e.g. 2560x1080.\n"
      "fullscreen=0\n"
      "#output_size=2560x1080\n"
      "# present_mode: fifo (vsync), mailbox, immediate.\n"
      "present_mode=fifo\n"
      "\n"
      "# CoreGS 2.0. hw_scale: internal resolution 1..12. hw_postfx: fxaa, or remove the line for none.\n"
      "hw_scale=3\n"
      "hw_postfx=fxaa\n"
      "# hw_hud=1 shows the performance HUD. Frame rate in a race, as the launcher's: hw_frame_skip_adaptive=1 (60 FPS,\n"
      "# frames left out only when the PC falls behind; the launcher's default) or hw_frame_skip=2 (30 FPS); the game keeps\n"
      "# its 60 Hz speed either way. Neither line: every frame drawn, and a slow PC slows the game.\n"
      "#hw_hud=1\n"
      "hw_frame_skip_adaptive=1\n"
      "#hw_frame_skip=2\n"
      "\n"
      "# Game patches (1 on, 0 off): the 360-degree camera on the right stick, the field of view on R3 + D-pad, the\n"
      "# full trigger range.\n"
      "camera_360=1\n"
      "fov_adjust=1\n"
      "trigger_full_range=1\n"
      "\n"
      "# Controllers. wheel_file: a wheel.toml with enabled = true (docs/WHEEL-NATIVE-DFP.md). controls.toml in this\n"
      "# folder is read when it exists.\n"
      "#wheel_file=~/.config/gt4recomp/wheel.toml\n";
  return text;
}

} // namespace gt4::linux_start
