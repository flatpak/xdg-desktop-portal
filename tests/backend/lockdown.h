#pragma once

void lockdown_init (GDBusConnection *connection, const char *object_path);

void lockdown_update (void);
