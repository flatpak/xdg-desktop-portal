#include "xdp-dbus.h"

typedef struct _Request Request;
typedef struct _RequestClass RequestClass;

struct _Request
{
  XdpRequestSkeleton parent_instance;

  gboolean exported;
  char *app_id;
  char *id;
  char *sender;
  GMutex mutex;
};

struct _RequestClass
{
  XdpRequestSkeletonClass parent_class;
};

GType request_get_type (void) G_GNUC_CONST;

G_DEFINE_AUTOPTR_CLEANUP_FUNC (Request, g_object_unref)

void set_proxy_use_threads (GDBusProxy *proxy);

void request_init_invocation (GDBusMethodInvocation  *invocation, const char *app_id);
Request *request_from_invocation (GDBusMethodInvocation *invocation);
void request_export (Request *request,
                     GDBusConnection *connection);
void request_unexport (Request *request);

static inline void
auto_unlock_helper (GMutex **mutex)
{
  if (*mutex)
    g_mutex_unlock (*mutex);
}

static inline GMutex *
auto_lock_helper (GMutex *mutex)
{
  if (mutex)
    g_mutex_lock (mutex);
  return mutex;
}

#define REQUEST_AUTOLOCK(request) G_GNUC_UNUSED __attribute__((cleanup (auto_unlock_helper))) GMutex * G_PASTE (request_auto_unlock, __LINE__) = auto_lock_helper (&request->mutex);
