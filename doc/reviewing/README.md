# XDG Desktop Portal - Review Guidelines

This directory contains guidelines for reviewing code contributions to xdg-desktop-portal. These guidelines are designed for both human reviewers and AI agents assisting with code reviews.

## Contents

- [CODING_STYLE.md](CODING_STYLE.md) - Code style and formatting conventions
- [PATTERNS.md](PATTERNS.md) - Common code patterns used in the project
- [GENERAL.md](GENERAL.md) - General code review principles and checklist
- [OWNERSHIP.md](OWNERSHIP.md) - GLib ownership and memory management patterns
- [SECURITY.md](SECURITY.md) - Security-focused review guidelines
- [API_DESIGN.md](API_DESIGN.md) - D-Bus API design and compatibility
- [TESTING.md](TESTING.md) - Testing requirements and guidelines
- [COMMITS.md](COMMITS.md) - Commit message and git workflow standards

## For AI Agents

When reviewing code or assisting with contributions:
- These guidelines are authoritative for this project
- Apply all relevant guidelines from these documents
- Flag violations clearly with file:line references
- Suggest specific fixes with code examples where possible
- Prioritize security and API compatibility concerns
- Consider the broader architectural impact of changes

## Project Context

XDG Desktop Portal is security-critical infrastructure that:
- Mediates access between sandboxed applications and the desktop
- Handles sensitive user data (files, screenshots, location, etc.)
- Exposes stable D-Bus APIs used by many applications
- Runs with elevated privileges in some scenarios

Changes must be reviewed carefully with these considerations in mind.
