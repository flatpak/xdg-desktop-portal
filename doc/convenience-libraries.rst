Convenience Libraries
=====================

Using the XDG Portals D-Bus APIs directly is often difficult and error-prone.
Fortunately, there are convenience libraries available that significantly ease
the development of apps:

* `ASHPD <https://bilelmoussaoui.github.io/ashpd/ashpd/>`_: a **Rust** crate that
  provides the APIs to interact with portals in idiomatic Rust. It has support for
  GTK4, direct X11 windows, and direct Wayland surfaces.
* `libportal <https://github.com/flatpak/libportal/>`_: small **C** library that
  provides a GObject API to interact with portals. It provides language bindings
  to a variety of other languages, such as **Python**, **JavaScript**, **Vala**,
  and more. It has support for GTK3, GTK4, Qt 5, and Qt 6.
* `portal <https://github.com/rymdport/portal>`_: a **Go** module that provides
  native APIs for interacting with portals from idiomatic Go code.
  It aims to be both toolkit agnostic and easy to use.
* `xdg_desktop_portal <https://pub.dev/packages/xdg_desktop_portal>`_: a native
  **Dart** package to interact with portals in **Dart** and **Flutter**.
