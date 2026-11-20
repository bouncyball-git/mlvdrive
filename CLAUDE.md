# MLV Drive

Shows Magic Lantern MLV clips as a virtual drive of CinemaDNG sequences (WinFsp on Windows), so
DaVinci Resolve imports MLV directly. The plan and milestones are in `PLAN.md`: read it first and
keep it up to date as milestones are done.

## Rules

- Independent project: it builds from this folder only. Code from other projects is copied in,
  never referenced by path; after copying, don't mention where it came from except the required
  credits (MLV-App in `third_party/mlvapp`, MLVFS for ported parts).
- After every change, suggest a ready-to-use commit message (subject + short body) in a code
  block at the end of the reply. Don't run `git commit` unless asked.
- C++17, CMake + Ninja, MSVC static runtime, LF line endings.
- Test clip: `E:\mlv\M21-1642.MLV` (5D Mark III, 1080p, 14-bit LJ92, 261 frames, 23.976 fps, audio).
