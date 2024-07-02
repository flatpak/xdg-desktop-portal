Documents & FUSE
================

The Document portal exposes files (filesystem entries) to applications through
FUSE.

Files are scoped in a domain operations are restricted with a domain. The FUSE
filesystem will proxy the file access by referencing the inodes and presenting
the files in a virtual filesystem.

Much like with xdg-desktop-portal, to run the development version of
the Document portal, do:

.. code-block:: shell

   _build/document-portal/xdg-document-portal --replace

FUSE
----

The Document portal is a separate executable, ``xdg-document-portal``, and it
implements the virtual filesystem using FUSE. The filesystem is mounted in the
user runtime ``doc`` directory. It usually is ``/run/user/[UID]/doc``, where
``[UID]`` is the user id on the system.

Inside the Flatpak sandbox, files for which the app has permission to access are
available at ``/run/flatpak/doc``.

Like any FUSE filesystem, it exposes inodes through the FUSE API.

Domains
"""""""

Domains specify the scope of an inode and organize the documents into
a hierarchy. It helps compartimentalize which documents the
applications have access to.

- ``XDP_DOMAIN_ROOT``: root domain.
- ``XDP_DOMAIN_BY_APP``: used to find app domains by app id.
- ``XDP_DOMAIN_APP``: contains the documents allowed for the app.
- ``XDP_DOMAIN_DOCUMENT``: a physical file or directory is a document
  in the ``XDP_DOMAIN_DOCUMENT``.

The first three are considered to be virtual, which mean they are not
backed by a physical inode.

The Document portal defines a few globals to hold the inodes:

- ``root_inode`` is an inode that holds the root domain.
- ``by_app_inode`` is the inode for the by app domain.
- ``physical_inodes`` is a hash table for the physical inodes, the key
  is ``devino``.
- ``all_inodes`` is a hash table for all the inodes, the key is the
  inode number.

The domains are used to create the filesystem hierarchy presented by
the document portal through the FUSE filesystem. It looks like this:

.. code-block::

   /
    ├─ by-app
    │   ├─ org.foo.Bar
    │   │   ├─ doc1
    │   │   └─ doc3
    │   └─ org.bar.Foo
    │       ├─ doc2
    │       └─ doc3
    ├─ doc1
    ├─ doc2
    └─ doc3

``/`` is the root domain. ``by-app`` is the ``XDP_DOMAIN_BY_APP`` domain.
``org.foo.Bar`` and ``org.bar.Foo`` are application ids.

``doc1``, ``doc2``, ``doc3`` are unique identifiers for the documents. As
mentioned, they can be shared across apps.

Document identifiers are unique. They are created using the inode number of the
physical file or directory.

Custom xattrs
-------------

Files and folders mounted through the Document portal have a custom attribute
with the host system path: ``user.document-portal.host-path``. This attribute
is read-only and any attempt to modify it will result in an error.