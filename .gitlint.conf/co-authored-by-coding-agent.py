# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: Copyright © the xdg-desktop-portal contributors

from gitlint.rules import CommitRule, RuleViolation

CODING_AGENTS = [
    "Co-authored-by: Claude <noreply@anthropic.com>",
    "Co-authored-by: Copilot <copilot@github.com>",
]


class CoAuthoredByCodingAgent(CommitRule):
    """Try to stop people from adding Co-Authored-By: AGENT_NAME
    and instead use the kernel convention of the Assisted-by tag.
    """

    name = "assisted-by-not-co-authored-by"
    id = "UC1"

    msg = (
        "Body contains a 'Co-Authored-By: AGENT_NAME' line. "
        "Use 'Assisted-by: AGENT_NAME:MODEL_VERSION' instead."
    )

    def validate(self, commit):
        for line in commit.message.body:
            for agent in CODING_AGENTS:
                if line.lower().startswith(agent.lower()):
                    return [RuleViolation(self.id, self.msg, line_nr=1)]
