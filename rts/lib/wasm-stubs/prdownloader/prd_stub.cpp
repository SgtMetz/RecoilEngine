/* WASM port: stand-in for the pr-downloader library (tools/pr-downloader/src).
 *
 * The WASM build has no libcurl, and game content is preloaded into the VFS rather
 * than downloaded, so every download request fails cleanly. This implements the
 * public API from pr-downloader.h that the engine (Lua/LuaVFSDownload.cpp) links against.
 */
#include "../tools/pr-downloader/src/pr-downloader.h"

int DownloadStart() { return 1; } // nonzero = failure
int DownloadAddByUrl(DownloadEnum::Category, const char*, const char*) { return -1; }
bool DownloadAdd(unsigned int) { return false; }
int DownloadSearch(DownloadEnum::Category, const char*) { return 0; } // 0 results
int DownloadSearch(std::vector<DownloadSearchItem>&) { return 0; }
bool DownloadGetInfo(int, downloadInfo&) { return false; }
void DownloadInit() {}
void DownloadShutdown() {}
bool DownloadSetConfig(CONFIG, const void*) { return true; }
bool DownloadGetConfig(CONFIG, const void**) { return false; }
bool DownloadRapidValidate(bool) { return false; }
bool DownloadDumpSDP(const char*) { return false; }
bool ValidateSDP(const char*) { return false; }
void DownloadDisableLogging(bool) {}
void SetDownloadListener(IDownloaderProcessUpdateListener) {}
char* CalcHash(const char*, int, int) { return nullptr; }
void SetAbortDownloads(bool) {}
DownloadEnum::Category getPlatformEngineCat() { return DownloadEnum::CAT_NONE; }
