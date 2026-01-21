Changes in 1.21.0
=================
Released: 2026-01-21

New Features:

- Add the `has_current_page` and `has_selected_pages` options to the Print
  Portal (#1777)
- Allow running the tests with Valgrind's memcheck (#1770)
- Add the `ConfigureShortcuts` method to the Global Shortcuts Portal (#1661)
- Send activation tokens in the actiavated and deactiavated signals on the
  Global Shortcuts Portal (#1792)
- Add a new reduced motion setting to the Settings Portal (#1840)
- Support linyaps applications (#1846)
- Add missing cell broadcast severities to the Notification Portal (#1738)

Enhancements:

- Code cleanup (#1574, #1809, #1771, #1727)
- Code refactoring (#1805, #1815, #1819, #1686)
- Documentation improvements (#1712, #1759, #1776, #1795, #1798, #1866)
- New and updated translations (#1735, #1760, #1765, #1774, #1780, #1781,
  #1786, #1787, #1789, #1797, #1800, #1801, #1802, #1811, #1833, #1826,
  #1852, #1841, #1843, #1851, #1869, #1860, #1788)
- Improve various permission dialog texts (#1769, #1327, #1804)
- Release procedure clarifications (#1710, #1714)
- Updates to ASAN suppressions (#1711)
- Make XdpAppInfo more testable (#1627)
- Use the new `PIDFD_GET_PID_NAMESPACE` ioctl to get the pidns (#1713)
- Improvements to the heuristics to translate a path in the sandbox to a path on
  the host (#1571)
- Improve the mocking of the GeoClue service (#1695)
- Make the camera permissions per-App on the host again (#1762)
- Clean up permissions and desktop IDs usage (#1772, #1773)
- Improve PID translations for host Apps (#1785)
- Show an appropriate error when access to remote documents fails (#823)
- Require a valid AppID from apps in `RequestBackground` to enable autostart
  (#1793)
- Require a valid AppID from apps to use the Global Shortcuts Portal (#1817)
- Test and document Notification Portal backward compatibilities (#1823)
- Improve the heuristic to detect the App ID for host apps (#1595)
- Add Merge Requirements documentation (#1775)
- Initialize the Secret Portal asynchronously to avoid blocking when the secret
  service is not available (#1814)
- Do not allow requesting a zero capability from the Input Capture Portal
  (#1880)
- Require GLib version 2.76 and drop the related compatibility code (#1730)

Bug Fixes:

- Fix a crash when loading information from Flatpak apps (#1720)
- Fix fd handling to prevent EBADF errors (#1721)
- Add a fallback code path for GLib older than 2.76 (#1728)
- Don't require a .desktop file for Flatpak and Snap apps (#1729)
- Fix a crash when calling `GlobalShortcuts.BindShortcuts` with an empty list
  (#1732)
- Fix a crash when passing Request token handles which contain `-` (#1748)
- Fix tests on systems without access to /proc/cmdline (#1766)
- Stop accidentally running pytests of subprojects (#1767)
- Give up trying to unmount an existing fuse mount when shutting down the
  Document Portal (#1799)
- Fix compilation on Debian Testing, caused by a wrong cast (#1625)
- Fix ownership of pidfd for XdpAppInfos (#1810)
- Fix uninitialized variables (#1825)
- Do not give access to read-only USB devices when read-write access was
  requested (#1794)
- Do not kill PID 0 and handle races properly (#1864)
- Fix forwarding the `available-source-types` and `available-cursor-modes` from
  the backend (#1868)
- Ensure valid WAYLAND_DISPLAY/DISPLAY by launching after the graphical session
  target (#1830)

Changes in 1.20.1
=================
Released: 2025-05-15

Enhancements:

- Code cleanups and improvements to app info tracking
- Include PID/TID in realtime portal error messages
- Search for portal backends in $XDG_DATA_DIRS (#603)
- Prioritize user portal configs over system ones

Bug Fixes:

- Fix race condition in the host registry portal
- Avoid spurious warnings when dbus.service stops
- Documentation fixes (#1663)
- Fix running tests from /tmp
- Fix installing dynamic launcher (#1674)
- Improve error reporting in the document portal
- Fix incorrect state tracking in input capture portal

Changes in 1.20.0
=================
Released: 2025-02-19

Enhancements:

- Document how the test suite works.
- Improve the test runner script.

Changes in 1.19.4
=================
Released: 2025-02-15

New Features:

- Introduce the host app registry. This interface allows host system apps
  (i.e. apps not running under a sandboxing mechanism like Flatpak) register
  themselves with XDG Desktop Portal. This allows XDG Desktop Portal to use
  a proper app id, and desktop file, improving the interaction with portal
  backends.

Enhancements:

- Use a new internal script to simplify running tests.

Bug Fixes:

- Properly escape notification body in the Notification portal.
- Fix various documentation links in the USB portal documentation page.

Changes in 1.19.3
=================
Released: 2025-02-12

Bug Fixes:

- Fix documentation links in the USB portal page.
- Make the Document portal track open files, and release them when shutting
  down. This should fix some harmless leak reports.
- Fix a memory leak, a crash, and improve robustness against non-existing
  folders in the Dynamic Launcher portal.
- Fix build with PipeWire 1.3.82

Enhancements:

- Make the host path xattr more useful by removing the trailing end character,
  and also reporting the xattr of files inside folders added to the document
  store.
- Remove libportal-based integrated tests. This should remove the cyclic
  dependency between libportal, and xdg-desktop-portal. All tests are now based
  on the Python testing framework.

Changes in 1.19.2
=================
Released: 2025-01-20

Bug Fixes:

- Fix permission check for host system apps in the Camera portal.
- Do not expose the Settings portal if there are no backends available.
- Disable sounds-related notification tests if the project is built without
  wavparse.

Enhancements:

- Start porting the test suite to Python tests. Once finished, this should
  break the cyclic dependency between xdg-desktop-portal and libportal.
- Install Python-based tests. This is mostly useful for distributions to run
  tests as part of their packaging process.

Changes in 1.19.1
=================
Released: 2024-12-21

Dependencies:

- XDG Desktop Portal now requires GLib 2.72 or higher.

New Features:

- Introduce the Notification v2 portal. This updated version of the Notification
  portal supports a plethora of new fields for notifications, such as sounds,
  categories, purpose, and more.
- Introduce the USB portal. This portal allows apps with relevant permissions
  to enumerate and acquire access to specific USB devices.
- Introduce a new `SchemeSupported` method to the OpenURI portal. This new
  method allows apps to know ahead of time if the host system is able to deal
  with a particular scheme.

Enhancements:

- Continued the move towards Python-based tests. This should simplify the
  test setup in the project quite significantly, and also will allow removing
  the cyclic dependency between libportal and XDG Desktop Portal.
- Introduce umockdev-based tests.
- Improve the icon validator so it can deal with memfd-based icons.
- Clarify behavior of the Settings portal for non-standardized keys.
- In the Global Shortcuts portal, clarify that the result the `BindShortcuts`
  of may be a subset of all requested shortcuts.
- Add a documentation page about icon validation requirements.

Bug Fixes:

- Fix memory leaks in the Background, Email, and Global Shortcuts portals.
- Fix a general file descriptor & memory leak.
- Fix a regression in the Settings portal.
- New and updated translations.

Changes in 1.19.0
=================
Released: 2024-10-09

- Completely rework and restructure the documentation website. Documentation is
  now segmented by target audience (app developers, desktop developers, and
  contributors). It also documents how the Document portal operates, the FUSE
  filesystem, and custom file attributes. This is available in the following
  address: https://flatpak.github.io/xdg-desktop-portal/docs/index.html
- The portals.conf parser is now able to handle fallback backends better, and
  respects the order of backends in the config file.
- Try to use the xdg-desktop-portal-gtk backend as a last resort backend, if
  everything else fails.
- Implement getlk and setlk, and honour O_NOFOLLOW, in the Document portal's
  FUSE filesystem.
- Neutralize the Devices portal. Originally the Devices portal was introduced
  so that services like PulseAudio or PipeWire could request access to
  microphones and cameras on the behalf of apps. It was not meant to be used by
  sandboxed apps directly, which is unusual for a portal. Practically, however,
  it didn't take off.
- Implement PID/TID mapping for host system apps.
- Add a new "supported_output_file_formats" option to the Print portal. This
  can be used by apps like browsers to limit the output file formats presented
  by the Print portal backend. For example, an app can limit file printing to
  PDF files.
- Add a new "GetHostPaths" method to the Document portal, which allows mapping
  file descriptors to paths on the host system. This can be used by apps to
  show more meaningful file paths in the user interface.
- Like the new method above, the Document portal sets the
  "user.document-portal.host-path" xattr on files, pointing to the the host
  system file path. The use case is similar to "GetHostPaths".
- Make the Background portal more robust when validating autostart files.
- Clarify behavior of the File Chooser portal in the documentation pages.
- Improve robustness against deleted o_path fds in the Document portal.
- Fix a warning in some systems while trying to load Request D-Bus object
  properties.
- Fix a physical inode leak in the Document portal.
- Various improvements to the test suite. Python-based tests now run in parallel
  and are more careful when setting up the mock D-Bus server. Tests also start
  dbus-monitor if necessary now. FUSE tests of the Document portal have been
  made more TAP-alike now.
- Memory leak fixes in a variety of portals and services, including the
  permissions database, the Document portal, the File Transfer portal, the
  Location portal, the Background portal, tests, and the icon validator. And
  more. There's a lot of memory leak fixes everywhere, really.
- Major refactorings of the icon validator. Icons are now limited to 4MB files.
- Update XML specification specifying session handle type to match current
  actual ABI in GlobalShortcuts, Inhibit, RemoteDesktop, and ScreenCast portals.
- New and updated translations.

Changes in 1.18.1
=================
Released: 2023-10-26

- Communicate better when the Background portal kills an app
- Properly quote Flatpak command in the Background portal
- Improve documentation of the "cursor_mode" property of the ScreenCast
  backend D-Bus interface
- Fix ScreenCast portal removing transient restore permissions too early.
  This fixes screen sharing dialogs on Chromium asking for the screen multiple
  times.
- Only send the Closed session signal to the sender
- Add Meson options to disable building with Bubblewrap, and without the
  Flatpak portal documentation. Disabling Bubblewrap is **highly** discouraged,
  and only meant to be used on platforms that do not currently support it. By
  disabling Bubblewrap, bitmap validation happens without a sandbox, which is
  highly insecure since image parsing is a common source of exploits. Really,
  just do not disable Bubblewrap please.
- Improve the manpage of portals.conf
- Various spelling fixes to the Document portal
- Add a new website! We don't have a fancy domain yet, but the website can be
  accessed at https://flatpak.github.io/xdg-desktop-portal/
- Improve pid mapping for host system apps. This should get rid of some rare,
  unnecessary warnings.
- Adjust documentation of Global Shortcuts portal's timestamps to millisecond
  granularity
- Bump minimum Meson version to 0.58

Changes in 1.18.0
=================
Released: 2023-09-18

- Bump interface version of the Printer portal to 2
- Validate addresses following the HTML specs in the Email portal

Changes in 1.17.2
=================
Released: 2023-09-01

- Document minimum version of the new ReadOne() method of the Settings portal
- Add a mapping id property to the ScreenCast portal
- Add activation token parameter to the Email portal
- Test tarball generation in CI

Changes in 1.17.1
=================
Released: 2023-08-27

- Document xdg-desktop-portal versioning scheme
- Fix various issues in the OpenURI portal
- Introduce the ReadOne() method in the Settings portal. This method is now
  preferred over the Read() method, as Read() mistakenly returned a variant
  inside a variant. The Read() method will continue to exist for compatibility
  with existing apps, but its usage is deprecated. We recommend apps to port
  to the ReadOne() method. Apps can decide whether to use ReadOne() or Read()
  by looking at the version of the Settings portal.
- Improvements to the new config-based portal matching mechanism. Config files
  are now searched in standard paths, in a way that is compatible to other
  system components (e.g. MIME types).
- Various small visual tweaks to the generated documentation
- Document a new 'accent-color' key in the Settings portal. This key represents
  an arbitrary color in sRGB color space. How implementations of the portal
  provide this key is entirely dependent on their internal policies and design.
- Translation updates

Changes in 1.17.0
=================
Released: 2023-08-04

- Drop the Autotools build. Meson is now the only supported build system.
- Rework how portal implementations are loaded. This new, more robust system
  allows selecting specific backends for specific portals, and layering them
  when necessary. Platforms that provide portals implementation are encouraged
  to provide a suitable configuration file.
- Introduce a new Clipboard portal. This portal extends the Remote Desktop
  portal by adding support for sharing clipboard between remote machines.
- Introduce a new Input Capture portal. This portal adds mechanisms for taking
  control of input devices. The primary usage model is centered around the
  InputLeap and Synergy use cases, where local devices are used to control
  remote displays.
- Stop using the deprecated GTimeVal struct
- Bump GLib dependency to 2.66
- Add an "accept-label" option the the Print portal. This lets apps suggest a
  proper label to the print operation.
- Various fixes to the Global Shortcuts portal
- Support restoring remote desktop sessions
- Improve robustness of the OpenURI portal by validating more URIs
- The PipeWire dependency is now mandatory
- Various improvements for the test suite
- Translation updates

Changes in 1.16.0
=================
Released: 2022-12-12

- Introduce a new background monitoring service. This allows desktop
  environments to list applications that are running in background, that is,
  sandboxed applications running without a visible window. Desktop environments
  can display these background running applications in their interfaces, and
  allow users to control their execution.
- Introduce the Global Shortcuts portal. This portal allows applications to
  register and receive keyboard shortcuts even when they're not focused. This
  was a highly requested feature, especially on Wayland desktops. There are
  improvements to come, but portal backends can now implement this new portal.
- Various CI improvements
- Translation updates

Changes in 1.15.0
=================
Released: 2022-08-11

- Add Meson build files. For now, both Autotools and Meson are available
  in the source tree. Starting from next release, tarballs will be generated
  using Meson. There is no set date to delete Autotools files, but it will
  happen at some point in the future. The removal of Autotools will be
  communicated in advance.
- Make the Screenshot portal request permission to take screenshots. Frontends
  that implement the version 2 of org.freedesktop.portal.impl.Screenshot portal
  can now be aware that the screenshot permission was granted through the new
  'permission_store_checked' option, and skip any kind of dialog when that is
  the case.
- Stop sending the app id quoted in the Background portal
- Fix a bug in cgroup name parsing
- Various fixes to the Realtime portal
- Various CI improvements
- Translation updates

Changes in 1.14.1
=================
Released: 2022-03-18

- Fix an issue in 1.14.0 where xdg-desktop-portal.service starting before
  graphical-session-pre.target would cause the GNOME session to deadlock by
  moving code to a new libexec binary which handles deleting or migrating
  .desktop files (and icons) from the dynamic launcher portal for apps which
  have been uninstalled or renamed.
- Fix some bugs in the aforementioned launcher migration implementation,
  "xdg-desktop-portal-rewrite-launchers".
- Fix build without libsystemd

Changes in 1.14.0
=================
Released: 2022-03-17

- Add a new "dynamic launcher" portal, which can install .desktop files and
  accompanying icons after user confirmation.
- Rework handling of empty app IDs: In case an empty string app ID is stored in
  the permission store, this permission is now shared only by apps whose app ID
  couldn't be determined, rather than all unsandboxed apps.
- Use libsystemd (when available) to try to determine the app ID of unsandboxed
  processes. This is useful since some portals otherwise can't be used by host
  apps.
- Make x-d-p start on session start, which is needed for the dynamic launcher
  portal to handle rewriting launchers for apps that have been renamed.
- Bring back the copy of Flatpak's icon-validator, which was dropped many
  releases ago.
- Icon validation is now required for the notification and dynamic launcher
  portals (previously it was only done if the "flatpak-validate-icon" binary
  could be found).
- document-portal: Move to the libfuse3 API
- document-portal: Use renameat2 sys call
- document-portal: Use mutex to fix concurrency bug
- realtime: Fix error code paths
- realtime: Fix MakeThreadHighPriorityWithPID method
- screencast: Fix an error when restoring streams
- ci: Various improvements
- Documentation improvements
- New translations: pt, ro

Changes in 1.12.1
=================
Released: 2021-12-22

- Fix a crash in the device portal

Changes in 1.12.0
=================
Released: 2021-12-21

- Place portals in the systemd session.slice
- settings: Add color-scheme key
- open-uri: Avoid a sync call to org.freedesktop.FileManager
- screencast: Allow restoring previous sessions
- Add a portal for requesting realtime permissions
- ci: Many improvements
- Publish the docs from a ci job
- Translation updates

Changes in 1.10.0
=================

- Remap /run/flatpak/app, for Flatpak 1.11.x
- Remap /var/config and /var/data
- permission-store: Avoid a crash
- permissions-store: Add GetPermission
- screencast: Add 'virtual' source type
- openuri: Use real path for OpenDirectory
- location: Fix accuracy levels
- Add power profile monitor implementation
- Translation updates

Changes in 1.8.1
================

- openuri: Fix an fd leak
- filechooser: Fix directory support
- build: Drop a fontconfig dependency
- snap: Use cgroups to identify confined apps
- documents: Add snap support
- wallpaper: Fix a crash
- build: Factor out a fuse check
- Translation updates

Changes in 1.8.0
================

- openuri: Allow skipping the chooser for more URL types
- openuri: Robustness fixes
- filechooser: Return the current filter
- camera: Make the client node visible
- camera: Don't leak pipewire proxy
- Fix file descriptor leaks
- Testsuite improvements
- Translation updates

Changes in 1.7.2
================

- document: Reduce the use of open fds
- document: Add more tests and fix issues they found
- Fix the build with musl

Changes in 1.7.1
================

- filechooser: Add a "directory" option
- filechooser: Document the "writable" option
- document: Expose directories with their proper name

Changes in 1.7.0
================

- testsuite improvements
- background: Avoid a segfault
- screencast: Require pipewire 0.3
- document: Support exporting directories
- document: New fuse implementation
- better support for snap and toolbox
- Translation updates

Changes in 1.6.0
================

- tests: Adapt to libportal api changes

Changes in 1.5.4
================

- background: Add a signal to the impl api
- background: Rewrite the monitoring to better track when apps disappear
- permissions: Fix SetValue handling of GVariant wrapping. This is an api change
- openuri: Add a per-type always-ask option
- openuri: Show the app chooser dialog less often
- memorymonitor: A new portal to let apps receive low memory warnings
- filetransfer: A new portal to rewrite file paths between sandboxes

Changes in 1.5.3
================

* Add more tests
* Translation updates
* location: Various fixes
* document portal: Monitor fuse mount
* openuri: Only ask 3 times to use the same app
* openuri: Add an 'ask' option
* Fix build from git
* email: Allow multiple addresses, cc and bcc
* filechooser: Allow saving multiple files

Changes in 1.5.2
================

* Add many more tests, using libportal
* gamemode: Add a pidfd-based api
* inhibit: Send a Response signal
* openuri: Add an OpenDirectory api
* Translation updates

Changes in 1.5.1
================

* Add a portal for setting desktop backgrounds
* Add tests
* Optionally use libportal (for tests)

Changes in 1.5.0
================

* Add a secret portal that is meant be used via
    libsecret inside the sandbox. One backend for
    this will live in gnome-keyring, others are
    possible
* Fix a file descriptor leak
* Reduce log spam
* Translation updates

Changes in 1.4.2
================

* Build fixes

Changes in 1.4.0
================

* Add a background&autostart portal
* Add a gamemode portal
* Add a camera portal
* Require pipewire 0.2.6
* inhibit: Track session state
* documents: Fix a ref-counting bug
* screencast: Add cursor modes
* screencast: Memory leak fixes
* Translation updates

Changes in 1.2.0
================

* notification: Use icon validator from flatpak
* notification: Don't leave temp files around
* email: Validate options better
* inhibit: Validate options better
* file chooser: Add support for setting the current filter
* Translation updates

Changes in 1.1.1
================

* Validate icons in notifications
* Respect lockdown settings
* Write back permissions for notifications to indicate portal use
* Set st_nlink in the documents portal
* Add infrastructure for validating options
* Validate email addresses
* Translation updates

Changes in 1.1.0
================

This is the first release in the new unstable 1.1.x series, leading up to 1.2
which is expected around the end of the year.

* Add a location portal, this requires geoclue 2.5.2
* Add a settings portal, for desktop-wide settings like XSettings or kdeglobals
* Allow locking down printing, save-to-disk and opening uris
* Monitor application changes in the open uri portal
* Add more tests

xdg-desktop-portal 1.0.3
========================

* Fix an option name in the remote desktop portal
* document-portal: Validate permissions and report errors
* Fix life-cycle issues with inodes in the document portal
* Improve the test coverage of the documents portal
* Add a 'coverage' make target

xdg-desktop-portal 1.0.2
========================

* networkmonitor: Fix several issues
* inhibit: Add session state monitoring

xdg-desktop-portal 1.0.1
========================

* networkmonitor: Add GetStatus and CanReach methods
* Unset GTK_USE_PORTAL
* Add a portal for moving files to the trash
* Fix an inode leak in the document portal

xdg-desktop-portal 1.0
======================

* screenshot: Add a color picker api
* screencast: Bump the pipewire dependency to 0.2.1
* Improve --help output
* Small documentation improvements

xdg-desktop-portal 0.99
=======================

* The NetworkMonitor portal API has been changed to allow access control
* The Proxy and NetworkMonitor portals only respond to requests from
  sandboxes with network access
* The flatpak portal is now documented

xdg-desktop-portal 0.11
=======================

* Add initial support for Snap packages.
* Fix memory leaks when ownership of bus names changes.
* Include docs for the session, remote desktop and screencast portals.
* document-portal: Be more flexible validating apps' IDs.
* document-portal: Be more strict when checking & granting file access.
* file-chooser: Fix crash with uninitialized data in the save dialog.
* open-uri: Don't ever skip showing the dialog if a threshold is set.
* open-uri: Don't register http: URIs for sandboxed handlers.
* remote-desktop: Use the correct device type values.
* screencast: Fix synchronization issue with PipeWire.

* Translation updates
 Chinese (Taiwan)
 Spanish

xdg-desktop-portal 0.10
=======================

This version of xdg-desktop-portal contains the xdg-document-portal
that used to be shipped by flatpak. The code was moved to
xdg-desktop-portal as a first step towards being used by
snappy. Additionally having the two related portals delivered together
makes it easier to implement new features that rely on changes to
both portals.

The two versions of the document portal are fully compatible, but the
package files will conflict with older versions of flatpak, so
packagers will have to pick one version. Following this there will be
a new release of unstable flatpak with the document portal removed,
and a release of the stable branch (0.10) that has a configure
option to disable the document portal.

Additionally, this release contains a new screencast and remote
desktop portal based on PipeWire.

Major changes in this versions:

 * Import permission store from flatpak
 * Import document portal from flatpak
 * Add remote desktop portal
 * Add screencast portal
 * Add "interactive" mode to screenshot portal
 * file-chooser: Don't return document paths for paths the application has direct access to
 * Handle newer version of bubblewrap
 * New abstraction for application info, supporting multiple sandbox technologies
 * Add basic test suite

xdg-desktop-portal 0.9
======================

* Install pkg-config files into datadir

* Avoid a race in the portal APIs

* Change the email portal to take fds

* Translation updates
 Galicican
 Indonesian
 Turkish


xdg-desktop-portal 0.8
======================

* Update po files

xdg-desktop-portal 0.7
======================

* In the OpenURI portal, send the content-type and filename to the appchooser

* Stop handling file: uris in the OpenURI method of the OpenURI portal

* Add an OpenFile method for local files to the OpenURI portal

* Bug fixes in the notification portal

* Translation updates:
 Czech
 Italian

xdg-desktop-portal 0.6
======================

* A portal for sending email has been added

* The OpenURI portal has be made a bit more permissive.
  It will now directly use the system default for "safe"
  schemes such as http, and local directories.

* Translation updates:
 French

xdg-desktop-portal 0.5
======================

* The notification portal now supports non-exported actions

* An Account portal for basic user information has been added

* All portal interface now have a version property. Currently,
  all interfaces are at version 1

* The file chooser portal was forgetting to make files created
  for SaveFile writable. This has been fixed

* Translation updates:
 Czech
 Polish
 Swedish

xdg-desktop-portal 0.4
======================

* No longer rely on cgroups to find the app id

* Fix handling of mime type filters in the file chooser portal

* Translation updates:
 Chinese
 Czech
 German
 Serbian


xdg-desktop-portal 0.3
======================

* open-uri: Allow configuring threshold

* open-uri: Use fallback applications when needed

* Translation updates:
 Brazilian Portuguese
 Hungarian
 Slovak
 Ukrainian


xdg-desktop-portal 0.2
======================

* Add internationalization support

* Add Qt annotations

* New portal APIs:
 - org.freedesktop.portal.Device

* Translation updates:
 Polish


Initial release 0.1
===================

Included portal APIs:
 * org.freedesktop.portal.FileChooser
 * org.freedesktop.portal.Print
 * org.freedesktop.portal.OpenURI
 * org.freedesktop.portal.Screenshot
 * org.freedesktop.portal.Inhibit
 * org.freedesktop.portal.Notification
 * org.freedesktop.portal.NetworkMonitor
 * org.freedesktop.portal.ProxyResolver
