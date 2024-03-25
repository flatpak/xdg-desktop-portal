Portal internals
================

This document will attempt to explain the internals of the portal to
answer both "How does it work?" and to explain the code, and it also
tries to provide guidance to contributors.


Document portal
---------------

The goal of the document portal is to expose files (filesystem
entries) to application through FUSE. Files are scoped in a domain
operations are restricted with a domain. The FUSE filesystem will
proxy the file access by referencing the inodes and presenting the
files in a virtual filesystem.

The document portal is a separate executable, ``xdg-document-portal``,
and it implements the virtual filesystem using FUSE.

The filesystem is mounted in the user runtime ``doc`` directory. It's
usually ``/run/user/UID/doc``, where ``UID`` is the user id on the
system.

Inside the Flatpak sandbox, the allowed documents are available at
``/run/flatpak/doc``.

Running
^^^^^^^

Much like with xdg-desktop-portal, to run the development version of
the document portal you just build, do:

.. code-block:: shell

   _build/document-portal/xdg-document-portal --replace


Internals
^^^^^^^^^

The implementation of the FUSE filesystem exposed for the file portal
is based on several concepts.

Like any FUSE filesystem, it exposes inodes through the FUSE API.

Domains
"""""""

Domains specify the scope of an inode and organize the documents into
a hierarchy. It helps compartimentalize which documents the
applications have access to.

- ``XDP_DOMAIN_ROOT``: The root domain is used as a top level.
- ``XDP_DOMAIN_BY_APP``: The by app domain is used to find app domains
  by app-id.
- ``XDP_DOMAIN_APP``: This is the domain for the app. It contains the
  documents allowed for the app.
- ``XDP_DOMAIN_DOCUMENT``: A physical file or directory is a document
  in the ``XDP_DOMAIN_DOCUMENT``.

The first three are considered to be virtual, which mean they are not
backed by a physical inode.

The document portal defines a few globals to hold the inodes:

- ``root_inode`` is an inode the hold the root domain.
- ``by_app_inode`` is the inode for the by app domain.
- ``physical_inodes`` is a hash table for the physical inodes, the key
  is ``devino``.
- ``all_inodes`` is a hash table for all the inodes, the key is the
  inode number.


FUSE
""""

The document portal implement a virtual filesystem in user space to
expose the compartimentalized access to documents.

The domains are used to create the filesystem hierarchy presented by
the document portal through the FUSE filesystem.

It looks like this:

* ``/``
    * ``by-app``
        * *app-id1*
            * *doc1*
            * *doc3*
        * *app-id2*
            * *doc2*
            * *doc3*
    * *doc1*
    * *doc2*
    * *doc3*

``by-app`` is the ``XDP_DOMAIN_BY_APP``.

*add-id1*, *app-id2* are applications.

*doc1*, *doc2*, *doc3* are unique identifiers for the documents. As
 shown, there can be shared accross apps.

Document identifiers are created uniquely. As an implementation detail
they are created using the inode number of the physical file or
directory.

Inods
"""""

Ownership model


