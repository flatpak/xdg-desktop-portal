# Code Patterns - Transitioning to Better Practices

This document describes patterns that are being actively updated in the codebase.
Old code still exists using deprecated patterns, but new code and modifications
should use the new patterns.

## Variable Initialization

### OLD Pattern: Complex logic in initialization

```c
g_autoptr(GTask) task = g_task_new (object, NULL, set_status_finished_cb, NULL);
```

**Problems:**
- Easy to miss complex logic
- Often makes lines too long

### NEW Pattern: Trivial initialization only

```c
XdpAppInfo *app_info = xdp_invocation_get_app_info (invocation);
g_autoptr(GTask) task = NULL;
g_auto(GVariantBuilder) opt_builder =
  G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

task = g_task_new (object, NULL, set_status_finished_cb, NULL);
```

**Why change:**
- Variables stay initialized
- No complex logic to miss in variable declaration

## File Descriptor Handling

### OLD Pattern: Direct FD List Access

```c
/* OLD - still exists in some files */
int fd = g_unix_fd_list_get (fd_list, fd_id, &error);
if (fd < 0)
  {
    /* ... */
  }
/* Must manually close fd later */
close (fd);
```

**Problems:**
- No bounds checking before access
- No validation that fd_id is valid index
- Manual close required, easy to leak on error paths
- Inconsistent error handling

### NEW Pattern: Use Helper Functions

```c
/* NEW - being adopted across codebase */
/* automatically checks fd_id is in bound */
g_autofd int fd = xdp_get_portal_call_fd (fd_list, fd_id, error);
if (fd < 0)
  return FALSE;

/* Use fd... */
/* Automatically closed at scope exit */
```

**Why change:**
- Security: prevents out-of-bounds access
- Safety: automatic cleanup prevents leaks
- Consistency: same pattern everywhere
- Validation: centralized checks

**Recent commits adopting this:**
- `e743575c sealed-fd: Use the new xdp_is_fd_list_index_valid helper`
- `43c049ee util: Add the xdp_get_portal_call_fd helper`
- `58d128ce xdp: Use the xdp_get_portal_call_fd helper`

### Copying FDs Between Lists

```c
/* OLD - manual copying with error-prone handling */
int src_fd = g_unix_fd_list_get (src_list, src_id, &error);
if (src_fd < 0)
  return FALSE;

int dst_id = g_unix_fd_list_append (dst_list, src_fd, &error);
close (src_fd);
if (dst_id < 0)
  return FALSE;

/* NEW - use helper */
int dst_id;
if (!xdp_copy_fd_to_lists (src_list, dst_list, src_id, &dst_id, error))
  {
    g_warning ("Failed to copy FD: %s", error->message);
    return FALSE;
  }
```

**Recent commits:**
- `354a25ca util: Add the xdp_copy_fd_to_lists helper`
- `f03dd21b clipboard: Use new xdp_copy_fd_to_lists helper`

## Memory Management and Ownership

### OLD Pattern: Manual Memory Management

```c
/* OLD - still exists in older code */
GVariant *variant = get_variant ();
GError *error = NULL;
char *str = g_strdup ("text");

/* ... code with multiple return paths ... */

if (some_condition)
  {
    g_variant_unref (variant);
    g_clear_error (&error);
    g_free (str);
    return FALSE;  /* Must remember to free */
  }

/* ... */

g_variant_unref (variant);
g_clear_error (&error);
g_free (str);
return TRUE;
```

**Problems:**
- Memory leaks in error paths
- Duplicate cleanup code
- Easy to forget cleanup
- Hard to review

### NEW Pattern: Automatic Cleanup

```c
/* NEW - prefer this pattern */
g_autoptr(GVariant) variant = get_variant ();
g_autoptr(GError) error = NULL;
g_autofree char *str = g_strdup ("text");

/* ... code with multiple return paths ... */

if (some_condition)
  return FALSE;  /* Automatic cleanup */

/* ... */

return TRUE;  /* Automatic cleanup */
```

