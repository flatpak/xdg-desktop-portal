# GLib Ownership and Memory Management

Proper ownership tracking is critical for preventing memory leaks,
use-after-free bugs, and other memory-related issues. This document provides
guidelines for ownership patterns in xdg-desktop-portal.

## Core Principles

### 1. C Doesn't Signal Ownership

C functions don't inherently indicate whether:
- Return values are caller-owned or callee-owned
- Parameters are borrowed or consumed
- Objects need explicit cleanup

GLib addresses this through:
- GObject introspection annotations
- Automatic cleanup and ownership transfer macros that serve as ownership
  documentation
- Consistent patterns that make ownership clear

### 2. Automatic Cleanup is Ownership Documentation

Using `g_autoptr` and related macros serves two purposes:
1. **Automatic cleanup** - prevents memory leaks
2. **Ownership annotation** - makes ownership explicit to readers

```c
/* Ownership is clear: caller owns the object */
g_autoptr(GError) error = NULL;
g_autoptr(GVariant) variant = get_variant ();
g_autofree char *str = g_strdup ("text");
g_auto(XdpFd) fd = open_file ();
```

## Required Practices

### Always Use Automatic Cleanup Helpers

**Pattern: Use g_autoptr for owned objects**

```c
/* Good - ownership is clear, no manual cleanup needed */
static gboolean
handle_request (void)
{
  g_autoptr(GError) error = NULL;
  g_autoptr(GVariant) result = NULL;

  result = perform_operation (&error);
  if (!result)
    {
      g_warning ("Operation failed: %s", error->message);
      return FALSE;
    }

  process_result (result);
  return TRUE;
  /* error and result automatically cleaned up */
}
```

**Anti-pattern: Manual cleanup**

```c
/* Bad - error-prone, must track all exit paths */
static gboolean
handle_request (void)
{
  GError *error = NULL;
  GVariant *result = NULL;

  result = perform_operation (&error);
  if (!result)
    {
      g_warning ("Operation failed: %s", error->message);
      g_clear_error (&error);
      return FALSE;  /* Must remember to clean up here */
    }

  process_result (result);
  g_variant_unref (result);  /* And here */
  return TRUE;
}
```

### Available Cleanup Macros

```c
/* For pointer types with cleanup functions defined */
g_autoptr(GObject) obj = g_object_new (...);
g_autoptr(GError) error = NULL;
g_autoptr(GVariant) variant = NULL;
g_autoptr(GDBusConnection) connection = NULL;
g_autoptr(XdpSession) session = NULL;

/* For objects to free with g_free */
g_autofree char *str = g_strdup ("text");
g_autofree guchar *data = g_malloc (size);

/* For stack allocated and special types */
g_auto(GStrv) strv = g_strsplit ("a,b,c", ",", -1);
g_auto(GVariantBuilder) options =
  G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);

/* For GList and GQueue containers for type Foo */
g_autolist(Foo) list = NULL;
g_autoqueue(Foo) queue = NULL;

/* For file descriptors */
g_autofd int fd = -1;
```

## Ownership Transfer

### Mark Transfers with g_steal_pointer

When a function takes ownership of an object, use `g_steal_pointer` to make the transfer explicit.

**Pattern: Explicit ownership transfer**

```c
static void
register_session (void)
{
  g_autoptr(XdpSession) session = create_session ();

  /* Configure session while we own it */
  xdp_session_set_property (session, "key", "value");

  /* Transfer ownership to the registry */
  session_registry_add (registry, g_steal_pointer (&session));
  /* session is now NULL, registry owns it */
}
```

**Without g_steal_pointer, ownership is unclear:**

```c
/* Bad - does registry take ownership or just borrow? */
static void
register_session (void)
{
  g_autoptr(XdpSession) session = create_session ();

  session_registry_add (registry, session);
  /* Unclear: will session be freed twice? */
}
```

### Common Ownership Transfer Scenarios

#### Storing in Container

