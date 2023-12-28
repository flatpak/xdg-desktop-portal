Requests
========

Requests made via portal interfaces generally involve user interaction, and
dialogs that can stay open for a long time. Therefore portal APIs don't just use
async method calls (which time out after at most 25 seconds), but instead return
results via a ``Response`` signal on :ref:`Request <org.freedesktop.portal.Request>`
objects.

Portal APIs don't use properties very much. This is partially because we need to
be careful about the flow of information, and partially because it would be
unexpected to have a dialog appear when you just set a property. However, every
portal has at least one version property that specifies the maximum version
supported by xdg-desktop-portal.

General Flow
------------

The general flow of the portal API is as follows:

1. The application makes a portal request
2. The portal replies to that method call with a handle (i.e. object path) to a
   :ref:`Request <org.freedesktop.portal.Request>` object that corresponds to the
   request
3. The object is exported on the bus and stays alive as long as the user
   interaction lasts
4. When the user interaction is over, the portal sends a ``Response`` signal back
   to the application with the results from the interaction, if any.

To avoid a race condition between the caller subscribing to the signal after
receiving the reply for the method call and the signal getting emitted, a
convention for Request object paths has been established that allows the caller
to subscribe to the signal before making the method call.