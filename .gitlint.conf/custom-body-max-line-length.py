# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

import re
from gitlint.rules import BodyMaxLineLength


class CustomBodyMaxLineLength(BodyMaxLineLength):
    name = "custom-body-max-line-length"
    id = "UC2"

    def validate(self, line, commit):
        # Ignore quoted content
        if line.startswith(" " * 4):
            return None

        # Ignore reference lines, such as:
        #
        #     [2]: https://example.org/foobar
        #     Closes: #XYZ
        #     Co-authored-by: Example <foo@example.com>
        #     …
        #
        # without ignoring lower-case letters.
        if re.match(r"^([A-Z]|\[\d+\])\S*: ", line):
            return None

        # Otherwise behave as the upstream BodyMaxLineLength rule
        return super().validate(line, commit)
