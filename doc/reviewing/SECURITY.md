# Security Review Guidelines

XDG Desktop Portal is security-critical infrastructure. It acts as a trusted
intermediary between sandboxed applications and the desktop environment,
handling sensitive operations like file access, screenshots, and location data.

## System Architecture and Threat Model

### The Portal System

XDG Desktop Portal operates in a multi-component security architecture:

```
┌────────────────────────────────────────────────────────────┐
│                    User's Desktop Session                  │
│  ┌────────────┐          ┌──────────────────────────────┐  │
│  │  Desktop   │          │   Backend Implementation     │  │
│  │  Shell     │◄────────►│  (GNOME/KDE/Wlroots/etc.)    │  │
│  │  (Wayland/ │          │                              │  │
│  │   X11)     │          │  - Shows permission dialogs  │  │
│  └────────────┘          │  - Accesses system resources │  │
│                          │  - Takes screenshots         │  │
│                          └──────────────▲───────────────┘  │
│                                         │                  │
│                          ┌──────────────┴───────────────┐  │
│                          │  xdg-desktop-portal          │  │
│   TRUSTED COMPONENT  ──► │  (Portal Frontend)           │  │
│                          │                              │  │
│                          │  - Validates all input       │  │
│                          │  - Enforces permissions      │  │
│                          │  - Manages sessions          │  │
│                          │  - Mediates access           │  │
│                          └──────────────▲───────────────┘  │
└─────────────────────────────────────────┼──────────────────┘
                                          │
                          ════════════════╪════════════════
                          TRUST BOUNDARY  │
                          ════════════════╪════════════════
                                          │
┌─────────────────────────────────────────┼───────────────────┐
│              Sandboxed Application      │                   │
│                                         │                   │
│  UNTRUSTED COMPONENT  ─────────────────►│                   │
│                                         │                   │
│  - Limited filesystem access            │                   │
│  - No direct hardware access            │                   │
│  - Restricted system calls              │                   │
│  - Must use portals for privileged ops  │                   │
│                                         │                   │
│  ┌──────────────────────────────────┐   │                   │
│  │  Application uses D-Bus API:     │   │                   │
│  │  org.freedesktop.portal.Desktop  │   │                   │
│  └──────────────────────────────────┘   │                   │
└─────────────────────────────────────────────────────────────┘
```

### Trust Boundaries

**Trusted Components:**
- **xdg-desktop-portal** (portal frontend) - Validates inputs, enforces policy
- **Backend implementations** - Interact with desktop shell and system resources
- **Desktop environment** - Provides user interface and system access

**Untrusted Components:**
- **Sandboxed applications** - All input from apps is considered hostile
- **D-Bus messages** - All parameters, file descriptors, and options are untrusted

**Critical Insight:** The portal frontend sits at the trust boundary. It receives
untrusted input from sandboxed applications and must validate everything before
forwarding to trusted backends or system resources.

#### Adversary Capabilities

A malicious sandboxed application can:
- **Send arbitrary D-Bus messages** with crafted parameters
- **Pass malformed data** - invalid strings, out-of-range integers, malicious file paths
- **Supply malicious file descriptors** - pointing to unexpected files or devices
- **Attempt resource exhaustion** - create unlimited sessions, send huge arrays
- **Exploit timing** - race conditions, use-after-free via disconnects
- **Attempt confused deputy attacks** - trick portal into acting on wrong resources
- **Probe for information** - learn about system through error messages
- **Attempt to access other apps' data** - sessions, files, permissions

### Security Goals

The portal frontend must ensure:

1. **Isolation Preservation** - Sandbox cannot be escaped through portal APIs
2. **Least Privilege** - Apps get minimum necessary access, nothing more
3. **User Consent** - Sensitive operations require explicit user permission via backend UI
4. **App Separation** - Apps cannot access each other's sessions, files, or data
4. **Host Separation** - Apps cannot access files, or data on the host system
6. **Fail-Safe** - Errors deny access rather than grant it
7. **Information Hiding** - Internal state not leaked through errors or timing

