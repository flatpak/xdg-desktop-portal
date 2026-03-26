# Coding Style

The style is based mostly on the GNOME coding style:
https://developer.gnome.org/documentation/guidelines/programming/coding-style.html

## When to Deviate

### Consistency Over Style
In existing files with established patterns, maintain consistency with the
surrounding code rather than strictly following these guidelines:

- If the file uses tabs, continue using tabs
- If the file has a different brace style, continue with it
- If variable names follow a different pattern, continue the pattern

### File Modernization
When making substantial changes to a file (not just a small fix), it's
acceptable to modernize the style to match the guidelines here.

## Key Style Guidelines

### Indentation
- Use 2 spaces for indentation (not tabs)
- Align continuation lines logically

```c
if (very_long_condition &&
    another_condition)
  {
    do_something ();
  }
```

### Braces
- Opening brace on same line for functions
- Opening brace on next line for control structures
- Always use braces for if/else/while/for

```c
static gboolean
function_name (int arg)
{
  if (condition)
    {
      statement;
    }
  else
    {
      other_statement;
    }

  return TRUE;
}
```

### Spacing
- Space after keywords: `if (`, `while (`, `for (`
- Space around operators: `a = b + c`
- Space before function calls: `function (arg)`
- No space inside parentheses: `(arg)` not `( arg )`

```c
/* Good */
if (condition)
  function (a, b, c);

/* Bad */
if( condition )
  function( a,b,c );
```

### Naming Conventions
- Functions: `lowercase_with_underscores`
- Types: `CamelCase` (e.g., `XdpSession`, `RemoteDesktopSession`)
- Macros: `UPPERCASE_WITH_UNDERSCORES`
- Constants: `UPPERCASE_WITH_UNDERSCORES`
- Variables: `lowercase_with_underscores`

```c
#define MAX_SESSIONS 100

typedef struct _XdpSession XdpSession;

static gboolean
xdp_session_is_active (XdpSession *session)
{
  g_autoptr(GError) error = NULL;
  int result = 0;

  return result > 0;
}
```

### Function Declarations
- Return type and qualifiers on separate line definitions
- Return type and qualifiers on same line in declarations
- One argument per line, names all alligned

```c
/* definition */
static gboolean
handle_method (XdpDbusPortal         *object,
               GDBusMethodInvocation *invocation,
               const char            *arg1,
               GVariant              *arg2)
{
  /* ... */
}

/* declaration */
XdpSession *xdp_session_new (const char *id);
```

### Parameter Alignment
- Align parameters vertically for readability
- Break long parameter lists into multiple lines

```c
xdp_dbus_impl_portal_call_method (impl,
                                  arg1,
                                  arg2,
                                  arg3,
                                  NULL,
                                  callback,
                                  user_data);
```

### Line Length
- Prefer lines under 80 characters
- Break long lines at logical points
- Break string literals when necessary

### Comments
- Use `/* */` for comments, not `//`
- Write comments that explain why, not what
- Place comments above the code they describe
- For multi-line comments use `*` on every line

```c
/* This is necessary because the backend may return stale data */
validate_backend_response (response);


/* This is necessary because the backend
 * may return stale data.
 */
validate_backend_response (response);
```

### Header Guards
Use `#pragma once` at the top of headers:

```c
#pragma once

#include <glib.h>
/* ... */
```

### Include Order
1. config.h (first, always)
2. System headers
3. GLib/GIO headers
4. Local headers

```c
#include "config.h"

#include <stdio.h>
#include <string.h>

#include <glib.h>
#include <gio/gio.h>

#include "xdp-utils.h"
#include "clipboard.h"
```

### Type Definitions
- Use GLib types only when necessary (`gboolean`)
- Use standard types for `int`, `char`, `unsigned int`, etc.
- Use standard types for specific sizes: `uint32_t`, `int64_t`, `size_t`

```c
gboolean result;
unsigned int count;
size_t length;
uint32_t flags;
```

### Automatic Cleanup
- `g_autoptr` and `g_auto` apply on the type name
- No space between the auto-cleanup modifiers and the type
- `g_autofree` and `g_autofd` stand in front of the type they modify

```c
g_autoptr(GError) error = NULL;
g_autoptr(GVariant) variant = NULL;
g_autofree char *str = g_strdup ("...");
g_autofd int fd = -1;
g_auto(GVariantBuilder) options =
  G_VARIANT_BUILDER_INIT (G_VARIANT_TYPE_VARDICT);
```

### Switch Statements
- Indent case labels
- Always include break or fallthrough comment

```c
switch (value)
  {
  case 0:
    handle_zero ();
    break;

  case 1:
    handle_one ();
    /* fallthrough */

  case 2:
    handle_one_or_two ();
    break;

  default:
    handle_default ();
    break;
  }
```

### Pointer Declaration
- Pointer asterisk with the variable name: `char *str`
- Not with the type: not `char* str`

```c
char *string;
const char *const_string;
XdpSession *session;
```

### File Structure
1. License header
2. Includes
3. Type definitions
4. Static/private function declarations (if needed)
5. Global variables (minimize these)
6. Function implementations
7. Public API last
