# Release workflow

- Routine releases use plain `0.x.y` versions starting with `0.2.0` after
  `0.1.0-alpha.39`. Increment the patch for fixes/refinements and the minor for
  meaningful feature releases (reset patch to zero). Do not continue numbering
  everyday releases as alpha. Reserve alpha/beta/rc for explicit preview builds;
  decide the 1.0 milestone separately rather than inferring it from daily use.
- Keep numeric file versions synchronized as `major.minor.patch.0`, including
  Settings FileVersion and the installer. Preserve historical release tags/notes.

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
