// TASK-074: tests of the Linux direct start rules (native/linux/linux_standalone.cpp) over a fake file system and
// environment.

#include "linux/linux_standalone.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <set>
#include <string>

namespace start = gt4::linux_start;
namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char *what) {
  if (!condition) {
    std::fprintf(stderr, "linux_standalone_fixture: FAILED %s\n", what);
    ++failures;
  }
}

struct Fake {
  std::map<std::string, std::string> environment;
  std::set<std::string> files;
  start::Access access() const {
    start::Access a;
    a.environment = [this](const std::string &name) -> std::optional<std::string> {
      const auto it = environment.find(name);
      if (it == environment.end() || it->second.empty()) return std::nullopt;
      return it->second;
    };
    a.is_file = [this](const fs::path &path) { return files.count(path.lexically_normal().string()) != 0; };
    return a;
  }
};

std::string value(const start::Environment &environment, const std::string &name) {
  for (const auto &[key, text] : environment)
    if (key == name) return text;
  return "<unset>";
}

bool has_argument(const std::vector<std::string> &arguments, const std::string &argument) {
  return std::find(arguments.begin(), arguments.end(), argument) != arguments.end();
}

std::string after(const std::vector<std::string> &arguments, const std::string &option) {
  const auto it = std::find(arguments.begin(), arguments.end(), option);
  return it == arguments.end() || it + 1 == arguments.end() ? "<none>" : *(it + 1);
}

} // namespace

