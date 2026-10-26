# Releases and automatic updates

Repository: https://github.com/jtheller/hivedesk

## Publish from this Windows build machine

1. Increment `HIVEDESK_VERSION`, native version resources, Settings versions and
   the Inno Setup version. Add `docs/releases/<version>.md`.
2. Run the relevant native tests; commit all source changes. Release packages
   require a clean Git tree and record the exact commit in `build-commit.txt`.
3. Run `scripts/build.ps1 -Package` (or `-SkipTests -Package` after running the
   checks separately). This builds locally, creates the installer/ZIP, signs the
   update manifest, pushes `main` and `v<version>`, uploads assets to a draft,
   verifies uploaded sizes/digests, then publishes it as the latest release.

This uses Git Credential Manager, not GitHub Actions or CI minutes. `-NoPublish`
is an explicit local-verification escape hatch; a final distributable must be
published. Published versions/assets are never overwritten by this workflow.
An interrupted draft upload can be retried with `scripts/publish-release.ps1
-ReleaseDirectory <release directory>` while the original source commit remains
checked out and clean. A failed upload leaves the previous published latest
release intact.

The default channel follows releases marked latest, including the current alpha
version series. Release drafts are invisible to clients. GitHub's `prerelease`
flag is deliberately false for this personal deployment channel; the version and
release notes still clearly identify alpha status. The developer ZIP is local;
users and OTA install the Setup executable, including its service.

## Signing key

The public RSA-3072 key is in `resources/update-public-key.json` and compiled into
the updater. The private key is outside the checkout, in
`%LOCALAPPDATA%\HiveDeskRelease\signing-key.dpapi`, encrypted with Windows
CurrentUser DPAPI and project-specific entropy. Publishing fails if it is missing
or does not match the pinned public key. It is never a release asset or CI secret.

Keep a secure recovery backup of the signing key and its Windows DPAPI context.
Copying the `.dpapi` file to another Windows account/machine alone is not a key
migration. A developer-machine migration requires an explicit secure key export
and reprotection step. Do not silently generate a replacement key: existing
clients would reject it. This release signature is separate from Authenticode.

`update.manifest` is a strict LF/ASCII five-line format containing the schema,
numeric version, canonical installer name, exact byte size and SHA256. Its
detached signature uses RSA/SHA256 PKCS#1 v1.5. The updater verifies the signature
before parsing, constructs a fixed repository/tag asset URL, rejects downgrade
versions and validates bytes again before execution. The signing fixture in
`tests/fixtures/update` covers an inert 8-byte file at version `0.0.0-alpha.1`.

## Idle installation

The existing SYSTEM service starts a windowless updater from an ACL-protected
`%ProgramData%\HiveDeskUpdates` cache. The helper is copied out of Program Files
so the installer can replace the installed executables. No new inbound port or
remote update command is added; only outbound HTTPS to GitHub release assets.

The updater downloads before asking any app to exit. It then holds a global
startup mutex and sets a SYSTEM-owned request event. Apps check every second,
serialize their idle decision with inbound/outbound session admission, stop
discovery and quit gracefully only when no active/start/reconnect intent or
queued work remains. The updater waits for the actual installed processes to
exit; it never kills an active controller to make an update proceed. New app
starts cannot cross the held installation barrier.

The helper retains the normal-user tokens before signaling exit. Setup runs
with `/OTA=1 /VERYSILENT /SUPPRESSMSGBOXES /SP- /NORESTART
/NOCLOSEAPPLICATIONS /NORESTARTAPPLICATIONS`; it skips launching a SYSTEM-owned
tray. After setup the helper verifies the installed version, releases the
barrier and restarts previously running trays with their original user tokens.
No automatic Windows reboot is performed. Setup failures are logged, the
barrier is released, and any usable installed tray is restarted. This is not a
transactional rollback installer; a partially failed installation can require
manual repair with the published Setup executable.

Checks recur every 15 minutes. Slow/failed downloads and bad signatures leave
the installed version running. A pending reconnect intentionally blocks the
update until it succeeds/ends or the user returns locally. A standalone orphan
Settings process also defers installation until closed. Update logs are in
`%ProgramData%\HiveDeskUpdates\update.log` (bounded to about 1 MiB) and
`setup.log`; the cache is restricted to SYSTEM/Administrators. Local agent
shutdown decisions are also logged in the usual per-user log.

Read-only verification commands:

```powershell
.\build\HiveDeskUpdater.exe --verify-release .\dist\HiveDesk-0.1.0-alpha.27
.\build\HiveDeskUpdater.exe --check-only
```

These verify signed assets/download metadata and do not install, signal the
running app or inject desktop input. A true SYSTEM OTA upgrade and both-machine
session deferral need validation after the first manual bootstrap installation.

References: [GitHub Releases API](https://docs.github.com/en/rest/releases/releases),
[Inno Setup command-line options](https://jrsoftware.org/ishelp/topic_setupcmdline.htm).
