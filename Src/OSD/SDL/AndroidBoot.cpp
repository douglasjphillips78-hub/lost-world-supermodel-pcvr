#ifdef __ANDROID__

#include "AndroidBoot.h"

#include <SDL.h>
#include <android/log.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <fstream>
#include <thread>

namespace
{
// ROM sets the app knows, in the order it prefers them when several are present. Each is the zip name
// (without .zip) and the game's name in Config/Games.xml. Clone sets such as swtrilgya also need their
// parent set (swtrilgy.zip) in the same folder.
struct KnownGame { const char* zip; const char* title; };
const KnownGame kGames[] = {
  {"lostwsga", "The Lost World: Jurassic Park"},
  {"swtrilgy", "Star Wars Trilogy Arcade"},
  {"swtrilgya", "Star Wars Trilogy Arcade (Export)"},
  {"swtrilgyp", "Star Wars Trilogy Arcade (Location Test)"},
  {"lostwsgp", "The Lost World: Jurassic Park (Location Test)"},
};
const char* kTag = "SupermodelVR";

// Supermodel prints to stdout/stderr, which Android discards. Forward both to logcat
// (adb logcat -s SupermodelVR) so a headset session can be debugged.
void ForwardStdioToLogcat()
{
  static int fds[2];
  if (pipe(fds) != 0)
    return;
  setvbuf(stdout, nullptr, _IOLBF, 0);
  setvbuf(stderr, nullptr, _IONBF, 0);
  dup2(fds[1], STDOUT_FILENO);
  dup2(fds[1], STDERR_FILENO);
  std::thread([] {
    std::string line;
    char buf[1024];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0) {
      for (ssize_t i = 0; i < n; i++) {
        if (buf[i] == '\n') {
          __android_log_write(ANDROID_LOG_INFO, kTag, line.c_str());
          line.clear();
        } else {
          line += buf[i];
        }
      }
    }
  }).detach();
}

bool Exists(const std::string& path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

long FileSize(const std::string& path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0 ? (long)st.st_size : -1;
}

void MakeDirs(const std::string& path)
{
  // mkdir -p
  for (size_t i = 1; i <= path.size(); i++) {
    if (i == path.size() || path[i] == '/')
      mkdir(path.substr(0, i).c_str(), 0775);
  }
}

// Reads a whole file out of the APK's assets/ folder.
bool ReadAsset(const std::string& name, std::string& out)
{
  SDL_RWops* rw = SDL_RWFromFile(name.c_str(), "rb");
  if (!rw)
    return false;
  const Sint64 size = SDL_RWsize(rw);
  out.clear();
  if (size > 0) {
    out.resize((size_t)size);
    if (SDL_RWread(rw, &out[0], 1, (size_t)size) != (size_t)size) {
      SDL_RWclose(rw);
      return false;
    }
  }
  SDL_RWclose(rw);
  return true;
}

bool WriteFile(const std::string& path, const std::string& data)
{
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f)
    return false;
  f.write(data.data(), (std::streamsize)data.size());
  return (bool)f;
}

// Files whose bundled copy is authoritative: refreshed whenever the size differs.
bool AlwaysRefresh(const std::string& rel)
{
  return rel == "Config/Games.xml" || rel == "Config/Music.xml";
}

// "IniVersion = N" in a settings file (0 if there is none)
int IniVersion(const std::string& text)
{
  const size_t at = text.find("IniVersion");
  if (at == std::string::npos)
    return 0;
  const size_t eq = text.find('=', at);
  return eq == std::string::npos ? 0 : atoi(text.c_str() + eq + 1);
}