int main() {
  Fake fake;
  fake.environment = {{"HOME", "/home/player"}};
  const fs::path exe_dir = "/games/gt4/private/linux074-pgo-build";
  const fs::path producer_elf = "/games/gt4/private/linux074-ee-producer/SCUS_973.28.elf";
  fake.files = {producer_elf.string(), "/discs/GT4.iso", "/home/player/discs/other.iso"};

  // Folders: XDG fallbacks under HOME, and an absolute XDG variable wins.
  auto folders = start::find_folders(exe_dir, fake.access());
  check(folders.has_value(), "folders from HOME");
  check(folders->config_dir == "/home/player/.config/gt4recomp", "config folder from HOME");
  check(folders->data_dir == "/home/player/.local/share/gt4recomp", "data folder from HOME");
  check(folders->cache_dir == "/home/player/.cache/gt4recomp", "cache folder from HOME");
  {
    Fake xdg = fake;
    xdg.environment["XDG_CONFIG_HOME"] = "/cfg";
    xdg.environment["XDG_CACHE_HOME"] = "relative/ignored";
    const auto f = start::find_folders(exe_dir, xdg.access());
    check(f->config_dir == "/cfg/gt4recomp", "XDG_CONFIG_HOME wins");
    check(f->cache_dir == "/home/player/.cache/gt4recomp", "a relative XDG variable is ignored");
    Fake nobody;
    check(!start::find_folders(exe_dir, nobody.access()), "no HOME and no XDG: no folders");
  }

  // The ELF beside the build folders is found without game.ini.
  check(start::find_elf(*folders, fake.access(), start::kV200Game) == producer_elf, "ELF found in ../linux074-ee-producer");

  // No game.ini: defaults, but no disc chosen.
  {
    const auto r = start::resolve(*folders, std::nullopt, {}, fake.access(), start::kV200Game);
    check(!r.problem.empty() && r.problem.find("disc=") != std::string::npos, "no disc: the start is refused, naming disc=");
    check(r.arguments.empty(), "a refused start has no command line");
  }

  // The template parses with no warnings, and once a disc is set it starts with play.sh's settings.
  {
    std::string text = start::template_text(*folders, producer_elf, start::kV200Game);
    check(text.find("elf=" + producer_elf.string() + "\n") != std::string::npos, "template names the found ELF");
    auto config = start::parse_config(text, *folders);
    check(config.warnings.empty(), "template: no warnings");
    check(!config.disc && !config.memcard, "template: empty disc and memcard lines are not chosen");
    const std::string pos = "disc=\n";
    text.replace(text.find(pos), pos.size(), "disc=/discs/GT4.iso\n");
    config = start::parse_config(text, *folders);
    const auto r = start::resolve(*folders, config, {}, fake.access(), start::kV200Game);
    check(r.problem.empty(), "template with a disc: starts");
    check(r.arguments.size() >= 6 && r.arguments[0] == producer_elf.string(), "ELF first");
    check(after(r.arguments, "--disc") == "/discs/GT4.iso", "disc from game.ini");
    check(after(r.arguments, "--memcard") == "/home/player/.local/share/gt4recomp/card.ps2", "default card in the data folder");
    check(has_argument(r.arguments, "--ntsc") && has_argument(r.arguments, "--window"), "--ntsc --window");
    check(value(r.set, "GT4_RENDERER") == "hw" && value(r.set, "GT4_HW_CORE") == "2", "CoreGS 2.0");
    check(value(r.set, "GT4_HW_SCALE") == "3", "hw_scale=3");
    check(value(r.set, "GT4_HW_POSTFX") == "fxaa", "hw_postfx=fxaa");
    check(value(r.set, "GT4_FORCE_480P") == "1" && value(r.set, "GT4_FORCE_WIDESCREEN") == "1", "480p and 16:9 hold");
    check(value(r.set, "GT4_DISPLAY_ASPECT") == "auto", "display aspect auto");
    check(value(r.set, "GT4_PRESENT_MODE") == "fifo", "vsync");
    check(value(r.set, "GT4_FULLSCREEN") == "0", "windowed");
    check(value(r.set, "GT4_TRIGGER_FULL_RANGE") == "1", "full trigger range");
    check(value(r.set, "GT4_CAMERA_360") == "1" && value(r.set, "GT4_FOV_ADJUST") == "1", "camera and FoV patches");
    check(value(r.set, "GT4_HW_SHADER_CACHE") == "/home/player/.cache/gt4recomp/shaders", "shader cache in the cache folder");
    check(value(r.set, "GT4_CONTROLS_FILE") == "<unset>", "no controls.toml: no variable");
  }

  // Paths: ~/ and relative to the config folder; unknown keys and refused values warn but never stop the start.
  {
    const auto config = start::parse_config("disc=~/discs/other.iso\nmemcard=cards/main.ps2\nhw_scale=99\ncolour=red\n", *folders);
    check(config.disc == fs::path("/home/player/discs/other.iso"), "~/ expands to HOME");
    check(config.memcard == fs::path("/home/player/.config/gt4recomp/cards/main.ps2"), "a relative path is from the config folder");
    check(config.warnings.size() == 2, "refused hw_scale and unknown key: two warnings");
    const auto r = start::resolve(*folders, config, {}, fake.access(), start::kV200Game);
    check(r.problem.empty(), "starts despite the warnings");
    check(value(r.set, "GT4_HW_SCALE") == "3", "a refused hw_scale falls back to the default");
    check(value(r.set, "GT4_HW_POSTFX") == "<unset>", "hw_postfx absent: no FXAA (the Windows meaning)");
  }

  // Precedence: environment > game.ini > default; command line > game.ini.
  {
    Fake env = fake;
    env.environment["GT4_HW_SCALE"] = "6";
    env.environment["GT4_PRESENT_MODE"] = "mailbox";
    env.files.insert("/home/player/.config/gt4recomp/controls.toml");
    const auto config = start::parse_config("disc=/discs/GT4.iso\nhw_scale=4\npresent_mode=immediate\nhw_present_mode=fifo\n", *folders);
    auto r = start::resolve(*folders, config, {}, env.access(), start::kV200Game);
    check(value(r.set, "GT4_HW_SCALE") == "<unset>", "an environment variable is left alone");
    check(value(r.set, "GT4_PRESENT_MODE") == "<unset>", "the environment wins over both present-mode keys");
    check(value(r.set, "GT4_CONTROLS_FILE") == "/home/player/.config/gt4recomp/controls.toml", "controls.toml in the config folder");
    r = start::resolve(*folders, config, {}, fake.access(), start::kV200Game);
    check(value(r.set, "GT4_PRESENT_MODE") == "fifo", "hw_present_mode wins over present_mode (as on Windows)");
    check(value(r.set, "GT4_HW_SCALE") == "4", "game.ini wins over the default");
    r = start::resolve(*folders, config,
                       {"--disc", "/home/player/discs/other.iso", "--memcard", "/tmp/c.ps2", "--fullscreen", "--frames", "10"},
                       fake.access(), start::kV200Game);
    check(after(r.arguments, "--disc") == "/home/player/discs/other.iso", "--disc replaces game.ini's");
    check(after(r.arguments, "--memcard") == "/tmp/c.ps2", "--memcard replaces the default");
    check(has_argument(r.arguments, "--fullscreen") && !has_argument(r.arguments, "--window"), "--fullscreen replaces --window");
    check(after(r.arguments, "--frames") == "10", "other options are passed on");
    r = start::resolve(*folders, config, {"--no-window"}, fake.access(), start::kV200Game);
    check(!has_argument(r.arguments, "--window") && !has_argument(r.arguments, "--no-window"), "--no-window: headless");
  }

  // Missing files are refused with what to set.
  {
    auto config = start::parse_config("disc=/discs/missing.iso\n", *folders);
    auto r = start::resolve(*folders, config, {}, fake.access(), start::kV200Game);
    check(r.problem.find("/discs/missing.iso") != std::string::npos, "a missing disc is named");
    config = start::parse_config("elf=/nowhere/SCUS_973.28.elf\ndisc=/discs/GT4.iso\n", *folders);
    r = start::resolve(*folders, config, {}, fake.access(), start::kV200Game);
    check(r.problem.find("elf=") != std::string::npos, "a missing ELF names elf=");
  }

  // Spec II (TASK-100): its own config file, ELF, ELF folder and default card; the same keys and precedence.
  {
    const start::Game &spec2 = start::kSpec2Game;
    const fs::path spec2_elf = "/games/gt4/private/linux074-spec2-elf/SCUS_974.36";
    Fake both = fake;
    both.files.insert(spec2_elf.string());
    check(std::string(spec2.config_file) == "game-spec2.ini", "Spec II reads game-spec2.ini");
    check(start::find_elf(*folders, both.access(), spec2) == spec2_elf, "Spec II ELF found in ../linux074-spec2-elf");
    check(start::find_elf(*folders, both.access(), start::kV200Game) == producer_elf, "v2.00 still finds its own ELF");
    std::string text = start::template_text(*folders, spec2_elf, spec2);
    check(text.find("Spec II") != std::string::npos && text.find("elf=" + spec2_elf.string() + "\n") != std::string::npos,
          "Spec II template names the game and its ELF");
    check(start::parse_config(text, *folders).warnings.empty(), "Spec II template: no warnings");
    auto r = start::resolve(*folders, std::nullopt, {}, both.access(), spec2);
    check(r.problem.find("game-spec2.ini") != std::string::npos && r.problem.find("Spec II") != std::string::npos,
          "Spec II: a refused start names game-spec2.ini");
    const auto config = start::parse_config("disc=/discs/GT4.iso\n", *folders);
    r = start::resolve(*folders, config, {}, both.access(), spec2);
    check(r.problem.empty() && r.arguments[0] == spec2_elf.string(), "Spec II starts with its ELF");
    check(after(r.arguments, "--memcard") == "/home/player/.local/share/gt4recomp/card-spec2.ps2", "Spec II default card");
#ifdef GT4_TARGET_SPEC2
    check(&start::kGame == &start::kSpec2Game, "a Spec II build uses the Spec II table");
#else
    check(&start::kGame == &start::kV200Game, "a v2.00 build uses the v2.00 table");
#endif
  }

  if (failures != 0) return 1;
  std::printf("linux_standalone_fixture: all checks passed\n");
  return 0;
}
