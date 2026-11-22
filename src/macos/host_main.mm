// MLV Drive.app for now: it carries the FSKit module (Contents/Extensions/MLVDriveFS.appex),
// which macOS finds when the app is registered, and says how to turn it on and mount. (The menu
// bar app comes later, PLAN.md milestone 8.)
#import <Foundation/Foundation.h>

#include <cstdio>

int main() {
    @autoreleasepool {
        std::printf(
            "MLV Drive " MLVDRIVE_VERSION "\n"
            "1. Turn on \"MLV Drive\" in System Settings > General > Login Items & Extensions > File System\n"
            "   Extensions (quit and reopen System Settings if it isn't listed).\n"
            "2. For clips in protected places (Desktop, Documents, external and network drives): System\n"
            "   Settings > Privacy & Security > Full Disk Access > + > MLV Drive.\n"
            "3. mount -F -t mlvdrive <MLV folder> <mount point>     (umount <mount point> to unmount)\n");
    }
    return 0;
}
