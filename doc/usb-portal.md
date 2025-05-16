# USB portal

The USB portal is the middleman between sandboxed apps, and the
devices connected and available to the host system. This is the first
version of the portal.

This part was implemented by

- Hubert Figuière
- Georges Basile Stavracas Neto

funded by the STF (Sovereign Tech Fund) effort, based upon initial
work by

- Ryan Gonzalez

and with help from other contributors.

## Device filtering

Sandboxed apps must declare which USB devices they support ahead of
time. This information is read by the XDG Desktop Portal and used to
determine which USB devices will be exposed to requesting apps. On
Flatpak, these enumerable and hidden devices are set by the `--usb`
and `--nousb` arguments against `flatpak build-finish` and `flatpak
run`. Neither `--devices=all` nor `--device=usb` do influence the
portal.

Hiding a device always take precedence over making them enumerable,
even when a blanket permission (`--usb=all`) is set.

However out of the sandbox we assume all devices are allowed as there
is no restriction that can prevent seeing anything.

Individual devices are assigned a unique identifier by the portal,
which is used for all further interactions. This unique identifier is
completely random and independent of the device. Permission checks are
in place to not allow apps to try and guess device ids without having
permission to access then.

## Permissions

There are 2 dynamic permissions managed by the USB portal in the
permission store:

1. Blanket USB permission: per-app permission to use any methods of
the USB portal. Without this permission, apps must not be able to do
anything - enumerate, monitor, or acquire - with the USB portal.[^1]

2. Specific device permission: per-app permission to acquire a
specific USB device, down to the serial number.

## Enumerating devices

There are 2 ways for apps to learn about devices:

- Apps can call the EnumerateDevices() method, which gives a snapshot
of the current devices to the app.

- Apps can create a device monitoring session with CreateSession()
which sends the list of available devices on creation, and also
notifies the app about connected and disconnected devices.

Only devices that the app is allowed to see are reported in both
cases.

The udev properties exposed by device enumeration is limited to a
well known subset of properties.[^2]

## Device acquisition & release

Once an app has determined which devices it wants to access, the app
can call the AcquireDevices() method. This method may prompt a dialog
for the user to allow or deny the app from accessing specific devices.

If permission is granted, XDG Desktop Portal tries to open the device
file on the behalf of the requesting app, and pass down the file
descriptor to that file.[^3]

The caller must then call FinishAcquireDevices() until it indicate it
is finished. It's only necessary to call it more than once if there
are too many file descriptors to return. This is a D-Bus
limitation. Check the `finished` return argument.

### Using the file descriptor

The file descriptors returned by the portal are meant to be used with
USB library you use.

In the case of libusb 1.0.23 and later, use `libusb_wrap_sys_device()`
and pass the file descriptor as the `sys_dev` argument.  It should be
noted that libusb must be compiled with udev support (it is the
default) in order to be able to work. Without this it looks for the
nonexistent `/dev/usb` that is not present.

If you use libhidapi, you need to use the function
`hid_libusb_wrap_sys_device()` provided by libhidapi-usb.

---

[^1]: Exceptionally, apps can release previously acquired devices,
even when this permission is disabled. This is so because we don't yet
have kernel-side USB revoking. With USB revoking in place, it would be
possible to hard-cut app access right when the app permission changes.

[^2]: This patch uses a hardcoded list. There is no mechanism for apps
to influence which other udev properties are fetched. This approach is
open to suggestions - it may be necessary to expose more information
more liberally through the portal. The initial attempt has been made
to provide sensible information useful for display in a user
interface.

[^3]: This is clearly not ideal. The ideal approach is to go through
logind's TakeDevice() method. However, that will add significant
complexity to the portal, since this logind method can only be called
by the session controller (i.e. the only executable capable of calling
TakeControl() in the session - usually the compositor).  This can and
probably should be implemented in a subsequent round of improvements
to the USB portal.
