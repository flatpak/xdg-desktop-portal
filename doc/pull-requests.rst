..
   SPDX-License-Identifier: LGPL-2.1-or-later
   SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

Pull Requests & Issues
======================

XDG Desktop Portal uses GitHub to
`discuss new portals and features <https://github.com/flatpak/xdg-desktop-portal/discussions>`_,
`track issues <https://github.com/flatpak/xdg-desktop-portal/issues>`_ and
`take contributions <https://github.com/flatpak/xdg-desktop-portal/pulls>`_.

Please be kind and patient as code reviews can be long and be minutious, issue
triage takes effort and everyone has limited time and resources.

Before developing features or fixing bugs, please make sure you have done the
following:

- Your code is not on the ``main`` branch of your fork.
- The code has been tested.
- All commit messages are properly formatted and commits squashed where
  appropriate.
- You have included updates to all appropriate documentation.

Commits
-------

The project maintains a clean history of commits without merge commits on main
and stable branches. Every commit should be buildable and passing all the tests.
Commits should be broken down into chunks of reviewable, independent changes.
This can be achieved by squashing, resetting and then committing chunks of the
work, as well as interactively rebasing branches which are already split into
reasonable commits.

Commit Messages
---------------

Commit messages generally follow the
`GNOME Commit Message guidelines <https://handbook.gnome.org/development/commit-messages.html>`_,
but ``gitlint`` enforces specific formatting. Enable the
:ref:`git hooks <building-and-running>` to ensure that your commit messages
will pass CI.

It can help to run ``git log ./path/to/folder/or/file`` to get an idea about
the right prefix for a specific commit.

It can make sense to create git aliases to make creating certain tags easier:

.. code-block::

    [alias]
      # Generate the fixes tag for a specific commit: `git fixes $commit`
      fixes = show -s --pretty='format:Fixes: %h (\"%s\")'


Continuous Integration
----------------------

The CI will automatically run when creating or pushing to a PR. To avoid
waiting for the results, fixing detected issues locally, and then pushing
again, run the tests locally and enable the git hooks.

If changes to the check or build OCI images are required, the image tag
in ``.github/workflows/main.yml`` needs to be bumped for CI to update the
image. The github action might need to succeed on your own fork for CI to
pick it up. In that case amending the last commit and force pushing can be
used to trigger a new CI run.
