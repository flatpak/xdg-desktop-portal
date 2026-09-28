# Commit Message and Git Workflow

Good commit messages and git hygiene make code review easier and project history
more useful.

## Commit Message Format

### Structure

```
component: Short summary (50 chars or less)

More detailed explanatory text, if necessary. Wrap at 72 characters.
The blank line separating the summary from the body is critical.

Explain the problem this commit solves. Focus on why you are making
this change as opposed to how. The code explains the how.

If this fixes a bug, reference it:
Fixes: #123
```

### Examples

#### Good Commit Message
```
clipboard: Use new xdp_copy_fd_to_lists helper

This also changes the error handling a bit. If something goes wrong in
the impl call or in the forwarding of the results, then all the client
should know is that there was an internal error. We should log what went
wrong though.
```

#### Another Good Example
```
util: Add the xdp_copy_fd_to_lists helper

Adds a helper to safely copy file descriptors between FD lists,
centralizing validation and error handling.

This will be used by clipboard and other portals that need to forward
file descriptors from backend responses to clients.
```

#### Bug Fix Example
```
sealed-fd: Fix bounds check in fd validation

The previous check was off by one, allowing access to index equal to
the list length. This could cause crashes or security issues.

Fixes: #456
```

## Commit Message Guidelines

### Subject Line
- Start with component name (clipboard, util, usb, etc.)
- Use imperative mood ("Add" not "Added" or "Adds")
- No period at the end
- Keep under 50 characters if possible
- Describe what the commit does, not what you did

### Body
- Wrap at 72 characters
- Explain the why and what, not the how
- Reference issues with "Fixes: #123" or "Closes: #123"
- If this is part of a larger change, explain the relationship

### Common Components
- `xdp` - Generic, use when e.g. lots of components get changed
- `clipboard` - Clipboard portal
- `usb` - USB portal
- `location` - Location portal
- `background` - Background portal
- `notification` - Notification portal
- `file-chooser` - File chooser portal
- `screen-cast` - Screen cast portal
- `remote-desktop` - Remote desktop portal
- `input-capture` - Input capture portal
- `util` - Utility functions
- `session` - Session management
- `request` - Request management
- `document-portal` - Document portal
- `permission-db` - Permission database
- `tests` - Test suite
- `build` - Build system
- `doc` - Documentation

## Commit Organization

### One Logical Change Per Commit
Each commit should represent one logical change when possible:
- ✓ Add a helper function
- ✓ Use the helper function
- ✗ Add helper and refactor entire codebase

### Commit Splitting Example

Instead of one large commit:
```
Refactor clipboard handling and add new feature

- Add helper functions
- Update clipboard portal
- Add tests
- Fix unrelated bug
- Update documentation
```

Split into focused commits:
```
1. util: Add xdp_copy_fd_to_lists helper
2. clipboard: Use new xdp_copy_fd_to_lists helper
3. clipboard: Add support for clipboard metadata
4. tests: Add clipboard metadata tests
5. sealed-fd: Fix bounds check in fd validation
6. doc: Update clipboard portal documentation
```

### Ordering Commits

Order commits logically:
1. Add new utility functions
2. Use them in the codebase
3. Add features that depend on them
4. Add tests
5. Add documentation

This allows incremental review and makes it easier to bisect issues.

## Pull Request Guidelines

### Before Creating PR

- [ ] Commits are logically organized
- [ ] Commit messages follow guidelines
- [ ] Each commit builds successfully
- [ ] Tests pass for each commit
- [ ] No "fixup" or "WIP" commits
- [ ] Branch is rebased on latest main

### PR Description

Include:
- Summary of changes
- Why this change is needed
- Any breaking changes or API additions
- Related issues (Fixes #123)
- Testing performed
- Screenshots for UI changes

Example:
```markdown
## Summary
Adds support for clipboard metadata in the clipboard portal.

## Motivation
Applications need to know the mime types of clipboard content before
requesting the actual data, to avoid unnecessary data transfers.

## Changes
- Add `GetMimeTypes` method to clipboard portal
- Update clipboard backend interface
- Add tests for new functionality

## Testing
- Unit tests added
- Tested with example application
- No regressions in existing tests

Fixes #789
```
