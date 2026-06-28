#ifndef UPDATER_H
#define UPDATER_H

#include <stddef.h>

struct UpdateManifest {
    bool available;
    char latestVersion[32];
    char releaseNotes[160];
    char artifactUrl[256];
    char artifactName[64];
    char artifactType[8];
    char sha256[65];
    int artifactSize;
};

typedef void (*UpdateProgressCallback)(int downloaded, int total, void *userData);

enum UpdateDownloadResult {
    UPDATE_DOWNLOAD_OK = 0,
    UPDATE_DOWNLOAD_NO_UPDATE = 1,
    UPDATE_DOWNLOAD_MANIFEST_FAILED = 2,
    UPDATE_DOWNLOAD_NO_ARTIFACT = 3,
    UPDATE_DOWNLOAD_FAILED = 4,
    UPDATE_DOWNLOAD_SIZE_MISMATCH = 5,
    UPDATE_DOWNLOAD_CHECKSUM_MISMATCH = 6,
    UPDATE_DOWNLOAD_INSTALL_FAILED = 7
};

class Updater {
public:
    static bool fetchManifest(const char *serverDomain, const char *httpPort, const char *currentVersion, UpdateManifest &manifest);
    static bool fetchManifest(const char *serverDomain, const char *httpPort, const char *currentVersion,
                              const char *packageType, UpdateManifest &manifest);
    static UpdateDownloadResult downloadUpdate(const char *serverDomain, const char *httpPort, const UpdateManifest &manifest,
                                               const char *targetPath, UpdateProgressCallback progress, void *userData);
    static UpdateDownloadResult downloadAndInstallCia(const char *serverDomain, const char *httpPort, const UpdateManifest &manifest,
                                                      const char *stagingPath, unsigned long long expectedTitleId,
                                                      UpdateProgressCallback progress, void *userData);
    static UpdateDownloadResult downloadUpdate(const char *serverDomain, const char *httpPort, const UpdateManifest &manifest);
    static bool checkForUpdate(const char *serverDomain, const char *httpPort, const char *currentVersion);
    static bool relaunchInstalledTitle(unsigned long long titleId);
    static const char *lastError();
};

#endif
