# D-Bus API Design and Compatibility

XDG Desktop Portal exposes two distinct D-Bus APIs with different compatibility
requirements. Understanding which API you're changing is critical for
determining compatibility constraints.

## The Two APIs

### Frontend API (org.freedesktop.portal.*)

**What it is:**
- D-Bus interface between **applications** and **xdg-desktop-portal**
- Exposed at bus name `org.freedesktop.portal.Desktop`
- Used by thousands of sandboxed applications (Flatpak, Snap, etc.)

**Who uses it:**
- Applications installed on user systems
- Applications that won't all update simultaneously
- Third-party apps outside system control

**Compatibility requirement: MUST be backwards compatible**
- Applications depend on these interfaces working forever
- Must not break existing apps with portal updates
- Must support old and new clients simultaneously

**Example interfaces:**
- `org.freedesktop.portal.FileChooser`
- `org.freedesktop.portal.Screenshot`
- `org.freedesktop.portal.Clipboard`

### Backend/Impl API (org.freedesktop.impl.portal.*)

**What it is:**
- D-Bus interface between **xdg-desktop-portal** and **backend implementations**
- Exposed at bus names like `org.freedesktop.impl.portal.desktop.gnome`
- Used by desktop environment components (GNOME Shell, KDE Plasma, etc.)

**Who uses it:**
- System components installed together
- Desktop environment shells and services
- All update together as a cohesive system

**Compatibility requirement: CAN break backwards compatibility**
- Frontend and backends update together via system packages
- Breaking changes are acceptable if coordinated
- Still prefer backwards compatibility when easy

**Example interfaces:**
- `org.freedesktop.impl.portal.FileChooser`
- `org.freedesktop.impl.portal.Screenshot`
- `org.freedesktop.impl.portal.Clipboard`

### Internal API

**What it is:**
- C functions, headers, structures within xdg-desktop-portal
- Not exposed over D-Bus
- Implementation details

**Compatibility requirement: None**
- Can change freely
- Only affects xdg-desktop-portal compilation
- No external consumers

**Examples:**
- `xdp_filter_options()`
- `xdp_get_portal_call_fd()`
- `XdpRequest`, `XdpSession` structures

## Frontend API Compatibility Rules

These rules apply to `org.freedesktop.portal.*` interfaces.

### Absolute Rules (NEVER Break)

#### 1. Must not Remove or Rename Existing Methods

```xml
<!-- BAD - breaking change -->
<interface name="org.freedesktop.portal.Screenshot">
  <!-- MethodName removed or renamed -->
</interface>

<!-- GOOD - deprecate but keep working -->
<interface name="org.freedesktop.portal.Screenshot">
  <method name="Screenshot">
    <!-- Deprecated: Use ScreenshotV2 instead -->
    <!-- But still functional -->
  </method>
  <method name="ScreenshotV2">
    <!-- New preferred method -->
  </method>
</interface>
```

#### 2. Must not Change Method Signatures

```xml
<!-- BAD - added required parameter -->
<method name="TakeScreenshot">
  <arg type="s" name="parent_window" direction="in"/>
  <arg type="s" name="new_required_arg" direction="in"/>  <!-- BREAKS CLIENTS -->
  <arg type="a{sv}" name="options" direction="in"/>
</method>

<!-- GOOD - use options dictionary for new parameters -->
<method name="TakeScreenshot">
  <arg type="s" name="parent_window" direction="in"/>
  <arg type="a{sv}" name="options" direction="in"/>
  <!-- New parameters go in options with defaults -->
</method>
```

#### 3. Must not Remove or Rename Properties/Signals

```xml
<!-- BAD - property removed -->
<interface name="org.freedesktop.portal.Desktop">
  <!-- version property removed - BREAKS CLIENTS -->
</interface>

<!-- GOOD - keep all properties forever -->
<interface name="org.freedesktop.portal.Desktop">
  <property name="version" type="u" access="read"/>
  <!-- Even if unused, must remain -->
</interface>
```

#### 4. Must not Change Semantics of Existing Options

```c
/* BAD - changed option meaning */
/* Version 1: "interactive" meant "show UI" */
/* Version 2: "interactive" means "require user action" */
/* This breaks apps expecting old behavior */

/* GOOD - new option for new behavior */
/* Version 1: "interactive" means "show UI" */
/* Version 2: Add "require_user_action" option for new behavior */
```

### Allowed Frontend API Changes

#### 1. Adding New Methods ✓

```xml
<interface name="org.freedesktop.portal.FileChooser">
  <!-- Existing method - must remain -->
  <method name="OpenFile">...</method>

  <!-- New method - fine to add -->
  <method name="OpenFileV2">...</method>
</interface>
```

Apps that don't know about new methods simply won't call them.

#### 2. Adding Optional Parameters via Options ✓

```xml
<method name="TakeScreenshot">
  <arg type="s" name="parent_window" direction="in"/>
  <arg type="a{sv}" name="options" direction="in"/>
</method>
```