### Defense Requirements

Every portal method must:
- **Validate all inputs** - Never trust client data
- **Verify ownership** - Sessions/requests belong to calling app
- **Check permissions** - App is allowed to perform this operation
- **Sanitize outputs** - Don't leak system information to clients
- **Limit resources** - Prevent DoS attacks
- **Use safe APIs** - Prefer fd-based operations, avoid path-based where possible
- **Handle edge cases** - Client disconnect, concurrent access, invalid state

## Critical Security Checks

### Input Validation

#### String Parameters
```c
/* Validate app IDs */
if (!xdp_is_valid_app_id (app_id))
  {
    g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                           XDG_DESKTOP_PORTAL_ERROR,
                                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                           "Invalid app ID");
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }

/* Validate tokens (session IDs, etc.) */
if (!xdp_is_valid_token (token))
  {
    g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                           XDG_DESKTOP_PORTAL_ERROR,
                                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                           "Invalid token");
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }

/* Always check for NULL strings */
if (param == NULL || *param == '\0')
  {
    g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                           XDG_DESKTOP_PORTAL_ERROR,
                                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                           "Missing required parameter");
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }
```

#### File Descriptors
```c
/* ALWAYS validate FD list indices */
if (!xdp_is_fd_list_index_valid (fd_list, fd_id))
  {
    g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                           XDG_DESKTOP_PORTAL_ERROR,
                                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                           "Invalid file descriptor");
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }

/* Use helper to get FDs safely */
g_auto(XdpFd) fd = xdp_get_portal_call_fd (fd_list, fd_id, &error);
if (fd == -1)
  {
    g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }
```

#### Numeric Parameters
```c
/* Check ranges */
if (width <= 0 || width > MAX_WIDTH ||
    height <= 0 || height > MAX_HEIGHT)
  {
    g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                           XDG_DESKTOP_PORTAL_ERROR,
                                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                           "Invalid dimensions");
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }

/* Watch for integer overflow */
if (count > G_MAXINT / sizeof (Item))
  {
    g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                           XDG_DESKTOP_PORTAL_ERROR,
                                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                           "Count too large");
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }

/* Validate array/list lengths */
size_t n_items;
const char **items = g_variant_get_strv (variant, &n_items);
if (n_items == 0 || n_items > MAX_ITEMS)
  {
    g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                           XDG_DESKTOP_PORTAL_ERROR,
                                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                           "Invalid number of items");
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }
```

#### Options Dictionaries
```c
/* Use xdp_filter_options to validate */
static XdpOptionKey supported_options[] = {
  { "key1", G_VARIANT_TYPE_STRING, validate_key1 },
  { "key2", G_VARIANT_TYPE_BOOLEAN, NULL },
};

g_auto(GVariantBuilder) options_builder =
  G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

if (!xdp_filter_options (options_in,
                         &options_builder,
                         supported_options,
                         G_N_ELEMENTS (supported_options),
                         NULL,
                         &error))
  {
    g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), error);
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }
```

### Permission Checks

#### Session Association
```c
/* Verify session belongs to calling app */
if (!xdp_session_is_owned_by (session, app_info))
  {
    g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                           XDG_DESKTOP_PORTAL_ERROR,
                                           XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                           "Invalid session");
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }

/* Check session state */
if (!session_is_in_valid_state (session))
  {
    g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                           XDG_DESKTOP_PORTAL_ERROR,
                                           XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                           "Invalid session state");
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }
```

#### Permission Checks
```c
/* Check stored permissions */
XdpPermissionStore *store = get_permission_store ();
if (!xdp_permission_store_lookup (store,
                                  "permission-table",
                                  app_id))
  {
    g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                           XDG_DESKTOP_PORTAL_ERROR,
                                           XDG_DESKTOP_PORTAL_ERROR_NOT_ALLOWED,
                                           "Permission denied");
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }
```

