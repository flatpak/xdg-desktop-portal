Versionning
===========

D−Bus Interfaces
----------------

D-Bus interfaces are versionned to:

- Enable non-backward compatible changes (e.g. remove deprecated functions,
  interface overhaul) with major bump.
- Enable backward compatible changes (e.g. method addition, new property for
  the optional argument) with minor bump.

Two major version of an interface can coexist to allow a deprecation period for
the former version.

The major version is determined by the main interface name (to not mix up with
the deprecated ``version`` property in interfaces documentation).

.. csv-table:: Major Version Example
    :header: "Interface Name", "Version", "Reason"

    "org.freedesktop.portal.Settings", "1", "Lack of number suffix, default to 1"
    "org.freedesktop.portal.Camera2", "2", "Suffix number is 2"

Interfaces have a minor version documented as "revision" (previously
"version" but renamed to avoid confusion), assume 1 if not documented.

Each of those interfaces must have a ``active-revision`` property which is not
meant to match the documented "revision" but the revision matching the
feature-set enabled, if missing or not set (returns 0) check the interface
documentation of the property for the recommended fallback or assumption to do.

The ``version`` property has been deprecated and is not guaranted to be identical
to the new ``active-revision`` for every interface.

Implementation interfaces
-------------------------

Portal implementation interfaces are also versionned (and can have revisions) but
do not require to match their non-implementation counterpart.

When an implementation provides an interfaces, it is expected that the feature-set
behind the set ``active-revision`` is implemented, if missing or not set (returns
0\) check the interface documentation of the property for the matching fallback
behaviour that will be used.
