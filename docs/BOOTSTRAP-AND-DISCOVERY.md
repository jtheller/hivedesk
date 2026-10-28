# Boot, console ownership and network recovery

Before 0.8.3, the service waited for `WTSQueryUserToken` before starting its
broker, and only the signed-in tray owned the network listener. An automatic
service therefore did not make a machine reachable before its first logon.
Windows documents that this API retrieves the **logged-on user's** token:
[WTSQueryUserToken](https://learn.microsoft.com/en-us/windows/win32/api/wtsapi32/nf-wtsapi32-wtsqueryusertoken).

The service now retries console availability every second and responds to
session and power events. `ERROR_NO_TOKEN` selects the prelogon role; other token
errors retry without interpreting an authorization failure as an empty console.
The broker uses SYSTEM-only pipe permissions in that role. A separate headless
mode of the installed HiveDesk executable owns discovery/transport and reuses
HostBridge, including its independent input lane and desktop-change recovery.
It creates no Shell, accesses no clipboard and runs no viewer. Its parent must
be the adjacent service executable, and only a SYSTEM token may enter the mode.

The service caches the console user's public device ID, display name and startup
preference under HKLM\Software\HiveDesk\Bootstrap. It reads those specific
preferences while impersonating that user, then returns to SYSTEM to write the
cache. No passwords, network keys, PINs, clipboard or executable paths are stored.
A new user configuration can adopt the cached device ID; existing configurations
retain theirs. This is the existing single-console model, not multi-user hosting.

Lock/unlock keeps the same user SID and broker. Logon/logoff or console migration
changes the role: the service signals its pinned headless child to release the
listener, bounds shutdown, then replaces the broker with the new permissions.
The ordinary tray takes over the usual port. It retries a temporarily occupied
listener rather than exiting during this handoff. Service crash recovery is
registered with SCM at 1, 3 and 10 seconds. No physical logoff/reboot is requested
by the implementation or its automated checks.

OTA counts the headless process as an agent, waits for actual control/live Peek
to finish, and never restarts it with a retained user token. Idle warm leases may
retire for update. The service suppresses prelogon respawn while update is pending;
the existing startup barrier closes races with installation.

Discovery observes IPv4 address, interface and route changes. Notification
callbacks only set an atomic flag; the owner cancels subscriptions outside locks,
following the Windows cancellation restrictions:
[NotifyUnicastIpAddressChange](https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-notifyunicastipaddresschange).
Address polling every five seconds covers missed/failed subscriptions. A change
rebuilds UDP discovery sockets with a 250 ms rebind throttle and starts a ten-second
250 ms announcement burst. Quiet operation returns to two-second announcements.
Directed broadcasts bind the source address of each local interface. These
timings are retry/pacing policy, not a guarantee of discovery within 250 ms.

The TCP listener is wildcard-bound: adding an address does not require destroying
it. Failed accept/bind operations retry every 500 ms. UDP bind failures also retry
every 500 ms. Existing authenticated streams survive route/metric changes; only
connections bound to a removed local address are closed. Dial/handshake work has
a separate FIFO with at most 64 queued endpoints and per-address retry admission,
so one unreachable peer cannot stop broadcasting or receiving announcements.

Tests reserve isolated ephemeral TCP/UDP ports, force actual bind failures,
release them, establish encrypted connections, interrupt the accept listener,
and verify recovery plus preservation of existing connections during notification
bursts. Discovery tests use a private ephemeral port so generated devices do not
appear in the real Hive. The prelogon test uses encrypted transport and a fake
desktop broker, exercising warm input refusal, promotion/retry, input ACKs,
stale warm cancellation and stop, with no physical input. A real cold boot to the
first Windows sign-in screen, remote login and handoff remain a separate physical
acceptance check; the automated tests do not establish that result.