### Command Injection Prevention

#### Spawning Processes
```c
// NEVER use system() or unvalidated strings in commands

// BAD - command injection vulnerability
g_autofree char *cmd = g_strdup_printf ("somecommand %s", user_input);
system (cmd);

// GOOD - use argument arrays
const char *argv[] = { "somecommand", user_input, NULL };
g_spawn_async (NULL, (char **)argv, NULL, G_SPAWN_DEFAULT,
               NULL, NULL, NULL, &error);

// Or use xdp_spawn helper
g_autofree char *output = xdp_spawn (&error, "somecommand", user_input, NULL);

// If you must build complex commands, properly quote
g_autofree char *quoted = g_shell_quote (user_input);
```

### Information Disclosure

#### Error Messages
```c
/* BAD - leaks system information */
g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                       G_FILE_ERROR,
                                       g_file_error_from_errno (errno),
                                       "Failed to open %s: %s",
                                       internal_path, g_strerror (errno));

/* GOOD - log internally, return generic error */
g_warning ("Failed to open %s: %s", internal_path, g_strerror (errno));
g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                       XDG_DESKTOP_PORTAL_ERROR,
                                       XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                       "Failed to open file");
```

#### Backend Responses
```c
/* BAD - forwards backend error directly */
g_dbus_method_invocation_return_gerror (g_steal_pointer (&invocation), backend_error);

/* GOOD - validates and sanitizes */
if (backend_error)
  {
    g_warning ("Backend error: %s", backend_error->message);
    g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                           XDG_DESKTOP_PORTAL_ERROR,
                                           XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                           "Operation failed");
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }
```

### Resource Limits

#### Prevent Resource Exhaustion
```c
/* Limit array sizes from clients */
if (n_items > MAX_REASONABLE_ITEMS)
  {
    g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                           XDG_DESKTOP_PORTAL_ERROR,
                                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                           "Too many items");
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }

/* Limit string lengths */
if (strlen (input) > MAX_REASONABLE_LENGTH)
  {
    g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                           XDG_DESKTOP_PORTAL_ERROR,
                                           XDG_DESKTOP_PORTAL_ERROR_INVALID_ARGUMENT,
                                           "Input too long");
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }

/* Limit active sessions per app */
if (get_session_count_for_app (app_info) >= MAX_SESSIONS_PER_APP)
  {
    g_dbus_method_invocation_return_error (g_steal_pointer (&invocation),
                                           XDG_DESKTOP_PORTAL_ERROR,
                                           XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                           "Too many active sessions");
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }
```

## Common Vulnerabilities to Check For

### 1. Path Traversal
- [ ] Do not use any path that might be attacker controlled
- [ ] Always use `glnx_chaseat` and similar fd-based functions

### 2. Integer Overflow
- [ ] Array allocations check for overflow
- [ ] Size calculations validated before allocation
- [ ] Indices checked against actual array sizes

### 3. Buffer Overflows
- [ ] String operations use bounded versions (strncpy, snprintf)
- [ ] Array accesses are bounds-checked
- [ ] Fixed-size buffers have sufficient space

### 4. Use After Free
- [ ] Objects not used after unref
- [ ] Signal handlers disconnected before object destruction
- [ ] Weak references used where appropriate

### 5. Information Disclosure
- [ ] Error messages don't leak system paths or internal state
- [ ] Debug logging doesn't expose sensitive data
- [ ] Temporary files have restrictive permissions

### 6. Privilege Escalation
- [ ] Apps can't access other apps' sessions or data
- [ ] Permissions are checked before granting access
- [ ] Session ownership is validated

### 7. Denial of Service
- [ ] Resource limits prevent exhaustion
- [ ] Infinite loops and recursive calls prevented
- [ ] Client disconnect cleanup prevents resource leaks

## Reporting Security Issues

Security vulnerabilities should be reported privately, not in public issues.
See the project security policy for details.