```c
/* Hash table takes ownership */
g_autoptr(Data) data = create_data ();
g_hash_table_insert (table,
                     g_strdup (key),
                     g_steal_pointer (&data));

/* List takes ownership */
g_autoptr(Item) item = create_item ();
list = g_list_prepend (list, g_steal_pointer (&item));
```

#### Returning Owned Value

```c
/* Transfer full ownership to caller */
XdpSession *
create_and_configure_session (void)
{
  g_autoptr(XdpSession) session = xdp_session_new ();

  configure_session (session);

  return g_steal_pointer (&session);
}
```

#### Passing to Async Callback

```c
static void
start_operation (void)
{
  g_autoptr(Request) request = create_request ();

  /* Request ownership transferred to callback */
  xdp_dbus_impl_call_method (impl,
                             ...,
                             NULL,
                             callback,
                             g_steal_pointer (&request));
}

static void
callback (GObject      *source,
          GAsyncResult *result,
          gpointer      user_data)
{
  g_autoptr(Request) request = user_data;
  /* Callback owns request, will auto-cleanup */
}
```

## Variable Scoping

### Scope Variables at Their Natural Lifetime

Modern C allows declaring variables where they're needed, which makes ownership
clearer.

**Pattern: Limit scope to limit lifetime**

```c
static void
process_items (GPtrArray *items)
{
  for (guint i = 0; i < items->len; i++)
    {
      g_autoptr(GError) error = NULL;
      g_autoptr(GVariant) result = NULL;

      Item *item = g_ptr_array_index (items, i);
      result = process_item (item, &error);

      if (!result)
        {
          g_warning ("Failed to process item %u: %s", i, error->message);
          continue;
        }

      store_result (result);
      /* result and error cleaned up here at end of iteration */
    }
}
```

**Anti-pattern: Function-scope variables**

```c
/* Bad - unclear when objects are still needed */
static void
process_items (GPtrArray *items)
{
  GError *error = NULL;
  GVariant *result = NULL;
  Item *item;

  for (guint i = 0; i < items->len; i++)
    {
      item = g_ptr_array_index (items, i);
      result = process_item (item, &error);

      if (!result)
        {
          g_warning ("Failed to process item %u: %s", i, error->message);
          g_clear_error (&error);
          continue;
        }

      store_result (result);
      g_clear_pointer (&result, g_variant_unref);
      /* Must manually clean up in every path */
    }
}
```

### Use Custom Scopes for Ownership Clarity

When variables are only needed for a short time, consider using explicit scopes
to declare them close to where they are needed and to make sure they can't be
accessed later.

```c
static void
prepare_and_execute (void)
{
  g_autoptr(ExecutionContext) context = NULL;

  /* Temporary objects for preparation */
  {
    g_autoptr(Config) config = load_config ();
    g_autoptr(GVariant) options = build_options ();

    context = create_execution_context (config, options);
    /* config and options cleaned up here */
  }

  /* Only context remains */
  execute (context);
}
```

## Review Checklist

### Ownership Clarity
- [ ] All owned objects use `g_autoptr`, `g_autofree`, or `g_auto`
- [ ] Ownership transfers use `g_steal_pointer`
- [ ] Borrowed references are not stored beyond guaranteed lifetime
- [ ] Variables scoped to their natural lifetime

### Memory Safety
- [ ] No manual `g_object_unref`, `g_free`, or `close` in simple cases
- [ ] No memory leaks in error paths
- [ ] No use-after-free issues
- [ ] No double-free issues

### Common Issues
- [ ] Objects stored in containers properly transferred
- [ ] Signal handlers don't create reference cycles
- [ ] File descriptors closed (via `g_auto(XdpFd)`)

## Further Reading

- [GLib Ownership Best Practices](https://blog.sebastianwick.net/posts/glib-ownership-best-practices/)
- [GObject Memory Management](https://docs.gtk.org/gobject/memory.html)
- [g_autoptr Documentation](https://docs.gtk.org/glib/auto-cleanup.html)
