# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

import re
from gitlint.rules import BodyMaxLineLength

TAGS = [
    "Signed-off-by",
    "Co-authored-by",
    "Fixes",
    "Closes",
]


class CustomBodyMaxLineLength(BodyMaxLineLength):
    name = "custom-body-max-line-length"
    id = "UC2"

    def validate(self, line, commit):
        # Ignore quoted content
        if line.startswith(" " * 4):
            return None

        # Ignore reference lines (e.g. [2]: https://example.org/foobar)
        ret = re.match(r"^\[\d+\] ", line)
        if ret is not None:
            return None

        # Ignore length for tags
        for tag in TAGS:
            tag = tag + ": "
            if line[: len(tag)].lower() == tag.lower():
                return None

        # Otherwise behave as the upstream BodyMaxLineLength rule
        return super().validate(line, commit)