**Why change:**
- No memory leaks in error paths
- Less code, easier to review
- Compiler helps ensure correctness
- Ownership is self-documenting

### Ownership Transfer Without g_steal_pointer

```c
/* OLD - ownership transfer unclear */
Session *session = create_session ();
register_session (registry, session);
/* Does registry own session? Will it be freed twice? */
```

**Problems:**
- Unclear if ownership transferred
- Can't tell from code if session will be freed twice
- Reader must check register_session implementation

### NEW Pattern: Explicit Ownership Transfer

```c
/* NEW - ownership transfer is explicit */
g_autoptr(Session) session = create_session ();
register_session (registry, g_steal_pointer (&session));
/* session is NULL now, registry owns it - clear from code */
```

**Why change:**
- Ownership transfer is explicit
- g_steal_pointer makes intent clear
- session set to NULL prevents use-after-free
- Self-documenting code

## Variable Scoping

### OLD Pattern: Function-Scope Variables

```c
/* OLD - still common in older portals */
static void
process_items (GPtrArray *items)
{
  GError *error = NULL;
  GVariant *result = NULL;
  Item *item = NULL;
  size_t i;

  for (i = 0; i < items->len; i++)
    {
      item = g_ptr_array_index (items, i);
      result = process_item (item, &error);

      if (!result)
        {
          g_warning ("Failed: %s", error->message);
          g_clear_error (&error);
          continue;
        }

      use_result (result);
      g_clear_pointer (&result, g_variant_unref);
    }
}
```

**Problems:**
- Variables live longer than needed
- Can accidentally use stale values
- More manual cleanup required
- Harder to see when values are valid

### NEW Pattern: Block/Loop-Scope Variables

```c
/* NEW - declare variables at natural scope */
static void
process_items (GPtrArray *items)
{
  for (size_t i = 0; i < items->len; i++)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GVariant) result = NULL;
      Item *item;

      item = g_ptr_array_index (items, i);
      result = process_item (item, &error);

      if (!result)
        {
          g_warning ("Failed: %s", error->message);
          continue;
        }

      use_result (result);
      /* error and result automatically cleaned up each iteration */
    }
}
```

**Why change:**
- Variables only exist when needed
- Automatic cleanup per iteration
- Can't accidentally reuse old values
- Clear lifetime boundaries

### Custom Scopes for Clarity

```c
/* NEW pattern - use explicit scopes */
static void
prepare_and_execute (void)
{
  g_autoptr(ExecutionContext) context = NULL;

  /* Temporary objects only needed for setup */
  {
    g_autoptr(Config) config = load_config ();
    g_autoptr(GVariant) options = build_options ();

    context = create_context (config, options);
    /* config and options freed here, can't be used later */
  }

  /* Only context remains */
  execute (context);
}
```

## Control Flow

### OLD Pattern: Nested if/else Blocks

```c
/* OLD - nested conditionals with success in else branch */
static gboolean
process_request (Request *request, GError **error)
{
  if (validate_request (request))
    {
      if (check_permissions (request))
        {
          if (perform_operation (request, error))
            {
              return TRUE;
            }
          else
            {
              g_set_error (error, ...);
              return FALSE;
            }
        }
      else
        {
          g_set_error (error, ...);
          return FALSE;
        }
    }
  else
    {
      g_set_error (error, ...);
      return FALSE;
    }
}
```

**Problems:**
- Deep nesting reduces readability
- Success path is buried in nested blocks
- Harder to follow the main logic flow
- More indentation means longer lines

### NEW Pattern: Early Exits for Errors

```c
/* NEW - early exits for errors, main path stays left */
static gboolean
process_request (Request *request, GError **error)
{
  if (!validate_request (request))
    {
      g_set_error (error, ...);
      return FALSE;
    }

  if (!check_permissions (request))
    {
      g_set_error (error, ...);
      return FALSE;
    }

  if (!perform_operation (request, error))
    return FALSE;

  return TRUE;
}
```

