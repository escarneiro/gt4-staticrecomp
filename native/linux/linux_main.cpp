// TASK-074 (Linux): the game's main. The same steps as the non-standalone path of native/game_main.cpp, which stays the
// Windows main: the patches that read their switches once (TASK-082 frame skip, TASK-159 360-degree camera, TASK-160
// field of view, TASK-161 full trigger range) are configured before the game starts, so an invalid value stops the start
// as it does on Windows, then the donor's main (the port's main.cpp, compiled as gt4_donor_main) runs the game.
// A start without an ELF argument is a direct start from game.ini (native/linux/linux_standalone.hpp): the file gives the
// paths and settings play.sh would pass. A start with an ELF argument runs exactly as before. Not here yet: the Windows
// install layout (receipt checks, launcher lock, session folders), which belongs with a Linux launcher (TASK-074 step L4).

#include "camera_360.hpp"
#include "controls.hpp"
#include "fov_adjust.hpp"
#include "frame_skip.hpp"
#include "linux/linux_standalone.hpp"
#include "trigger_range.hpp"
#include "window.hpp"

#ifdef GT4_GS_STREAM_CLIENT
#include "gs_stream/gs_stream_client.hpp"
#include "ps2/memory.hpp"
#endif

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <optional>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

int gt4_donor_main(int argc, char **argv);

namespace {

// TASK-161: one switch drives both sides of the full trigger range (as game_main.cpp configure_trigger_range).
void configure_trigger_range() {
  gt4::trigger_range::configure_from_environment();
  gt4::controls::set_pressure_scale(gt4::trigger_range::enabled() ? gt4::controls::PressureScale::full
                                                                  : gt4::controls::PressureScale::stock);
}

// A direct start that cannot go on: the reason on stderr, and in a message box when the game would have opened a window.
int refuse_start(const std::string &text, bool window) {
  std::fprintf(stderr, "[start] %s\n", text.c_str());
  if (window) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "GT4Recomp", text.c_str(), nullptr);
  return 3;
}

// The direct start (TASK-074): fills `arguments` (argv for the donor main, program name first) and sets the environment
// from game.ini. Writes the template game.ini when none exists. Returns 0, or the exit code of a refused start.
int prepare_direct_start(int argc, char **argv, std::vector<std::string> &arguments) {
  namespace start = gt4::linux_start;
  start::Access access;
  access.environment = [](const std::string &name) -> std::optional<std::string> {
    const char *value = std::getenv(name.c_str());
    if (!value || !*value) return std::nullopt;
    return std::string(value);
  };
  access.is_file = [](const std::filesystem::path &path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
  };
  std::vector<std::string> user(argv + 1, argv + argc);
  const bool window = std::find(user.begin(), user.end(), "--no-window") == user.end();
  std::error_code error;
  const std::filesystem::path executable = std::filesystem::read_symlink("/proc/self/exe", error);
  const auto folders = start::find_folders(executable.parent_path(), access);
  if (!folders)
    return refuse_start("Neither HOME nor XDG_CONFIG_HOME is set: the game cannot find " + std::string(start::kGame.config_file) +
                        ".", window);

  const std::filesystem::path ini = folders->config_dir / start::kGame.config_file;
  std::optional<start::Config> config;
  if (std::ifstream in{ini}) {
    std::stringstream text;
    text << in.rdbuf();
    config = start::parse_config(text.str(), *folders);
    for (const std::string &warning : config->warnings) std::fprintf(stderr, "[start] %s\n", warning.c_str());
  } else {
    std::filesystem::create_directories(folders->config_dir, error);
    std::ofstream out{ini};
    out << start::template_text(*folders, start::find_elf(*folders, access));
    if (!out) return refuse_start("Cannot write " + ini.string(), window);
    std::fprintf(stderr, "[start] wrote %s\n", ini.string().c_str());
  }

  const start::Resolved resolved = start::resolve(*folders, config, user, access);
  if (!resolved.problem.empty()) return refuse_start(resolved.problem, window);
  std::filesystem::create_directories(folders->data_dir, error);  // the default card's folder
  for (const auto &[name, value] : resolved.set) {
    if (name == "GT4_HW_SHADER_CACHE") std::filesystem::create_directories(value, error);
    setenv(name.c_str(), value.c_str(), 0);
  }
  std::fprintf(stderr, "[start] direct start from %s:", ini.string().c_str());
  for (const std::string &argument : resolved.arguments) std::fprintf(stderr, " %s", argument.c_str());
  std::fprintf(stderr, "\n");
  arguments = {argv[0]};
  arguments.insert(arguments.end(), resolved.arguments.begin(), resolved.arguments.end());
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  // TASK-074: a start without an ELF argument takes its command line and settings from game.ini.
  std::vector<std::string> direct_arguments;
  std::vector<char *> direct_argv;
  if (gt4::standalone::is_standalone_start(argc, argv)) {
    if (const int refused = prepare_direct_start(argc, argv, direct_arguments)) return refused;
    for (std::string &argument : direct_arguments) direct_argv.push_back(argument.data());
    direct_argv.push_back(nullptr);
    argc = static_cast<int>(direct_arguments.size());
    argv = direct_argv.data();
  }
#ifdef GT4_GS_STREAM_CLIENT
  // TASK-116 step 0: the guest RAM window of the GT4_GS_READBACK_LOG lines (nothing happens when the switch is unset).
  gt4::gs_stream::set_guest_ram(ps2::g_ram, ps2::kRamSize);
#endif
  bool window = false;
  for (int i = 1; i < argc; ++i)
    window = window || std::strcmp(argv[i], "--window") == 0 || std::strcmp(argv[i], "--fullscreen") == 0;
  gt4::frame_skip::start_from_environment();
  gt4::camera360::configure_from_environment();  // an invalid GT4_CAMERA_360 stops the start here
  gt4::fov_adjust::configure_from_environment();  // an invalid GT4_FOV_ADJUST stops the start here
  configure_trigger_range();                       // an invalid GT4_TRIGGER_FULL_RANGE stops the start here
  if (!window && !install_input_replay_without_window()) return 1;
  return gt4_donor_main(argc, argv);
}
