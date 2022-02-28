#pragma once

G_BEGIN_DECLS

typedef enum {
  DOCUMENT_PERMISSION_FLAGS_READ               = (1 << 0),
  DOCUMENT_PERMISSION_FLAGS_WRITE              = (1 << 1),
  DOCUMENT_PERMISSION_FLAGS_GRANT_PERMISSIONS  = (1 << 2),
  DOCUMENT_PERMISSION_FLAGS_DELETE             = (1 << 3),

  DOCUMENT_PERMISSION_FLAGS_ALL               = ((1 << 4) - 1)
} DocumentPermissionFlags;

typedef enum {
  DOCUMENT_ADD_FLAGS_REUSE_EXISTING             = (1 << 0),
  DOCUMENT_ADD_FLAGS_PERSISTENT                 = (1 << 1),
  DOCUMENT_ADD_FLAGS_AS_NEEDED_BY_APP           = (1 << 2),
  DOCUMENT_ADD_FLAGS_DIRECTORY                  = (1 << 3),

  DOCUMENT_ADD_FLAGS_FLAGS_ALL                  = ((1 << 4) - 1)
} DocumentAddFullFlags;

G_END_DECLS
