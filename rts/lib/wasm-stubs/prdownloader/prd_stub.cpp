/* WASM port: stand-in for the pr-downloader library (tools/pr-downloader/src).
 *
 * The WASM build has no libcurl, and game content is preloaded into the VFS rather
 * than downloaded, so every download request fails cleanly. This implements the
 * public API from pr-downloader.h that the engine (Lua/LuaVFSDownload.cpp) links against.
 *
 * CalcHash is the exception: it backs VFS.CalculateHash (BAR's widget handler keys
 * user widgets by it), so it is implemented exactly like pr-downloader's own.
 */
#include "../tools/pr-downloader/src/pr-downloader.h"
#include "../tools/pr-downloader/src/lib/md5/md5.h"

#include <base64.h>

#include <cstdlib>
#include <cstring>
#include <string>

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
// base64(MD5(str)); only type 0 is supported (as in pr-downloader). Caller free()s.
char* CalcHash(const char* str, int size, int type)
{
	if (type != 0)
		return nullptr;

	MD5_CTX ctx;
	MD5Init(&ctx);
	MD5Update(&ctx, (unsigned char*)str, size);
	MD5Final(&ctx);

	const std::string encoded = base64_encode(ctx.digest, 16);
	char* ret = static_cast<char*>(malloc(encoded.size() + 1));
	std::memcpy(ret, encoded.c_str(), encoded.size() + 1);
	return ret;
}
void SetAbortDownloads(bool) {}
DownloadEnum::Category getPlatformEngineCat() { return DownloadEnum::CAT_NONE; }