```c
/* Version 1: no options */
/* Version 2: add "include_cursor" option with default false */
static XdpOptionKey screenshot_options[] = {
  { "include_cursor", G_VARIANT_TYPE_BOOLEAN, NULL },  /* New in version 2 */
};

/* Old clients don't pass this option - default behavior maintained */
```

#### 3. Adding New Properties ✓

```xml
<property name="version" type="u" access="read"/>
<property name="AvailableFeatures" type="as" access="read"/>  <!-- New property -->
```

Old clients that don't check new properties continue working.

#### 4. Adding New Result Keys ✓

```c
/* Version 1: return only "uris" */
g_variant_builder_add (&results, "{sv}",
                       "uris", uris_variant);

/* Version 2: also return "metadata" if available */
g_variant_builder_add (&results, "{sv}",
                       "uris", uris_variant);
if (metadata)
  g_variant_builder_add (&results, "{sv}",
                         "metadata", metadata_variant);  /* New key */

/* Old clients ignore unknown keys per D-Bus spec */
```

## Backend API Compatibility

These rules apply to `org.freedesktop.impl.portal.*` interfaces.

### Backend API is More Flexible

Since xdg-desktop-portal and backends update together as system packages, we
can make breaking changes when necessary.

**Prefer backwards compatibility** when it's easy, but don't let it block
important changes.

### Breaking Backend API

When making breaking backend changes:

1. **Coordinate with backend maintainers**
   - Notify GNOME, KDE, wlroots backend maintainers
   - Give advance notice for major changes

2. **Update all in-tree backends simultaneously**
   - If we maintain test backends, update them

3. **Version the backend interface**
   - Increment backend interface version
   - Frontend can check backend version before using new features

4. **Provide migration path**
   - Document what backends need to change
   - Provide example code if complex

### Backend Changes That Don't Break Compatibility

Even for backends, these changes are always safe:

- Adding new methods (backends can ignore if not implemented)
- Adding optional parameters to options dictionaries
- Adding new result keys
- Adding new properties
- Adding new signals

### Example Backend Breaking Change

```xml
<!-- Version 1 -->
<method name="TakeScreenshot">
  <arg type="o" name="handle" direction="in"/>
  <arg type="s" name="app_id" direction="in"/>
  <arg type="s" name="parent_window" direction="in"/>
  <arg type="a{sv}" name="options" direction="in"/>
  <arg type="u" name="response" direction="out"/>
  <arg type="a{sv}" name="results" direction="out"/>
</method>

<!-- Version 2 - BREAKING: changed signature -->
<method name="TakeScreenshot">
  <arg type="o" name="handle" direction="in"/>
  <arg type="s" name="app_id" direction="in"/>
  <arg type="s" name="parent_window" direction="in"/>
  <arg type="s" name="session_id" direction="in"/>  <!-- NEW REQUIRED ARG -->
  <arg type="a{sv}" name="options" direction="in"/>
  <arg type="u" name="response" direction="out"/>
  <arg type="a{sv}" name="results" direction="out"/>
</method>
```

This is acceptable for backend API because:
- GNOME backend updates simultaneously with xdg-desktop-portal
- KDE backend updates simultaneously with xdg-desktop-portal
- All packaged together as system update

For frontend API, this would be **unacceptable** because apps won't all update.

## Version Discovery

Both frontend and backend interfaces use version properties for feature discovery.

### Frontend Version

```xml
<interface name="org.freedesktop.portal.FileChooser">
  <property name="version" type="u" access="read"/>
</interface>
```

```c
/* Portal implementation */
g_object_class_install_property (..., "version", ...);

/* In portal init */
xdp_dbus_file_chooser_set_version (skeleton, 4);  /* Current version */
```

```python
# Client checks version before using new features
portal = bus.get_object('org.freedesktop.portal.Desktop', '/org/freedesktop/portal/desktop')
iface = dbus.Interface(portal, 'org.freedesktop.portal.FileChooser')
version = iface.Get('org.freedesktop.portal.FileChooser', 'version')

if version >= 4:
    # Use new features added in version 4
    options['new_feature'] = True
```

### Backend Version

```xml
<interface name="org.freedesktop.impl.portal.FileChooser">
  <property name="version" type="u" access="read"/>
</interface>
```

```c
/* Frontend checks backend version before using new features */
guint impl_version = xdp_dbus_impl_file_chooser_get_version (impl);

if (impl_version >= 3)
  {
    /* Backend supports new feature added in version 3 */
    g_variant_builder_add (&options, "{sv}",
                           "new_backend_feature", g_variant_new_boolean (TRUE));
  }

xdp_dbus_impl_file_chooser_call_open_file (impl, ..., &options, ...);
```

### When to Increment Version

**Frontend version:**
- Increment when adding new methods
- Increment when adding new options (even optional)
- Increment when adding new result keys
- Increment when changing behavior (even if compatible)
- DO NOT increment for bug fixes or documentation

