#ifndef UPDATER_H
#define UPDATER_H

class Updater {
public:
    static bool checkForUpdate(const char *serverDomain, const char *httpPort, const char *currentVersion);
};

#endif
