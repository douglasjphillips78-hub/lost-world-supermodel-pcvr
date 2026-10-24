#ifdef __ANDROID__

#include "AndroidBoot.h"

#include <SDL.h>
#include <android/log.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <thread>

namespace
{
const char* kRomName = "lostwsga.zip";
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
    const bool have = Exists(rel);
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

  const std::string rom = std::string("ROMs/") + kRomName;
  if (!Exists(rom)) {
    WriteFile("ROMs/PUT-ROMS-HERE.txt", std::string("Put your own ") + kRomName + " ROM set in this folder, then start the app again.\n");
    const std::string msg = std::string("ROM not found.\n\nCopy ") + kRomName + " (The Lost World, Japan, Rev A) to:\n" + ext +
                            "/ROMs/\n\nover USB (Quest Developer Hub / file transfer) or adb push, then start the app again.";
    __android_log_print(ANDROID_LOG_ERROR, kTag, "%s", msg.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Supermodel VR", msg.c_str(), nullptr);
    return false;
  }

  // The same arguments Scripts/vr/Play The Lost World VR.bat uses on Windows.
  args = {"supermodel", rom, "-vr", "-window", "-res=992,768", "-no-wide-screen"};
  return true;
}
}  // namespace AndroidBoot

#endif  // __ANDROID__
