#include <gdk-pixbuf/gdk-pixbuf.h>

static int
validate_icon (const char *filename)
{
  GdkPixbufFormat *format;
  int width, height;
  const char *name;
  const char *allowed_formats[] = { "png", "jpeg", "svg", NULL };
  g_autoptr(GdkPixbuf) pixbuf = NULL;
  g_autoptr(GError) error = NULL;

  format = gdk_pixbuf_get_file_info (filename, &width, &height);
  if (format == NULL) 
    {
      g_printerr ("Format not recognized\n");
      return 1;
    }

  name = gdk_pixbuf_format_get_name (format);
  if (!g_strv_contains (allowed_formats, name))
    {
      g_printerr ("Format %s not allowed\n", name);
      return 1;
    }

  if (width > 256 || height > 256)
    {
      g_printerr ("Image too large (%dx%d)\n", width, height);
      return 1;
    }

  pixbuf = gdk_pixbuf_new_from_file (filename, &error);
  if (pixbuf == NULL)
    {
      g_printerr ("Failed to load image: %s\n", error->message);
      return 1;
    }

  return 0;
}

int
main (int argc, char *argv[])
{
  if (argc != 2)
    {
      g_error ("Expect a single path");
      return 1;
    }

  return validate_icon (argv[1]);
}
