// The probe's host app: it carries the FSKit extension (Contents/Extensions), which macOS finds
// when the app is registered. Given folders (open "MLV Drive Probe.app" --args <folder> ...), it
// reads them once: macOS asks the user for access to protected ones (Desktop, Documents, removable
// and network volumes), and the extension, whose reads macOS checks against this app, can read
// them from then on.
#import <Foundation/Foundation.h>

#include <dirent.h>

#include <cerrno>
#include <cstdio>
#include <cstring>

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        if (argc < 2) {
            std::printf("MLV Drive Probe: turn on \"MLV Drive Probe\" in System Settings > General > "
                        "Login Items & Extensions > File System Extensions, then mount (see macos/probe/README.md).\n");
        }
        for (int i = 1; i < argc; ++i) {
            DIR* d = opendir(argv[i]);
            if (!d) {
                std::printf("%s: NOT readable: %s\n", argv[i], std::strerror(errno));
                continue;
            }
            size_t n = 0;
            while (readdir(d)) ++n;
            closedir(d);
            std::printf("%s: readable, %zu entries\n", argv[i], n);
        }
    }
    return 0;
}
