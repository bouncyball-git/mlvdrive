// The probe's host app: it only carries the FSKit extension (Contents/Extensions), which macOS
// finds when the app is registered. Run once, it says how to turn the extension on.
#import <Foundation/Foundation.h>

#include <cstdio>

int main() {
    @autoreleasepool {
        std::printf("MLV Drive Probe: turn on \"MLV Drive Probe\" in System Settings > General > "
                    "Login Items & Extensions > File System Extensions, then mount (see macos/probe/README.md).\n");
    }
    return 0;
}
