# This file is built into the GDbus sources but does not get installed by us
flatpak_intf_sources = []
if flatpak_intf_dir != ''
  flatpak_intf_sources = files(flatpak_intf_dir / 'org.freedesktop.portal.Flatpak.xml')
endif

portal_sources = files(
  'org.freedesktop.portal.Account.xml',
  'org.freedesktop.portal.Background.xml',
  'org.freedesktop.portal.Camera.xml',
  'org.freedesktop.portal.Clipboard.xml',
  'org.freedesktop.portal.Documents.xml',
  'org.freedesktop.portal.DynamicLauncher.xml',
  'org.freedesktop.portal.Email.xml',
  'org.freedesktop.portal.FileChooser.xml',
  'org.freedesktop.portal.FileTransfer.xml',
  'org.freedesktop.portal.GameMode.xml',
  'org.freedesktop.portal.GlobalShortcuts.xml',
  'org.freedesktop.portal.Inhibit.xml',
  'org.freedesktop.portal.InputCapture.xml',
  'org.freedesktop.portal.Location.xml',
  'org.freedesktop.portal.MemoryMonitor.xml',
  'org.freedesktop.portal.NetworkMonitor.xml',
  'org.freedesktop.portal.Notification.xml',
  'org.freedesktop.portal.OpenURI.xml',
  'org.freedesktop.portal.PowerProfileMonitor.xml',
  'org.freedesktop.portal.Print.xml',
  'org.freedesktop.portal.ProxyResolver.xml',
  'org.freedesktop.portal.Realtime.xml',
  'org.freedesktop.portal.RemoteDesktop.xml',
  'org.freedesktop.portal.Request.xml',
  'org.freedesktop.portal.ScreenCast.xml',
  'org.freedesktop.portal.Screenshot.xml',
  'org.freedesktop.portal.Secret.xml',
  'org.freedesktop.portal.Session.xml',
  'org.freedesktop.portal.Settings.xml',
  'org.freedesktop.portal.Speech.xml',
  'org.freedesktop.portal.Trash.xml',
  'org.freedesktop.portal.Usb.xml',
  'org.freedesktop.portal.Wallpaper.xml',
)

portal_host_sources = files(
  'org.freedesktop.host.portal.Registry.xml',
)

portal_impl_sources = files(
  'org.freedesktop.impl.portal.Access.xml',
  'org.freedesktop.impl.portal.Account.xml',
  'org.freedesktop.impl.portal.AppChooser.xml',
  'org.freedesktop.impl.portal.Background.xml',
  'org.freedesktop.impl.portal.Clipboard.xml',
  'org.freedesktop.impl.portal.DynamicLauncher.xml',
  'org.freedesktop.impl.portal.Email.xml',
  'org.freedesktop.impl.portal.FileChooser.xml',
  'org.freedesktop.impl.portal.GlobalShortcuts.xml',
  'org.freedesktop.impl.portal.Inhibit.xml',
  'org.freedesktop.impl.portal.InputCapture.xml',
  'org.freedesktop.impl.portal.Lockdown.xml',
  'org.freedesktop.impl.portal.Notification.xml',
  'org.freedesktop.impl.portal.PermissionStore.xml',
  'org.freedesktop.impl.portal.Print.xml',
  'org.freedesktop.impl.portal.RemoteDesktop.xml',
  'org.freedesktop.impl.portal.Request.xml',
  'org.freedesktop.impl.portal.ScreenCast.xml',
  'org.freedesktop.impl.portal.Screenshot.xml',
  'org.freedesktop.impl.portal.Secret.xml',
  'org.freedesktop.impl.portal.Session.xml',
  'org.freedesktop.impl.portal.Settings.xml',
  'org.freedesktop.impl.portal.Usb.xml',
  'org.freedesktop.impl.portal.Wallpaper.xml',
)

background_monitor_sources = files(
  'org.freedesktop.background.Monitor.xml',
)

install_data([portal_sources, portal_host_sources, portal_impl_sources],
    install_dir: datadir / 'dbus-1' / 'interfaces',
)