// A newer bundled settings preset replaces the installed Config/Supermodel.ini once (the old one is
// kept as Supermodel.ini.bak). Returns true if the bundled file should be written.
bool PresetIsNewer(const std::string& bundled)
{
  const int wanted = IniVersion(bundled);
  if (wanted <= 0)
    return false;
  std::ifstream f("Config/Supermodel.ini", std::ios::binary);
  std::string have((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  const int installed = IniVersion(have);
  if (installed >= wanted)
    return false;
  // keep the old file: the first time as .bak, after an earlier preset as .v<N>.bak (so nothing is lost)
  const std::string bak = installed == 0 ? "Config/Supermodel.ini.bak" : "Config/Supermodel.ini.v" + std::to_string(installed) + ".bak";
  std::remove(bak.c_str());
  std::rename("Config/Supermodel.ini", bak.c_str());
  __android_log_print(ANDROID_LOG_INFO, kTag, "settings preset %d installed (old file kept as Supermodel.ini.bak)", wanted);
  return true;
}

void CopyBundledFiles()
{
  // assets/manifest.txt lists one relative path per line, written by the Gradle build.
  std::string manifest;
  if (!ReadAsset("manifest.txt", manifest)) {
    __android_log_print(ANDROID_LOG_WARN, kTag, "no manifest.txt in the APK assets");
    return;
  }
  size_t pos = 0;
  while (pos < manifest.size()) {
    size_t eol = manifest.find('\n', pos);
    if (eol == std::string::npos)
      eol = manifest.size();
    std::string rel = manifest.substr(pos, eol - pos);
    pos = eol + 1;
    while (!rel.empty() && (rel.back() == '\r' || rel.back() == ' '))
      rel.pop_back();
    if (rel.empty())
      continue;

    std::string data;
    if (!ReadAsset(rel, data)) {
      __android_log_print(ANDROID_LOG_WARN, kTag, "bundled file missing: %s", rel.c_str());
      continue;
    }
    bool have = Exists(rel);
    if (have && rel == "Config/Supermodel.ini" && PresetIsNewer(data))
      have = false;
    if (have && !(AlwaysRefresh(rel) && FileSize(rel) != (long)data.size()))
      continue;
    const size_t slash = rel.rfind('/');
    if (slash != std::string::npos)
      MakeDirs(rel.substr(0, slash));
    if (!WriteFile(rel, data))
      __android_log_print(ANDROID_LOG_ERROR, kTag, "could not write %s", rel.c_str());
  }
}
}  // namespace

namespace AndroidBoot
{
bool Prepare(std::vector<std::string>& args)
{
  ForwardStdioToLogcat();
  const char* ext = SDL_AndroidGetExternalStoragePath();
  if (!ext) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Supermodel VR", "The app's storage folder is not available.", nullptr);
    return false;
  }
  if (chdir(ext) != 0) {
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Supermodel VR", "Cannot open the app's storage folder.", nullptr);
    return false;
  }
  for (const char* d : {"Config", "ROMs", "NVRAM", "Saves", "Assets", "Log"})
    MakeDirs(d);

  CopyBundledFiles();

  // Which game to start: the one named in Config/StartGame.txt (a ROM name such as swtrilgy) if that
  // ROM is there; otherwise the first known ROM set found in ROMs/. With one game installed there
  // is nothing to choose.
  std::string wanted;
  {
    std::ifstream f("Config/StartGame.txt");
    if (f >> wanted) {
      while (!wanted.empty() && (wanted.back() == '\r' || wanted.back() == '\n'))
        wanted.pop_back();
      if (wanted.size() > 4 && wanted.compare(wanted.size() - 4, 4, ".zip") == 0)
        wanted.resize(wanted.size() - 4);
    }
  }
  std::string rom, title;
  if (!wanted.empty() && Exists("ROMs/" + wanted + ".zip")) {
    rom = "ROMs/" + wanted + ".zip";
    title = wanted;
    for (const auto& g : kGames)
      if (wanted == g.zip)
        title = g.title;
  } else {
    for (const auto& g : kGames) {
      if (Exists(std::string("ROMs/") + g.zip + ".zip")) {
        rom = std::string("ROMs/") + g.zip + ".zip";
        title = g.title;
        break;
      }
    }
  }
  if (rom.empty()) {
    std::string names;
    for (const auto& g : kGames)
      names += std::string("  ") + g.zip + ".zip  (" + g.title + ")\n";
    WriteFile("ROMs/PUT-ROMS-HERE.txt", "Put your own ROM sets in this folder, then start the app again.\nSupported:\n" + names +
              "\nWith several installed, the first is started. To pick another, put its name (for example swtrilgy)\nin Config/StartGame.txt.\n");
    const std::string msg = std::string("No ROM found.\n\nCopy your own ROM set to:\n") + ext + "/ROMs/\n\nSupported:\n" + names +
                            "\nover USB (Quest Developer Hub file manager) or adb push, then start the app again.";
    __android_log_print(ANDROID_LOG_ERROR, kTag, "%s", msg.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Supermodel VR", msg.c_str(), nullptr);
    return false;
  }
  __android_log_print(ANDROID_LOG_INFO, kTag, "Starting %s (%s)", title.c_str(), rom.c_str());

  // The same arguments Scripts/vr/Play The Lost World VR.bat uses on Windows, plus -channels=2: The
  // Lost World asks for 4 speaker channels, but the headset is stereo, and a 4-channel stream can open
  // without error and play nothing. The emulator downmixes to 2 itself.
  args = {"supermodel", rom, "-vr", "-window", "-res=992,768", "-no-wide-screen", "-channels=2"};
  return true;
}
}  // namespace AndroidBoot

#endif  // __ANDROID__
