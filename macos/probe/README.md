# FSKit probe

A throwaway FSKit module (PLAN.md, milestone 8, step 2): it measures how fast FSKit serves MLV
Drive's kind of volume before the real one is written. It is a read-only volume of fake DNG-sized
files (`f_000000.dng` ...) whose bytes are one memcpy from a pattern, plus `probe_report.txt`:
the request counts so far, and whether the sandboxed extension could read its folder and a second
one. It is pure Objective-C++ (entry point `NSExtensionMain`, principal class from the
Info.plist), like Apple's own FSKit modules.

```
macos/probe/build.sh          # build/probe/MLV Drive Probe.app (+ the .appex), build/probe/probe_check
```

Then, once:

1. Register it: open the app once (`open "build/probe/MLV Drive Probe.app"`); then quit and
   reopen System Settings, which lists modules as it found them when it opened.
2. Turn it on: System Settings > General > Login Items & Extensions > File System Extensions >
   "MLV Drive Probe". (`pluginkit -e use` isn't enough: FSKit keeps its own list.)
3. For protected folders (Desktop, Documents, external and network volumes): System Settings >
   Privacy & Security > Full Disk Access > + > the app. The extension's reads are checked against
   the app; a permission for one folder (the app's Allow prompt) isn't passed on, Full Disk
   Access is.

Measure:

```
macos/probe/run.sh sweep                    # every variant: reads, check, report
macos/probe/run.sh "iosize=4194304,reply=sync"
SECOND=/Volumes/Other/mlv macos/probe/run.sh  # can the extension read a second folder?
```

By hand: `mount -F -t mlvprobe -o files=300,iosize=1048576 <folder> <mount point>`, read it
(`build/mlvdrive_tool read <mount point>`, `build/probe/probe_check <mount point>`), `umount`.
The options are listed at the top of `probe_fs.mm`.

Signing: ad hoc, with the app sandbox as the only entitlement. The FSKit entitlement
(`com.apple.developer.fskit.fsmodule`) is restricted: an ad hoc signed binary carrying it is killed
at launch ("adhoc signed but contains restricted entitlements"); without it FSKit runs the module.
After a rebuild that made a new bundle, turn the module off and on again in System Settings.