**Why change:**
- Success path remains at the same indentation level
- Error conditions exit early and immediately
- Main logic flow is easier to follow
- Reduced nesting improves readability
- Less indentation allows longer variable names

**Key principle:** Handle errors first and return early. Keep the success case as the main path through the function.

## Error Reporting to Clients

### OLD Pattern: Detailed Error Messages

```c
/* OLD - still exists in some places */
if (!g_file_get_contents (path, &contents, NULL, &error))
  {
    g_dbus_method_invocation_return_error (invocation,
                                           G_FILE_ERROR,
                                           g_file_error_from_errno (errno),
                                           "Failed to open %s: %s",
                                           path, error->message);
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }
```

**Problems:**
- Leaks internal paths to sandboxed apps
- Exposes system information
- Can aid attackers in probing system
- Inconsistent error messages

### NEW Pattern: Generic Errors, Detailed Logs

```c
/* NEW - log details internally, return generic error */
if (!g_file_get_contents (path, &contents, NULL, &error))
  {
    /* Log with full details for debugging */
    g_warning ("Failed to open %s: %s", path, error->message);

    /* Return generic error to client */
    g_dbus_method_invocation_return_error (invocation,
                                           XDG_DESKTOP_PORTAL_ERROR,
                                           XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                           "Failed to open file");
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }
```

**Why change:**
- Security: don't leak system information
- Consistency: all errors use XDG_DESKTOP_PORTAL_ERROR
- Debugging: full details still logged
- Privacy: client doesn't learn about system layout

### Backend Errors

```c
/* OLD - forward backend error directly */
if (!backend_call_finish (..., &error))
  {
    g_dbus_method_invocation_return_gerror (invocation, error);
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }

/* NEW - sanitize backend errors */
if (!backend_call_finish (..., &error))
  {
    /* Strip D-Bus error prefix and log */
    g_dbus_error_strip_remote_error (error);
    g_warning ("Backend call failed: %s", error->message);

    /* Return generic error */
    g_dbus_method_invocation_return_error (invocation,
                                           XDG_DESKTOP_PORTAL_ERROR,
                                           XDG_DESKTOP_PORTAL_ERROR_FAILED,
                                           "Operation failed");
    return G_DBUS_METHOD_INVOCATION_HANDLED;
  }
```

## Peer Disconnect Handling

### OLD Pattern: Manual Signal Management

```c
/* OLD - manual signal connection and cleanup */
static guint peer_vanished_id = 0;

static void
on_peer_vanished (GDBusConnection *connection,
                  const char      *name,
                  gpointer         user_data)
{
  /* Cleanup */
}

/* In setup */
peer_vanished_id = g_dbus_connection_signal_subscribe (
  connection,
  NULL,
  "org.freedesktop.DBus",
  "NameOwnerChanged",
  "/org/freedesktop/DBus",
  NULL,
  G_DBUS_SIGNAL_FLAGS_MATCH_ARG0_NAMESPACE,
  on_peer_vanished,
  data,
  NULL);

/* In cleanup - easy to forget */
if (peer_vanished_id)
  g_dbus_connection_signal_unsubscribe (connection, peer_vanished_id);
```

**Problems:**
- Verbose boilerplate
- Easy to forget unsubscribe
- Inconsistent across codebase
- Manual lifetime management

### NEW Pattern: Use Helper Function

```c
/* NEW - use xdp_connection_track_peer_disconnect */
static void
on_peer_disconnect (const char *name,
                    gpointer    user_data)
{
  Session *session = user_data;
  cleanup_session (session);
}

/* In setup */
guint subscription_id =
  xdp_connection_track_peer_disconnect (connection,
                                        on_peer_disconnect,
                                        session);

/* Store for cleanup */
g_object_set_data (G_OBJECT (session),
                   "peer-disconnect-id",
                   GUINT_TO_POINTER (subscription_id));

/* Cleanup helper handles unsubscribe */
xdp_connection_untrack_peer_disconnect (connection, subscription_id);
```

