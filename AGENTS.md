# Release workflow

- Repository: https://github.com/jtheller/hivedesk (public).
- Every distributable package must have a matching GitHub Release. Increment the
  version in CMake, native resources, Settings and Inno Setup; add release notes;
  commit the source before running `scripts/build.ps1 -Package`.
- `-Package` publishes by default from a clean commit. `-NoPublish` is only for
  temporary local validation, never the final handoff. Do not replace published
  release assets or reuse a version for different binaries.
- Release signing private material stays outside the repository in the user's
  Windows DPAPI store. Never print, commit or upload credentials/private keys.
- Automatic updates must not interrupt inbound, outbound, starting or reconnecting
  remote sessions. Do not treat idle discovery connections as active sessions.
- Do not claim live OTA installation was tested when only signature/download or
  headless checks ran. Do not run desktop-input probes on the user's active work.
