#ifndef DOODLE_HOST_TEST_COMPAT_H
#define DOODLE_HOST_TEST_COMPAT_H

#ifdef _WIN32
#include <direct.h>

inline int doodleHostTestMkdir(const char *path, int)
{
    return _mkdir(path);
}

#define mkdir doodleHostTestMkdir
#endif

#endif