**Why change:**
- Consistent pattern across portals
- Less boilerplate
- Centralized implementation
- Easier to ensure cleanup

**Recent commits:**
- Multiple commits moving to peer-disconnect signal pattern
- `659ad8a8 notification: Use peer-disconnect signal to clean up`
- `d674aa3e session-persistence: Use peer-disconnect signal to clean up`

## Request/Session Locking

### OLD Pattern: Manual Mutex Operations

```c
/* OLD - manual lock/unlock */
g_mutex_lock (&request->mutex);

if (request->exported)
  {
    xdp_request_emit_response (...);
    xdp_request_unexport (request);
  }

g_mutex_unlock (&request->mutex);
```

**Problems:**
- Easy to forget unlock
- Error paths can leave mutex locked
- Deadlock if function returns early
- Verbose

### NEW Pattern: Auto-Locking Macros

```c
/* NEW - automatic unlock at scope exit */
REQUEST_AUTOLOCK (request);

if (request->exported)
  {
    xdp_request_emit_response (...);
    xdp_request_unexport (request);
  }

/* Mutex automatically unlocked */
```

**For sessions:**
```c
/* Auto-lock and auto-unref */
SESSION_AUTOLOCK_UNREF (session);

/* Access session safely */
use_session_id (session->id);

/* Unlocked and unrefed at scope exit */
```

**Why change:**
- No forgotten unlocks
- Safe with early returns
- Less verbose
- Consistent pattern

## Invocation Return Patterns

### OLD Pattern: Return TRUE

```c
/* OLD - inconsistent return value */
static gboolean
handle_method (XdpDbusPortal         *object,
               GDBusMethodInvocation *invocation,
               ...)
{
  do_something ();
  xdp_dbus_portal_complete_method (object, invocation);
  return TRUE;  /* Unclear what TRUE means */
}
```

**Problems:**
- TRUE doesn't indicate method was handled
- Inconsistent with D-Bus patterns
- Not self-documenting

### NEW Pattern: Return G_DBUS_METHOD_INVOCATION_HANDLED

```c
/* NEW - explicit and clear */
static gboolean
handle_method (XdpDbusPortal         *object,
               GDBusMethodInvocation *invocation,
               ...)
{
  do_something ();
  xdp_dbus_portal_complete_method (object, invocation);
  return G_DBUS_METHOD_INVOCATION_HANDLED;
}
```

**Why change:**
- Self-documenting
- Consistent with GDBus conventions
- Clear intent

## Summary: Updating Old Code

When modifying existing code:

1. **File descriptors** - Replace direct access with helper functions
2. **Memory** - Add g_autoptr, g_autofree, g_auto
3. **Ownership transfers** - Add g_steal_pointer
4. **Variable scope** - Move declarations closer to use
5. **Control flow** - Use early exits for errors, keep success path unindented
6. **Error messages** - Log details, return generic errors to clients
7. **Peer disconnect** - Use xdp_connection_track_peer_disconnect
8. **Locking** - Use REQUEST_AUTOLOCK, SESSION_AUTOLOCK_UNREF
9. **Paths** - Canonicalize and validate, prefer fd-based operations
10. **Method returns** - Use G_DBUS_METHOD_INVOCATION_HANDLED

When writing new code:

- Use all new patterns from the start
- Don't copy old patterns from existing code
- Reference recent commits and modern portals
- See: `clipboard.c`, recent `util` changes

## Finding Code to Update

Search for old patterns:
```bash
# Find direct FD list access
git grep "g_unix_fd_list_get"

# Find manual memory management
git grep -E "g_free|g_variant_unref" | grep -v g_autofree

# Find function-scope error variables
git grep "GError \*error = NULL" | grep -v g_autoptr

# Find detailed error messages
git grep "g_dbus_method_invocation_return_error" | grep "error->message"
```

When you find old patterns during review or development, update them to use
new patterns as part of your changes.
