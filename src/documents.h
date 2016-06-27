void init_document_proxy (GDBusConnection *connection);

char *register_document (const char *uri,
                         const char *app_id,
                         gboolean for_save,
                         gboolean writable,
                         GError **error);