**Backend version:**
- Increment when adding new methods
- Increment when adding new options
- Increment when making breaking changes
- DO NOT increment for internal refactoring

## Frontend API Patterns

### Request Pattern

Most portal methods follow the Request pattern for asynchronous operations:

```xml
<method name="MethodName">
  <arg type="s" name="parent_window" direction="in"/>
  <arg type="a{sv}" name="options" direction="in"/>
  <arg type="o" name="handle" direction="out"/>  <!-- Request object path -->
</method>
```

Flow:
1. Client calls method, gets Request object path immediately
2. Client subscribes to Response signal on Request object
3. Portal validates, creates Request, forwards to backend
4. Backend shows UI, performs operation
5. Backend returns response to portal
6. Portal emits Response signal on Request object
7. Client receives Response with results

## Documentation Requirements

Every API change must be documented.

### Frontend API Changes

1. **XML Interface Definition**
   ```xml
   <!--
     MethodName:
     @parent_window: Parent window identifier
     @options: Additional options
     @handle: Object path for the request

     Description of what this method does.

     Supported options:

     * `option1` (`s`)

       Description. Default: "value".

     * `option2` (`b`) (since version 2)

       Description. Default: false.

     This method was added in version 1.
     The `option2` option was added in version 2.
   -->
   ```

2. **Option Documentation**
   - Type signature
   - Description and semantics
   - Default value if not provided
   - Version that added this option

3. **Result Documentation**
   - Type signature
   - When it's included (always vs. optional)
   - Version that added this key

4. **Migration Guide** (if behavior changes)
   - How old behavior is preserved
   - How clients can opt into new behavior
   - Code examples showing both patterns

### Backend API Changes

1. **Interface Definition**
   - Complete XML definition
   - Parameter descriptions

2. **Migration Guide** (for breaking changes)
   - What backends need to change
   - Code examples showing old and new patterns
   - Timeline for change

3. **Coordination Plan**
   - Which backends are affected
   - Who maintains each backend
   - Notification sent to maintainers

## Review Checklist

### Frontend API Changes

- [ ] Maintains backwards compatibility (REQUIRED)
- [ ] New options are optional with sensible defaults
- [ ] New result keys don't break old clients
- [ ] XML documentation complete with version annotations
- [ ] Frontend version incremented if adding features
- [ ] Old clients tested to ensure they still work
- [ ] Security implications documented
- [ ] Migration guide provided if behavior changes

### Backend API Changes

- [ ] Backend maintainers notified of breaking changes
- [ ] Backend version incremented for new features/breaking changes
- [ ] Frontend checks backend version before using new features
- [ ] All in-tree backends updated simultaneously
- [ ] Migration guide provided for backends
- [ ] Backwards compatibility preferred when easy
- [ ] Breaking changes justified and necessary

### Internal API Changes

- [ ] No external compatibility concerns
- [ ] Compiles successfully
- [ ] No public ABI breaks (if library is used externally)

## Common Mistakes

### Adding Required Parameters to Frontend Methods

**WRONG:** Breaks apps expecting the old method signature

```xml
<method name="OpenFile">
  <arg type="s" name="parent_window" direction="in"/>
  <arg type="s" name="new_required" direction="in"/>  <!-- BREAKS COMPATIBILITY -->
</method>
```

**Alternative:** Using Options for New Parameters

```xml
<method name="OpenFile">
  <arg type="s" name="parent_window" direction="in"/>
  <arg type="a{sv}" name="options" direction="in"/>
  <!-- new_parameter goes in options with default -->
</method>
```

### Changing Option Semantics

**WRONG:** Breaks apps expecting old behavior

```c
/* Version 1: "modal" meant "block parent window" */
/* Version 2: "modal" means "system modal dialog" */
```

**Alternative:** Adding new options for new behavior

```c
/* Version 1: "modal" means "block parent window" */
/* Version 2: add "system_modal" for new behavior */
```

### Removing Frontend Methods

**WRONG:** Apps calling the removed method might crash or misbehave

```xml
<interface name="org.freedesktop.portal.Screenshot">
  <!-- Screenshot method removed -->
</interface>
```

**Alternative:** Deprecating old methods and adding a new method

```xml
<interface name="org.freedesktop.portal.Screenshot">
  <!-- Deprecated: use Screenshot2 -->
  <method name="Screenshot">
    <!-- Old, still functional method -->
  </method>
  <method name="Screenshot2">
    <!-- New preferred method -->
  </method>
</interface>
```

## Summary

- **Frontend API** (`org.freedesktop.portal.*`) - MUST be backwards compatible
  - Apps depend on it, won't all update
  - Breaking changes are unacceptable
  - Use options dictionaries for extensibility

- **Backend API** (`org.freedesktop.impl.portal.*`) - CAN break when necessary
  - Frontend and backends update together
  - Still prefer compatibility when easy
  - Coordinate with backend maintainers

- **Internal API** - Can change freely
  - No external consumers
  - Standard C library practices

Always use version properties for feature discovery. Document everything.
