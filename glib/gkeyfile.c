/* gkeyfile.c - key file parser
 *
 *  Copyright 2004  Red Hat, Inc.  
 *
 * Written by Ray Strode <rstrode@redhat.com>
 *
 * GLib is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * GLib is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with GLib; see the file COPYING.LIB.  If not,
 * write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 *   Boston, MA 02111-1307, USA.
 */

#include "config.h"
#include "gkeyfile.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "gconvert.h"
#include "gdataset.h"
#include "gerror.h"
#include "gfileutils.h"
#include "ghash.h"
#include "glibintl.h"
#include "glist.h"
#include "gslist.h"
#include "gmem.h"
#include "gmessages.h"
#include "gstring.h"
#include "gstrfuncs.h"
#include "gutils.h"

typedef struct _GKeyFileGroup GKeyFileGroup;

struct _GKeyFile
{
  GList *groups;

  gchar  *start_group_name;

  GKeyFileGroup *current_group;

  GString *parse_buffer; /* Holds up to one line of not-yet-parsed data */

  /* Used for sizing the output buffer during serialization
   */
  gsize approximate_size;

  gchar list_separator;

  GKeyFileFlags flags;
};

struct _GKeyFileGroup
{
  const gchar *name;  /* NULL for above first group (which will be comments) */

  GList *key_value_pairs; 

  /* Used in parallel with key_value_pairs for
   * increased lookup performance
   */
  GHashTable *lookup_map;
};

typedef struct
{
  gchar *key;  /* NULL for comments */
  gchar *value;
} GKeyFileKeyValuePair;

static gint                  find_file_in_data_dirs            (const gchar            *file,
								gchar                 **output_file,
								gchar                ***data_dirs,
								GError                **error);
static void                  g_key_file_load_from_fd           (GKeyFile               *key_file,
								gint                    fd,
								GKeyFileFlags           flags,
								GError                **error);
static GKeyFileGroup        *g_key_file_lookup_group           (GKeyFile               *key_file,
								const gchar            *group_name);
static GKeyFileKeyValuePair *g_key_file_lookup_key_value_pair  (GKeyFile               *key_file,
								GKeyFileGroup          *group,
								const gchar            *key);
static void                  g_key_file_remove_group_node      (GKeyFile               *entry,
								GList                  *group_node);
static void                  g_key_file_add_key                (GKeyFile               *entry,
								const gchar            *group_name,
								const gchar            *key,
								const gchar            *value);
static void                  g_key_file_add_group              (GKeyFile               *entry,
								const gchar            *group_name);
static void                  g_key_file_key_value_pair_free    (GKeyFileKeyValuePair   *pair);
static gboolean              g_key_file_line_is_comment        (const gchar            *line);
static gboolean              g_key_file_line_is_group          (const gchar            *line);
static gboolean              g_key_file_line_is_key_value_pair (const gchar            *line);
static gchar                *g_key_file_parse_value_as_string  (GKeyFile               *entry,
								const gchar            *value,
								GSList                **separators,
								GError                **error);
static gchar                *g_key_file_parse_string_as_value  (GKeyFile               *entry,
								const gchar            *string,
								gboolean                escape_separator);
static gint                  g_key_file_parse_value_as_integer (GKeyFile               *entry,
								const gchar            *value,
								GError                **error);
static gchar                *g_key_file_parse_integer_as_value (GKeyFile               *entry,
								gint                    value);
static gboolean              g_key_file_parse_value_as_boolean (GKeyFile               *entry,
								const gchar            *value,
								GError                **error);
static gchar                *g_key_file_parse_boolean_as_value (GKeyFile               *entry,
								gboolean                value);
static void                  g_key_file_parse_key_value_pair   (GKeyFile               *entry,
								const gchar            *line,
								gsize                   length,
								GError                **error);
static void                  g_key_file_parse_comment          (GKeyFile               *entry,
								const gchar            *line,
								gsize                   length,
								GError                **error);
static void                  g_key_file_parse_group            (GKeyFile               *entry,
								const gchar            *line,
								gsize                   length,
								GError                **error);
static gchar                *key_get_locale                    (const gchar            *key);
static void                  g_key_file_parse_data             (GKeyFile               *entry,
								const gchar            *data,
								gsize                   length,
								GError                **error);
static void                  g_key_file_flush_parse_buffer     (GKeyFile               *entry,
								GError                **error);


GQuark
g_key_file_error_quark (void)
{
  static GQuark error_quark = 0;

  if (error_quark == 0)
    error_quark = g_quark_from_static_string ("g-key-file-error-quark");

  return error_quark;
}

static void
g_key_file_init (GKeyFile *key_file)
{  
  key_file->current_group = g_new0 (GKeyFileGroup, 1);
  key_file->groups = g_list_prepend (NULL, key_file->current_group);
  key_file->start_group_name = NULL;
  key_file->parse_buffer = g_string_sized_new (128);
  key_file->approximate_size = 0;
  key_file->list_separator = ';';
  key_file->flags = 0;
}

static void
g_key_file_clear (GKeyFile *key_file)
{
  GList *tmp, *group_node;

  if (key_file->parse_buffer)
    g_string_free (key_file->parse_buffer, TRUE);

  g_free (key_file->start_group_name);

  tmp = key_file->groups;
  while (tmp != NULL)
    {
      group_node = tmp;
      tmp = tmp->next;
      g_key_file_remove_group_node (key_file, group_node);
    }

  g_assert (key_file->groups == NULL);
}


/**
 * g_key_file_new:
 * @flags: flags from #GKeyFileFlags
 *
 * Creates a new empty #GKeyFile object. Use g_key_file_load_from_file(),
 * g_key_file_load_from_data() or g_key_file_load_from_data_dirs() to
 * read an existing key file.
 *
 * Return value: an empty #GKeyFile.
 *
 * Since: 2.6
 **/
GKeyFile *
g_key_file_new (void)
{
  GKeyFile *key_file;

  key_file = g_new0 (GKeyFile, 1);
  g_key_file_init (key_file);

  return key_file;
}

/**
 * g_key_file_set_list_separator:
 * @key_file: a #GKeyFile 
 * @separator: the separator
 *
 * Sets the character which is used to separate
 * values in lists. Typically ';' or ',' are used
 * as separators. The default list separator is ';'.
 *
 * Since: 2.6
 */
void
g_key_file_set_list_separator (GKeyFile *key_file,
			       gchar     separator)
{
  key_file->list_separator = separator;
}


/* Iterates through all the directories in *dirs trying to
 * open file.  When it successfully locates and opens a file it
 * returns the file descriptor to the open file.  It also
 * outputs the absolute path of the file in output_file and
 * leaves the unchecked directories in *dirs.
 */
static gint
find_file_in_data_dirs (const gchar   *file,
                        gchar        **output_file,
                        gchar       ***dirs,
                        GError       **error)
{
  gchar **data_dirs, *data_dir, *path;
  gint fd;
  GError *file_error;

  file_error = NULL;
  path = NULL;
  fd = -1;

  if (dirs == NULL)
    return fd;

  data_dirs = *dirs;

  while (data_dirs && (data_dir = *data_dirs) && fd < 0)
    {
      gchar *candidate_file, *sub_dir;

      candidate_file = (gchar *) file;
      sub_dir = g_strdup ("");
      while (candidate_file != NULL && fd < 0)
        {
          gchar *p;

          path = g_build_filename (data_dir, sub_dir,
                                   candidate_file, NULL);

          fd = open (path, O_RDONLY);

          if (output_file != NULL)
            *output_file = g_strdup (path);

          g_free (path);

          if (fd < 0 && file_error == NULL)
            file_error = g_error_new (G_KEY_FILE_ERROR,
                                      G_KEY_FILE_ERROR_NOT_FOUND,
                                      _("Valid key file could not be "
                                        "found in data dirs")); 

          candidate_file = strchr (candidate_file, '-');

          if (candidate_file == NULL)
            break;

          candidate_file++;

          g_free (sub_dir);
          sub_dir = g_strndup (file, candidate_file - file - 1);

          for (p = sub_dir; *p != '\0'; p++)
            {
              if (*p == '-')
                *p = G_DIR_SEPARATOR;
            }
        }
      g_free (sub_dir);
      data_dirs++;
    }

  *dirs = data_dirs;

  if (file_error)
    {
      if (fd >= 0)
        g_error_free (file_error);
      else
        g_propagate_error (error, file_error);
    }

  if (output_file && fd < 0)
    {
      g_free (*output_file);
      *output_file = NULL;
    }

  return fd;
}

static void
g_key_file_load_from_fd (GKeyFile       *key_file,
			 gint            fd,
			 GKeyFileFlags   flags,
			 GError        **error)
{
  GError *key_file_error = NULL;
  gsize bytes_read;
  struct stat stat_buf;
  gchar read_buf[4096];

  if (key_file->approximate_size > 0)
    {
      g_key_file_clear (key_file);
      g_key_file_init (key_file);
    }
  key_file->flags = flags;

  fstat (fd, &stat_buf);
  if (stat_buf.st_size == 0)
    {
      g_set_error (error, G_KEY_FILE_ERROR,
                   G_KEY_FILE_ERROR_PARSE,
                   _("File is empty"));
      return;
    }

  key_file->start_group_name = NULL;

  bytes_read = 0;
  do
    {
      bytes_read = read (fd, read_buf, 4096);

      if (bytes_read == 0)  /* End of File */
        break;

      if (bytes_read < 0)
        {
          if (errno == EINTR)
            continue;

          g_set_error (error, G_FILE_ERROR,
                       g_file_error_from_errno (errno),
                       _("Failed to read from file: %s"),
                       g_strerror (errno));
          close (fd);
          return;
        }

      g_key_file_parse_data (key_file, 
			     read_buf, bytes_read,
			     &key_file_error);
    }
  while (!key_file_error);

  close (fd);

  if (key_file_error)
    {
      g_propagate_error (error, key_file_error);
      return;
    }

  g_key_file_flush_parse_buffer (key_file, &key_file_error);

  if (key_file_error)
    {
      g_propagate_error (error, key_file_error);
      return;
    }
}

/**
 * g_key_file_load_from_file:
 * @key_file: an empty #GKeyFile struct
 * @file: the path of a filename to load
 * @flags: flags from #GKeyFileFlags
 * @error: return location for a #GError, or %NULL
 *
 * Loads a key file into an empty #GKeyFile structure.
 * If the file could not be loaded then %error is set to 
 * either a #GFileError or #GKeyFileError.
 *
 * Since: 2.6
 **/
void
g_key_file_load_from_file (GKeyFile       *key_file,
			   const gchar    *file,
			   GKeyFileFlags   flags,
			   GError        **error)
{
  GError *key_file_error = NULL;
  gint fd;

  fd = open (file, O_RDONLY);

  if (fd < 0)
    {
      g_set_error (error, G_FILE_ERROR,
                   g_file_error_from_errno (errno),
                   _("Failed to open file '%s': %s"),
                   file, g_strerror (errno));
      return;
    }

  if (!g_file_test (file, G_FILE_TEST_IS_REGULAR))
    {
      g_set_error (error, G_FILE_ERROR,
                   G_FILE_ERROR_ISDIR,
                   _("Not a regular file: '%s'"),
                   file);
      return;
    }

  g_key_file_load_from_fd (key_file, fd, flags, &key_file_error);

  if (key_file_error)
    g_propagate_error (error, key_file_error);
}

/**
 * g_key_file_load_from_data:
 * @key_file: an empty #GKeyFile struct
 * @data: key file loaded in memory.
 * @length: the length of @data in bytes
 * @flags: flags from #GKeyFileFlags
 * @error: return location for a #GError, or %NULL
 *
 * Loads a key file from memory into an empty #GKeyFile structure.  
 * If the object cannot be created then %error is set to a #GKeyFileError.
 *
 * Since: 2.6
 **/
void
g_key_file_load_from_data (GKeyFile       *key_file,
			   const gchar    *data,
			   gsize           length,
			   GKeyFileFlags   flags,
			   GError        **error)
{
  GError *key_file_error = NULL;

  g_return_if_fail (data != NULL);
  g_return_if_fail (length != 0);

  if (key_file->approximate_size > 0)
    {
      g_key_file_clear (key_file);
      g_key_file_init (key_file);
    }
  key_file->flags = flags;

  g_key_file_parse_data (key_file, data, length, &key_file_error);
  
  if (key_file_error)
    {
      g_propagate_error (error, key_file_error);
      return;
    }

  g_key_file_flush_parse_buffer (key_file, &key_file_error);
  
  if (key_file_error)
    g_propagate_error (error, key_file_error);
}

/**
 * g_key_file_load_from_data_dirs:
 * @key_file: an empty #GKeyFile struct
 * @file: a relative path to a filename to open and parse
 * @full_path: return location for a string containing the full path
 *   of the file, or %NULL
 * @flags: flags from #GKeyFileFlags 
 * @error: return location for a #GError, or %NULL
 *
 * This function looks for a key file named @file in the paths 
 * returned from g_get_user_data_dir() and g_get_system_data_dirs(), 
 * loads the file into @key_file and returns the file's full path in 
 * @full_path.  If the file could not be loaded then an %error is
 * set to either a #GFileError or #GKeyFileError.
 *
 * Since: 2.6
 **/
void
g_key_file_load_from_data_dirs (GKeyFile       *key_file,
				const gchar    *file,
				gchar         **full_path,
				GKeyFileFlags   flags,
				GError        **error)
{
  GError *key_file_error = NULL;
  gchar **all_data_dirs, **data_dirs;
  const gchar * user_data_dir;
  const const gchar * const * system_data_dirs;
  gsize i, j;
  gchar *output_path;
  gint fd;
  
  g_return_if_fail (!g_path_is_absolute (file));

  user_data_dir = g_get_user_data_dir ();
  system_data_dirs = g_get_system_data_dirs ();
  all_data_dirs = g_new (gchar *, g_strv_length ((gchar **)system_data_dirs) + 1);

  i = 0;
  all_data_dirs[i++] = g_strdup (user_data_dir);

  j = 0;
  while (system_data_dirs[j] != NULL)
    all_data_dirs[i++] = g_strdup (system_data_dirs[j++]);

  data_dirs = all_data_dirs;
  while (*data_dirs != NULL)
    {
      fd = find_file_in_data_dirs (file, &output_path, &data_dirs, 
                                   &key_file_error);
      
      if (fd < 0)
        {
          if (key_file_error)
            g_propagate_error (error, key_file_error);

	  break;
        }

      g_key_file_load_from_fd (key_file, fd, flags,
			       &key_file_error);
      
      if (key_file_error)
        {
	  g_propagate_error (error, key_file_error);
          g_free (output_path);

	  break;
        }
      
      if (full_path)
	*full_path = output_path;
    }

  g_strfreev (all_data_dirs);
}

/**
 * g_key_file_free:
 * @key_file: a #GKeyFile
 *
 * Frees a #GKeyFile.
 *
 * Since: 2.6
 **/
void
g_key_file_free (GKeyFile *key_file)
{
  g_return_if_fail (key_file != NULL);
  
  g_key_file_clear (key_file);
  g_free (key_file);
}

/* If G_KEY_FILE_KEEP_TRANSLATIONS is not set, only returns
 * true for locales that match those in g_get_language_names().
 */
static gboolean
g_key_file_locale_is_interesting (GKeyFile    *key_file,
				  const gchar *locale)
{
  const const gchar * const * current_locales;
  gsize i;

  if (key_file->flags & G_KEY_FILE_KEEP_TRANSLATIONS)
    return TRUE;

  current_locales = g_get_language_names ();

  for (i = 0; current_locales[i] != NULL; i++)
    {
      if (g_ascii_strcasecmp (current_locales[i], locale) == 0)
	return TRUE;
    }

  return FALSE;
}

static void
g_key_file_parse_line (GKeyFile     *key_file,
		       const gchar  *line,
		       gsize         length,
		       GError      **error)
{
  GError *parse_error = NULL;
  gchar *line_start;

  g_return_if_fail (key_file != NULL);
  g_return_if_fail (line != NULL);

  line_start = (gchar *) line;
  while (g_ascii_isspace (*line_start))
    line_start++;

  if (g_key_file_line_is_comment (line_start))
    g_key_file_parse_comment (key_file, line, length, &parse_error);
  else if (g_key_file_line_is_group (line_start))
    g_key_file_parse_group (key_file, line_start,
			    length - (line_start - line),
			    &parse_error);
  else if (g_key_file_line_is_key_value_pair (line_start))
    g_key_file_parse_key_value_pair (key_file, line_start,
				     length - (line_start - line),
				     &parse_error);
  else
    {
      g_set_error (error, G_KEY_FILE_ERROR,
                   G_KEY_FILE_ERROR_PARSE,
                   _("Key file contains line '%s' which is not "
                     "a key-value pair, group, or comment"), line);
      return;
    }

  if (parse_error)
    g_propagate_error (error, parse_error);
}

static void
g_key_file_parse_comment (GKeyFile     *key_file,
			  const gchar  *line,
			  gsize         length,
			  GError      **error)
{
  GKeyFileKeyValuePair *pair;
  
  if (!key_file->flags & G_KEY_FILE_KEEP_COMMENTS)
    return;
  
  g_assert (key_file->current_group != NULL);
  
  pair = g_new0 (GKeyFileKeyValuePair, 1);
  
  pair->key = NULL;
  pair->value = g_strndup (line, length);
  
  key_file->current_group->key_value_pairs =
    g_list_prepend (key_file->current_group->key_value_pairs, pair);
}

static void
g_key_file_parse_group (GKeyFile     *key_file,
			const gchar  *line,
			gsize         length,
			GError      **error)
{
  gchar *group_name;
  const gchar *group_name_start, *group_name_end;
  
  /* advance past opening '['
   */
  group_name_start = line + 1;
  group_name_end = line + length - 1;
  
  while (*group_name_end != ']')
    group_name_end--;

  group_name = g_strndup (group_name_start, 
                          group_name_end - group_name_start);
  
  if (key_file->start_group_name == NULL)
    key_file->start_group_name = g_strdup (group_name);
  
  g_key_file_add_group (key_file, group_name);
  g_free (group_name);
}

static void
g_key_file_parse_key_value_pair (GKeyFile     *key_file,
				 const gchar  *line,
				 gsize         length,
				 GError      **error)
{
  gchar *key, *value, *key_end, *value_start, *locale;
  gsize key_len, value_len;

  if (key_file->current_group == NULL || key_file->current_group->name == NULL)
    {
      g_set_error (error, G_KEY_FILE_ERROR,
		   G_KEY_FILE_ERROR_GROUP_NOT_FOUND,
		   _("Key file does not start with a group"));
      return;
    }

  key_end = value_start = strchr (line, '=');

  g_assert (key_end != NULL);

  key_end--;
  value_start++;

  /* Pull the key name from the line (chomping trailing whitespace)
   */
  while (g_ascii_isspace (*key_end))
    key_end--;

  key_len = key_end - line + 2;

  g_assert (key_len <= length);

  key = g_strndup (line, key_len - 1);

  /* Pull the value from the line (chugging leading whitespace)
   */
  while (g_ascii_isspace (*value_start))
    value_start++;

  value_len = line + length - value_start + 1;

  value = g_strndup (value_start, value_len);

  if (!g_utf8_validate (value, -1, NULL))
    {
      g_set_error (error, G_KEY_FILE_ERROR,
                   G_KEY_FILE_ERROR_UNKNOWN_ENCODING,
                   _("Key file contains line '%s' "
                     "which is not UTF-8"), line);

      g_free (key);
      g_free (value);
      return;
    }

  if (key_file->current_group
      && key_file->current_group->name
      && strcmp (g_key_file_get_start_group (key_file),
                 key_file->current_group->name) == 0
      && strcmp (key, "Encoding") == 0)
    {
      if (g_ascii_strcasecmp (value, "UTF-8") != 0)
        {
          g_set_error (error, G_KEY_FILE_ERROR,
                       G_KEY_FILE_ERROR_UNKNOWN_ENCODING,
                       _("Key file contains unsupported encoding '%s'"), value);

          g_free (key);
          g_free (value);
          return;
        }
    }

  /* Is this key a translation? If so, is it one that we care about?
   */
  locale = key_get_locale (key);

  if (locale == NULL || g_key_file_locale_is_interesting (key_file, locale))
    g_key_file_add_key (key_file, key_file->current_group->name, key, value);

  g_free (locale);
  g_free (key);
  g_free (value);
}

static gchar *
key_get_locale (const gchar *key)
{
  gchar *locale;

  locale = g_strrstr (key, "[");

  if (locale && strlen (locale) <= 2)
    locale = NULL;

  if (locale)
    locale = g_strndup (locale + 1, strlen (locale) - 2);

  return locale;
}

static void
g_key_file_parse_data (GKeyFile     *key_file,
		       const gchar  *data,
		       gsize         length,
		       GError      **error)
{
  GError *parse_error;
  gsize i;

  g_return_if_fail (key_file != NULL);

  parse_error = NULL;

  for (i = 0; i < length; i++)
    {
      if (data[i] == '\n')
        {
          /* When a newline is encountered flush the parse buffer so that the
           * line can be parsed.  Note that completely blank lines won't show
           * up in the parse buffer, so they get parsed directly.
           */
          if (key_file->parse_buffer->len > 0)
            g_key_file_flush_parse_buffer (key_file, &parse_error);
          else
            g_key_file_parse_comment (key_file, "", 1, &parse_error);

          if (parse_error)
            {
              g_propagate_error (error, parse_error);
              return;
            }
        }
      else
        g_string_append_c (key_file->parse_buffer, data[i]);
    }

  key_file->approximate_size += length;
}

static void
g_key_file_flush_parse_buffer (GKeyFile  *key_file,
			       GError   **error)
{
  GError *file_error = NULL;

  g_return_if_fail (key_file != NULL);

  file_error = NULL;

  if (key_file->parse_buffer->len > 0)
    {
      g_key_file_parse_line (key_file, key_file->parse_buffer->str,
			     key_file->parse_buffer->len,
			     &file_error);
      g_string_erase (key_file->parse_buffer, 0, -1);

      if (file_error)
        {
          g_propagate_error (error, file_error);
          return;
        }
    }
}

/**
 * g_key_file_to_data:
 * @key_file: a #GKeyFile
 * @length: return location for the length of the 
 *   returned string, or %NULL
 * @error: return location for a #GError, or %NULL
 *
 * This function outputs @key_file as a string.  
 *
 * Return value: a newly allocated string holding
 *   the contents of the #GKeyFile 
 *
 * Since: 2.6
 **/
gchar *
g_key_file_to_data (GKeyFile  *key_file,
		    gsize     *length,
		    GError   **error)
{
  GString *data_string;
  gchar *data;
  GList *group_node, *key_file_node;

  g_return_val_if_fail (key_file != NULL, NULL);

  data_string = g_string_sized_new (2 * key_file->approximate_size);
  
  for (group_node = g_list_last (key_file->groups);
       group_node != NULL;
       group_node = group_node->prev)
    {
      GKeyFileGroup *group;

      group = (GKeyFileGroup *) group_node->data;

      if (group->name != NULL)
        g_string_append_printf (data_string, "[%s]\n", group->name);

      for (key_file_node = g_list_last (group->key_value_pairs);
           key_file_node != NULL;
           key_file_node = key_file_node->prev)
        {
          GKeyFileKeyValuePair *pair;

          pair = (GKeyFileKeyValuePair *) key_file_node->data;

          if (pair->key != NULL)
            g_string_append_printf (data_string, "%s=%s\n", pair->key, pair->value);
          else
            g_string_append_printf (data_string, "%s\n", pair->value);
        }
    }

  if (length)
    *length = data_string->len;

  data = data_string->str;

  g_string_free (data_string, FALSE);

  return data;
}

/**
 * g_key_file_get_keys:
 * @key_file: a #GKeyFile
 * @group_name: a group name, or %NULL
 * @length: return location for the number of keys returned, or %NULL
 * @error: return location for a #GError, or %NULL
 *
 * Returns all keys for the group name @group_name. If @group_name is
 * %NULL, the start group is used. The array of returned keys will be 
 * %NULL-terminated, so @length may optionally be %NULL.
 *
 * Return value: a newly-allocated %NULL-terminated array of
 * strings. Use g_strfreev() to free it.
 *
 * Since: 2.6
 **/
gchar **
g_key_file_get_keys (GKeyFile     *key_file,
		     const gchar  *group_name,
		     gsize        *length,
		     GError      **error)
{
  GKeyFileGroup *group;
  GList *tmp;
  gchar **keys;
  gsize i, num_keys;
  
  g_return_val_if_fail (key_file != NULL, NULL);
  
  if (group_name == NULL)
    group_name = (const gchar *) key_file->start_group_name;
  
  group = g_key_file_lookup_group (key_file, group_name);
  
  if (!group)
    {
      g_set_error (error, G_KEY_FILE_ERROR,
                   G_KEY_FILE_ERROR_GROUP_NOT_FOUND,
                   _("Key file does not have group '%s'"),
                   group_name);
      return NULL;
    }

  num_keys = g_list_length (group->key_value_pairs);
  
  keys = (gchar **) g_new (gchar **, num_keys + 1);

  tmp = group->key_value_pairs;
  for (i = 0; i < num_keys; i++)
    {
      GKeyFileKeyValuePair *pair;

      pair = (GKeyFileKeyValuePair *) tmp->data;
      keys[i] = g_strdup (pair->key);

      tmp = tmp->next;
    }
  keys[i] = NULL;

  if (length)
    *length = num_keys;

  return keys;
}

/**
 * g_key_file_get_start_group:
 * @key_file: a #GKeyFile
 *
 * Returns the name of the start group of the file. 
 *
 * Return value: The start group of the key file.
 *
 * Since: 2.6
 **/
gchar *
g_key_file_get_start_group (GKeyFile *key_file)
{
  g_return_val_if_fail (key_file != NULL, NULL);

  if (key_file->start_group_name)
    return g_strdup (key_file->start_group_name);
  else
    return NULL;
}

/**
 * g_key_file_get_groups:
 * @key_file: a #GKeyFile
 * @length: return location for the number of returned groups, or %NULL
 *
 * Returns all groups in the key file loaded with @key_file.  The
 * array of returned groups will be %NULL-terminated, so @length may
 * optionally be %NULL.
 *
 * Return value: a newly-allocated %NULL-terminated array of strings. 
 *   Use g_strfreev() to free it.
 * Since: 2.6
 **/
gchar **
g_key_file_get_groups (GKeyFile *key_file,
		       gsize    *length)
{
  GList *tmp;
  gchar **groups;
  gsize i, num_groups;

  g_return_val_if_fail (key_file != NULL, NULL);

  num_groups = g_list_length (key_file->groups);
  groups = (gchar **) g_new (gchar **, num_groups + 1);

  tmp = key_file->groups;
  for (i = 0; i < num_groups; i++)
    {
      GKeyFileGroup *group;

      group = (GKeyFileGroup *) tmp->data;
      groups[i] = g_strdup (group->name);

      tmp = tmp->next;
    }
  groups[i] = NULL;

  if (length)
    *length = num_groups;

  return groups;
}

/**
 * g_key_file_get_value:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key
 * @error: return location for a #GError, or #NULL
 *
 * Returns the value associated with @key under @group_name.  
 * In the event the key cannot be found, %NULL is returned and 
 * @error is set to #G_KEY_FILE_ERROR_KEY_NOT_FOUND.  In the 
 * event that the @group_name cannot be found, %NULL is returned 
 * and @error is set to #G_KEY_FILE_ERROR_GROUP_NOT_FOUND.
 *
 * Return value: a string or %NULL if the specified key cannot be
 * found.
 *
 * Since: 2.6
 **/
gchar *
g_key_file_get_value (GKeyFile     *key_file,
		      const gchar  *group_name,
		      const gchar  *key,
		      GError      **error)
{
  GKeyFileGroup *group;
  GKeyFileKeyValuePair *pair;
  gchar *value = NULL;

  if (group_name == NULL)
    group_name = (const gchar *) key_file->start_group_name;
  
  group = g_key_file_lookup_group (key_file, group_name);

  if (!group)
    {
      g_set_error (error, G_KEY_FILE_ERROR,
                   G_KEY_FILE_ERROR_GROUP_NOT_FOUND,
                   _("Key file does not have group '%s'"),
                   group_name);
      return NULL;
    }

  pair = g_key_file_lookup_key_value_pair (key_file, group, key);

  if (pair)
    value = g_strdup (pair->value);
  else
    g_set_error (error, G_KEY_FILE_ERROR,
                 G_KEY_FILE_ERROR_KEY_NOT_FOUND,
                 _("Key file does not have key '%s'"), key);

  return value;
}

/**
 * g_key_file_set_value:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key
 * @value: a string
 *
 * Associates a new value with @key under @group_name.
 * If @key cannot be found then it is created. If @group_name
 * cannot be found then it is created as well.
 *
 * Since: 2.6
 **/
void
g_key_file_set_value (GKeyFile    *key_file,
		      const gchar *group_name,
		      const gchar *key,
		      const gchar *value)
{
  GKeyFileGroup *group;
  GKeyFileKeyValuePair *pair;

  pair = NULL;

  if (group_name == NULL)
    group_name = (const gchar *) key_file->start_group_name;

  if (!g_key_file_has_key (key_file, group_name, key, NULL))
    g_key_file_add_key (key_file, group_name, key, value);
  else
    {
      group = g_key_file_lookup_group (key_file, group_name);
      pair = g_key_file_lookup_key_value_pair (key_file, group, key);
      g_free (pair->value);
      pair->value = g_strdup (value);
    }
}

/**
 * g_key_file_get_string:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key
 * @error: return location for a #GError, or #NULL
 *
 * Returns the value associated with @key under @group_name.  
 * In the event the key cannot be found, %NULL is returned and 
 * @error is set to #G_KEY_FILE_ERROR_KEY_NOT_FOUND.  In the 
 * event that the @group_name cannot be found, %NULL is returned 
 * and @error is set to #G_KEY_FILE_ERROR_GROUP_NOT_FOUND.
 *
 * Return value: a string or %NULL if the specified key cannot be
 * found.
 *
 * Since: 2.6
 **/
gchar *
g_key_file_get_string (GKeyFile     *key_file,
		       const gchar  *group_name,
		       const gchar  *key,
		       GError      **error)
{
  gchar *value, *string_value;
  GError *key_file_error;

  g_return_val_if_fail (key_file != NULL, NULL);
  g_return_val_if_fail (key != NULL, NULL);

  key_file_error = NULL;

  value = g_key_file_get_value (key_file, group_name, key, &key_file_error);

  if (key_file_error)
    {
      g_propagate_error (error, key_file_error);
      return NULL;
    }

  string_value = g_key_file_parse_value_as_string (key_file, value, NULL,
						   &key_file_error);
  g_free (value);

  if (key_file_error)
    {
      if (g_error_matches (key_file_error,
                           G_KEY_FILE_ERROR,
                           G_KEY_FILE_ERROR_INVALID_VALUE))
        {
          g_set_error (error, G_KEY_FILE_ERROR,
                       G_KEY_FILE_ERROR_INVALID_VALUE,
                       _("Key file contains key '%s' "
                         "which has value that cannot be interpreted."),
                       key);
          g_error_free (key_file_error);
        }
      else
        g_propagate_error (error, key_file_error);
    }

  return string_value;
}

/**
 * g_key_file_set_string:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key
 * @string: a string
 *
 * Associates a new string value with @key under @group_name.
 * If @key cannot be found then it is created. If @group_name
 * cannot be found then it is created as well.
 *
 * Since: 2.6
 **/
void
g_key_file_set_string (GKeyFile    *key_file,
		       const gchar *group_name,
		       const gchar *key,
		       const gchar *string)
{
  gchar *value;

  g_return_if_fail (key_file != NULL);
  g_return_if_fail (key != NULL);
  
  value = g_key_file_parse_string_as_value (key_file, string, FALSE);
  g_key_file_set_value (key_file, group_name, key, value);
  g_free (value);
}

/**
 * g_key_file_get_string_list:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key
 * @length: return location for the number of returned strings, or %NULL
 * @error: return location for a #GError, or %NULL
 *
 * Returns the values associated with @key under @group_name.
 * In the event the key cannot be found, %NULL is returned and
 * @error is set to #G_KEY_FILE_ERROR_KEY_NOT_FOUND.  In the
 * event that the @group_name cannot be found, %NULL is returned
 * and @error is set to #G_KEY_FILE_ERROR_GROUP_NOT_FOUND.
 *
 * Return value: a %NULL-terminated string array or %NULL if the specified 
 *   key cannot be found. The array should be freed with g_strfreev().
 *
 * Since: 2.6
 **/
gchar **
g_key_file_get_string_list (GKeyFile     *key_file,
			    const gchar  *group_name,
			    const gchar  *key,
			    gsize        *length,
			    GError      **error)
{
  GError *key_file_error = NULL;
  gchar *value, *string_value, **values;
  gint i, len;
  GSList *p, *pieces = NULL;

  value = g_key_file_get_value (key_file, group_name, key, &key_file_error);

  if (key_file_error)
    {
      g_propagate_error (error, key_file_error);
      return NULL;
    }

  string_value = g_key_file_parse_value_as_string (key_file, value, &pieces, &key_file_error);
  g_free (value);
  g_free (string_value);

  if (key_file_error)
    {
      if (g_error_matches (key_file_error,
                           G_KEY_FILE_ERROR,
                           G_KEY_FILE_ERROR_INVALID_VALUE))
        {
          g_set_error (error, G_KEY_FILE_ERROR,
                       G_KEY_FILE_ERROR_INVALID_VALUE,
                       _("Key file contains key '%s' "
                         "which has value that cannot be interpreted."),
                       key);
          g_error_free (key_file_error);
        }
      else
        g_propagate_error (error, key_file_error);
    }

  len = g_slist_length (pieces);
  values = g_new (gchar *, len + 1); 
  for (p = pieces, i = 0; p; p = p->next)
    values[i++] = p->data;
  values[len] = NULL;

  g_slist_free (pieces);

  if (length)
    *length = len;

  return values;
}

/**
 * g_key_file_set_string_list:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key
 * @list: an array of locale string values
 * @length: number of locale string values in @list
 *
 * Associates a list of string values for @key under @group_name.
 * If the @key cannot be found then it is created.
 *
 * Since: 2.6
 **/
void
g_key_file_set_string_list (GKeyFile            *key_file,
			    const gchar         *group_name,
			    const gchar         *key,
			    const gchar * const  list[],
			    gsize                length)
{
  GString *value_list;
  gsize i;

  g_return_if_fail (key_file != NULL);
  g_return_if_fail (key != NULL);

  value_list = g_string_sized_new (length * 128);
  for (i = 0; list[i] != NULL && i < length; i++)
    {
      gchar *value;

      value = g_key_file_parse_string_as_value (key_file, list[i], TRUE);
      g_string_append (value_list, value);
      g_string_append_c (value_list, key_file->list_separator);

      g_free (value);
    }

  g_key_file_set_value (key_file, group_name, key, value_list->str);
  g_string_free (value_list, TRUE);
}

/**
 * g_key_file_set_locale_string:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key
 * @locale: a locale
 * @string: a string
 *
 * Associates a string value for @key and @locale under
 * @group_name.  If the translation for @key cannot be found 
 * then it is created.
 *
 * Since: 2.6
 **/
void
g_key_file_set_locale_string (GKeyFile     *key_file,
			      const gchar  *group_name,
			      const gchar  *key,
			      const gchar  *locale,
			      const gchar  *string)
{
  gchar *full_key, *value;

  g_return_if_fail (key_file != NULL);
  g_return_if_fail (key != NULL);

  value = g_key_file_parse_string_as_value (key_file, string, FALSE);
  full_key = g_strdup_printf ("%s[%s]", key, locale);
  g_key_file_set_value (key_file, group_name, full_key, value);
  g_free (full_key);
  g_free (value);
}

extern GSList *_g_compute_locale_variants (const gchar *locale);

/**
 * g_key_file_get_locale_string:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key
 * @locale: a locale or %NULL
 * @error: return location for a #GError, or %NULL
 *
 * Returns the value associated with @key under @group_name
 * translated in the given @locale if available.  If @locale is
 * %NULL then the current locale is assumed. If @key cannot be
 * found then %NULL is returned and @error is set to
 * #G_KEY_FILE_ERROR_KEY_NOT_FOUND. If the value associated
 * with @key cannot be interpreted or no suitable translation can
 * be found then the untranslated value is returned and @error is
 * set to #G_KEY_FILE_ERROR_INVALID_VALUE and
 * #G_KEY_FILE_ERROR_KEY_NOT_FOUND, respectively. In the
 * event that the @group_name cannot be found, %NULL is returned
 * and @error is set to #G_KEY_FILE_ERROR_GROUP_NOT_FOUND.
 *
 * Return value: a string or %NULL if the specified key cannot be
 *               found.
 * Since: 2.6
 **/
gchar *
g_key_file_get_locale_string (GKeyFile     *key_file,
			      const gchar  *group_name,
			      const gchar  *key,
			      const gchar  *locale,
			      GError      **error)
{
  gchar *candidate_key, *translated_value;
  GError *key_file_error;
  gchar **languages;
  gboolean free_languages = FALSE;
  gint i;

  candidate_key = NULL;
  translated_value = NULL;
  key_file_error = NULL;

  if (!g_key_file_has_group (key_file, group_name))
    {
      g_set_error (error, G_KEY_FILE_ERROR,
                   G_KEY_FILE_ERROR_GROUP_NOT_FOUND,
                   _("Key file does not have group '%s'"),
                   group_name);
      return NULL;
    }

  if (locale)
    {
      GSList *l, *list;

      list = _g_compute_locale_variants (locale);

      languages = g_new0 (gchar *, g_slist_length (list) + 1);
      for (l = list, i = 0; l; l = l->next, i++)
	languages[i] = l->data;
      languages[i] = NULL;

      g_slist_free (list);
      free_languages = TRUE;
    }
  else
    {
      languages = (gchar **) g_get_language_names ();
      free_languages = FALSE;
    }
  
  for (i = 0; languages[i]; i++)
    {
      candidate_key = g_strdup_printf ("%s[%s]", key, languages[i]);
      
      translated_value = g_key_file_get_string (key_file,
						group_name,
						candidate_key, NULL);
      g_free (candidate_key);

      if (translated_value)
	break;
   }

  if (translated_value && !g_utf8_validate (translated_value, -1, NULL))
    {
      g_set_error (error, G_KEY_FILE_ERROR,
		   G_KEY_FILE_ERROR_INVALID_VALUE,
		   _("Key file contains key '%s' "
		     "which has value that cannot be interpreted."),
		   candidate_key);
      g_free (translated_value);
      translated_value = NULL;
  }

  /* Fallback to untranslated key
   */
  if (!translated_value)
    {
      translated_value = g_key_file_get_string (key_file, group_name, key,
						&key_file_error);
      
      if (!translated_value)
        g_propagate_error (error, key_file_error);
      else
        g_set_error (error, G_KEY_FILE_ERROR,
                     G_KEY_FILE_ERROR_KEY_NOT_FOUND,
                     _("Key file contains no translated value "
                       "for key '%s' with locale '%s'."),
                     key, locale);
    }

  if (free_languages)
    g_strfreev (languages);

  return translated_value;
}

/** 
 * g_key_file_get_locale_string_list: 
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key
 * @locale: a locale
 * @length: return location for the number of returned strings or %NULL
 * @error: return location for a #GError or %NULL
 *
 * Returns the values associated with @key under @group_name
 * translated in the given @locale if available.  If @locale is
 * %NULL then the current locale is assumed. If @key cannot be
 * found then %NULL is returned and @error is set to
 * #G_KEY_FILE_ERROR_KEY_NOT_FOUND. If the values associated
 * with @key cannot be interpreted or no suitable translations
 * can be found then the untranslated values are returned and
 * @error is set to #G_KEY_FILE_ERROR_INVALID_VALUE and
 * #G_KEY_FILE_ERROR_KEY_NOT_FOUND, respectively. In the
 * event that the @group_name cannot be found, %NULL is returned
 * and @error is set to #G_KEY_FILE_ERROR_GROUP_NOT_FOUND.
 * The returned array is %NULL-terminated, so @length may optionally be %NULL.
 *
 * Return value: a newly allocated %NULL-terminated string array
 *   or %NULL if the key isn't found. The string array should be freed
 *   with g_strfreev().
 *
 * Since: 2.6
 **/
gchar **
g_key_file_get_locale_string_list (GKeyFile     *key_file,
				   const gchar  *group_name,
				   const gchar  *key,
				   const gchar  *locale,
				   gsize        *length,
				   GError      **error)
{
  GError *key_file_error;
  gchar **values, *value;

  key_file_error = NULL;

  value = g_key_file_get_locale_string (key_file, group_name, 
					key, locale,
					&key_file_error);
  
  if (key_file_error)
    g_propagate_error (error, key_file_error);
  
  if (!value)
    return NULL;

  if (value[strlen (value) - 1] == ';')
    value[strlen (value) - 1] = '\0';

  values = g_strsplit (value, ";", 0);

  g_free (value);

  if (length)
    *length = g_strv_length (values);

  return values;
}

/**
 * g_key_file_set_locale_string_list:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key
 * @locale: a locale
 * @list: a %NULL-terminated array of locale string values
 * @length: the length of @list
 *
 * Associates a list of string values for @key and @locale under
 * @group_name.  If the translation for @key cannot be found then
 * it is created.
 *
 * Since: 2.6
 **/
void
g_key_file_set_locale_string_list (GKeyFile            *key_file,
				   const gchar         *group_name,
				   const gchar         *key,
				   const gchar         *locale,
				   const gchar * const  list[],
				   gsize                length)
{
  GString *value_list;
  gchar *full_key;
  gsize i;

  g_return_if_fail (key_file != NULL);
  g_return_if_fail (key != NULL);

  value_list = g_string_sized_new (length * 128);
  for (i = 0; list[i] != NULL && i < length; i++)
    {
      gchar *value;
      
      value = g_key_file_parse_string_as_value (key_file, list[i], TRUE);
      
      g_string_append (value_list, value);
      g_string_append_c (value_list, ';');

      g_free (value);
    }

  full_key = g_strdup_printf ("%s[%s]", key, locale);
  g_key_file_set_value (key_file, group_name, full_key, value_list->str);
  g_free (full_key);
  g_string_free (value_list, TRUE);
}

/**
 * g_key_file_get_boolean:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key
 * @error: return location for a #GError
 *
 * Returns the value associated with @key under @group_name as a
 * boolean.  If @key cannot be found then the return value is
 * undefined and @error is set to
 * #G_KEY_FILE_ERROR_KEY_NOT_FOUND. Likewise, if the value
 * associated with @key cannot be interpreted as a boolean then
 * the return value is also undefined and @error is set to
 * #G_KEY_FILE_ERROR_INVALID_VALUE.
 *
 * Return value: the value associated with the key as a boolean
 * Since: 2.6
 **/
gboolean
g_key_file_get_boolean (GKeyFile     *key_file,
			const gchar  *group_name,
			const gchar  *key,
			GError      **error)
{
  GError *key_file_error = NULL;
  gchar *value;
  gboolean bool_value;

  value = g_key_file_get_value (key_file, group_name, key, &key_file_error);

  if (!value)
    {
      g_propagate_error (error, key_file_error);
      return FALSE;
    }

  bool_value = g_key_file_parse_value_as_boolean (key_file, value,
						  &key_file_error);
  g_free (value);

  if (key_file_error)
    {
      if (g_error_matches (key_file_error,
                           G_KEY_FILE_ERROR,
                           G_KEY_FILE_ERROR_INVALID_VALUE))
        {
          g_set_error (error, G_KEY_FILE_ERROR,
                       G_KEY_FILE_ERROR_INVALID_VALUE,
                       _("Key file contains key '%s' "
                         "which has value that cannot be interpreted."),
                       key);
          g_error_free (key_file_error);
        }
      else
        g_propagate_error (error, key_file_error);
    }

  return bool_value;
}

/**
 * g_key_file_set_boolean:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key
 * @value: %TRUE or %FALSE
 *
 * Associates a new boolean value with @key under @group_name.
 * If @key cannot be found then it is created.
 *
 * Since: 2.6
 **/
void
g_key_file_set_boolean (GKeyFile    *key_file,
			const gchar *group_name,
			const gchar *key,
			gboolean     value)
{
  gchar *result;

  g_return_if_fail (key_file != NULL);
  g_return_if_fail (key != NULL);

  result = g_key_file_parse_boolean_as_value (key_file, value);
  g_key_file_set_value (key_file, group_name, key, result);
  g_free (result);
}

/**
 * g_key_file_get_boolean_list:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key
 * @length: the number of booleans returned
 * @error: return location for a #GError
 *
 * Returns the values associated with @key under @group_name as
 * booleans.  If @key cannot be found then the return value is
 * undefined and @error is set to
 * #G_KEY_FILE_ERROR_KEY_NOT_FOUND. Likewise, if the values
 * associated with @key cannot be interpreted as booleans then
 * the return value is also undefined and @error is set to
 * #G_KEY_FILE_ERROR_INVALID_VALUE.
 *
 * Return value: the values associated with the key as a boolean
 **/
gboolean *
g_key_file_get_boolean_list (GKeyFile     *key_file,
			     const gchar  *group_name,
			     const gchar  *key,
			     gsize        *length,
			     GError      **error)
{
  GError *key_file_error;
  gchar **values;
  gboolean *bool_values;
  gsize i, num_bools;

  key_file_error = NULL;

  values = g_key_file_get_string_list (key_file, group_name, key,
				       &num_bools, &key_file_error);

  if (key_file_error)
    g_propagate_error (error, key_file_error);

  if (!values)
    return NULL;

  bool_values = g_new (gboolean, num_bools);

  for (i = 0; i < num_bools; i++)
    {
      bool_values[i] = g_key_file_parse_value_as_boolean (key_file,
							  values[i],
							  &key_file_error);

      if (key_file_error)
        {
          g_propagate_error (error, key_file_error);
          g_strfreev (values);
          g_free (bool_values);

          return NULL;
        }
    }
  g_strfreev (values);

  if (length)
    *length = num_bools;

  return bool_values;
}

/**
 * g_key_file_set_boolean_list:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key
 * @list: an array of boolean values
 * @length: length of @list
 *
 * Associates a list of boolean values with @key under
 * @group_name.  If @key cannot be found then it is created.
 *
 * Since: 2.6
 **/
void
g_key_file_set_boolean_list (GKeyFile    *key_file,
			     const gchar *group_name,
			     const gchar *key,
			     gboolean     list[],
			     gsize        length)
{
  GString *value_list;
  gsize i;

  g_return_if_fail (key_file != NULL);
  g_return_if_fail (key != NULL);

  value_list = g_string_sized_new (length * 8);
  for (i = 0; i < length; i++)
    {
      gchar *value;

      value = g_key_file_parse_boolean_as_value (key_file, list[i]);

      g_string_append (value_list, value);
      g_string_append_c (value_list, key_file->list_separator);

      g_free (value);
    }

  g_key_file_set_value (key_file, group_name, key, value_list->str);
  g_string_free (value_list, TRUE);
}

/**
 * g_key_file_get_integer:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key
 * @error: return location for a #GError
 *
 * Returns the value associated with @key under @group_name as an
 * integer. If @key cannot be found then the return value is
 * undefined and @error is set to
 * #G_KEY_FILE_ERROR_KEY_NOT_FOUND. Likewise, if the value
 * associated with @key cannot be interpreted as an integer then
 * the return value is also undefined and @error is set to
 * #G_KEY_FILE_ERROR_INVALID_VALUE.
 *
 * Return value: the value associated with the key as an integer.
 * Since: 2.6
 **/
gint
g_key_file_get_integer (GKeyFile     *key_file,
			const gchar  *group_name,
			const gchar  *key,
			GError      **error)
{
  GError *key_file_error;
  gchar *value;
  gint int_value;

  key_file_error = NULL;

  value = g_key_file_get_value (key_file, group_name, key, &key_file_error);

  if (key_file_error)
    {
      g_propagate_error (error, key_file_error);
      return 0;
    }

  int_value = g_key_file_parse_value_as_integer (key_file, value,
						 &key_file_error);
  g_free (value);

  if (key_file_error)
    {
      if (g_error_matches (key_file_error,
                           G_KEY_FILE_ERROR,
                           G_KEY_FILE_ERROR_INVALID_VALUE))
        {
          g_set_error (error, G_KEY_FILE_ERROR,
                       G_KEY_FILE_ERROR_INVALID_VALUE,
                       _("Key file contains key '%s' in group '%s' "
                         "which has value that cannot be interpreted."), key, 
                       group_name);
          g_error_free (key_file_error);
        }
      else
        g_propagate_error (error, key_file_error);
    }

  return int_value;
}

/**
 * g_key_file_set_integer:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key
 * @value: an integer value
 *
 * Associates a new integer value with @key under @group_name.
 * If @key cannot be found then it is created.
 *
 * Since: 2.6
 **/
void
g_key_file_set_integer (GKeyFile    *key_file,
			const gchar *group_name,
			const gchar *key,
			gint         value)
{
  gchar *result;

  g_return_if_fail (key_file != NULL);
  g_return_if_fail (key != NULL);

  result = g_key_file_parse_integer_as_value (key_file, value);
  g_key_file_set_value (key_file, group_name, key, result);
  g_free (result);
}

/**
 * g_key_file_get_integer_list:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key
 * @length: the number of integers returned
 * @error: return location for a #GError
 *
 * Returns the values associated with @key under @group_name as
 * integers.  If @key cannot be found then the return value is
 * undefined and @error is set to
 * #G_KEY_FILE_ERROR_KEY_NOT_FOUND. Likewise, if the values
 * associated with @key cannot be interpreted as integers then
 * the return value is also undefined and @error is set to
 * #G_KEY_FILE_ERROR_INVALID_VALUE.
 *
 * Return value: the values associated with the key as a integer
 * Since: 2.6
 **/
gint *
g_key_file_get_integer_list (GKeyFile     *key_file,
			     const gchar  *group_name,
			     const gchar  *key,
			     gsize        *length,
			     GError      **error)
{
  GError *key_file_error = NULL;
  gchar **values;
  gint *int_values;
  gsize i, num_ints;

  values = g_key_file_get_string_list (key_file, group_name, key,
				       &num_ints, &key_file_error);

  if (key_file_error)
    g_propagate_error (error, key_file_error);

  if (!values)
    return NULL;

  int_values = g_new (gint, num_ints);

  for (i = 0; i < num_ints; i++)
    {
      int_values[i] = g_key_file_parse_value_as_integer (key_file,
							 values[i],
							 &key_file_error);

      if (key_file_error)
        {
          g_propagate_error (error, key_file_error);
          g_strfreev (values);
          g_free (int_values);

          return NULL;
        }
    }
  g_strfreev (values);

  if (length)
    *length = num_ints;

  return int_values;
}

/**
 * g_key_file_set_integer_list:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key
 * @list: an array of integer values
 * @length: number of integer values in @list
 *
 * Associates a list of integer values with @key under
 * @group_name.  If @key cannot be found then it is created.
 *
 * Since: 2.6
 **/
void
g_key_file_set_integer_list (GKeyFile     *key_file,
			     const gchar  *group_name,
			     const gchar  *key,
			     gint          list[],
			     gsize         length)
{
  GString *values;
  gsize i;

  g_return_if_fail (key_file != NULL);
  g_return_if_fail (key != NULL);

  values = g_string_sized_new (length * 16);
  for (i = 0; i < length; i++)
    {
      gchar *value;

      value = g_key_file_parse_integer_as_value (key_file, list[i]);

      g_string_append (values, value);
      g_string_append_c (values, ';');

      g_free (value);
    }

  g_key_file_set_value (key_file, group_name, key, values->str);
  g_string_free (values, TRUE);
}

/**
 * g_key_file_has_group:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 *
 * Looks whether the key file has the group @group_name.
 *
 * Return value: %TRUE if @group_name is a part of @key_file, %FALSE
 * otherwise.
 * Since: 2.6
 **/
gboolean
g_key_file_has_group (GKeyFile    *key_file,
		      const gchar *group_name)
{
  GList *tmp;
  GKeyFileGroup *group;

  g_return_val_if_fail (key_file != NULL, FALSE);
  g_return_val_if_fail (group_name != NULL, FALSE);

  for (tmp = key_file->groups; tmp != NULL; tmp = tmp->next)
    {
      group = (GKeyFileGroup *) tmp->data;
      if (group && group->name && (strcmp (group->name, group_name) == 0))
        return TRUE;
    }

  return FALSE;
}

/**
 * g_key_file_has_key:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key name
 * @error: return location for a #GError
 *
 * Looks whether the key file has the key @key in the group
 * @group_name.
 *
 * Return value: %TRUE if @key is a part of @group_name, %FALSE
 * otherwise.
 * Since: 2.6
 **/
gboolean
g_key_file_has_key (GKeyFile     *key_file,
		    const gchar  *group_name,
		    const gchar  *key,
		    GError      **error)
{
  GKeyFileKeyValuePair *pair;
  GKeyFileGroup *group;

  g_return_val_if_fail (key_file != NULL, FALSE);
  g_return_val_if_fail (key != NULL, FALSE);

  pair = NULL;

  if (group_name == NULL)
    group_name = (const gchar *) key_file->start_group_name;

  group = g_key_file_lookup_group (key_file, group_name);

  if (!group)
    {
      g_set_error (error, G_KEY_FILE_ERROR,
                   G_KEY_FILE_ERROR_GROUP_NOT_FOUND,
                   _("Key file does not have group '%s'"),
                   group_name);

      return FALSE;
    }

  pair = g_key_file_lookup_key_value_pair (key_file, group, key);

  return pair != NULL;
}

static void
g_key_file_add_group (GKeyFile    *key_file,
		      const gchar *group_name)
{
  GKeyFileGroup *group;

  g_return_if_fail (key_file != NULL);
  g_return_if_fail (group_name != NULL);
  g_return_if_fail (g_key_file_lookup_group (key_file, group_name) == NULL);

  group = g_new0 (GKeyFileGroup, 1);
  group->name = g_strdup (group_name);
  group->lookup_map = g_hash_table_new (g_str_hash, g_str_equal);
  key_file->groups = g_list_prepend (key_file->groups, group);
  key_file->approximate_size += strlen (group_name) + 3;
  key_file->current_group = group;

  if (key_file->start_group_name == NULL)
    key_file->start_group_name = g_strdup (group_name);
}


static void
g_key_file_key_value_pair_free (GKeyFileKeyValuePair *pair)
{
  if (pair != NULL)
    {
      g_free (pair->key);
      g_free (pair->value);
      g_free (pair);
    }
}

static void
g_key_file_remove_group_node (GKeyFile *key_file,
			      GList    *group_node)
{
  GKeyFileGroup *group;

  group = (GKeyFileGroup *) group_node->data;

  /* If the current group gets deleted make the current group the first
   * group.
   */
  if (key_file->current_group == group)
    {
      GList *first_group;

      first_group = key_file->groups;

      if (first_group)
        key_file->current_group = (GKeyFileGroup *) first_group->data;
      else
        key_file->current_group = NULL;
    }

  key_file->groups = g_list_remove_link (key_file->groups, group_node);

  if (group->name != NULL)
    key_file->approximate_size -= strlen (group->name) + 3;

  /* FIXME: approximate_size isn't getting updated for the
   * removed keys in group.  
   */
  g_list_foreach (group->key_value_pairs, 
                  (GFunc) g_key_file_key_value_pair_free, NULL);
  g_list_free (group->key_value_pairs);
  group->key_value_pairs = NULL;

  g_hash_table_destroy (group->lookup_map);
  group->lookup_map = NULL;

  g_free ((gchar *) group->name);
  g_free (group);
  g_list_free_1 (group_node);
}

/**
 * g_key_file_remove_group:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @error: return location for a #GError or %NULL
 *
 * Removes the specified group, @group_name, 
 * from the key file. 
 *
 * Since: 2.6
 **/
void
g_key_file_remove_group (GKeyFile     *key_file,
			 const gchar  *group_name,
			 GError      **error)
{
  GKeyFileGroup *group;
  GList *group_node;

  g_return_if_fail (key_file != NULL);
  g_return_if_fail (group_name != NULL);

  group = g_key_file_lookup_group (key_file, group_name);

  if (!group)
    g_set_error (error, G_KEY_FILE_ERROR,
		 G_KEY_FILE_ERROR_GROUP_NOT_FOUND,
		 _("Key file does not have group '%s'"),
		 group_name);

  group_node = g_list_find (key_file->groups, group);
  g_key_file_remove_group_node (key_file, group_node);
}

static void
g_key_file_add_key (GKeyFile    *key_file,
		    const gchar *group_name,
		    const gchar *key,
		    const gchar *value)
{
  GKeyFileGroup *group;
  GKeyFileKeyValuePair *pair;

  group = g_key_file_lookup_group (key_file, group_name);

  if (!group)
    {
      g_key_file_add_group (key_file, group_name);
      group = (GKeyFileGroup *) key_file->groups->data;
    }

  pair = g_new0 (GKeyFileKeyValuePair, 1);

  pair->key = g_strdup (key);
  pair->value =  g_strdup (value);

  g_hash_table_replace (group->lookup_map, pair->key, pair);
  group->key_value_pairs = g_list_prepend (group->key_value_pairs, pair);
  key_file->approximate_size += strlen (key) + strlen (value) + 2;
}

/**
 * g_key_file_remove_key:
 * @key_file: a #GKeyFile
 * @group_name: a group name
 * @key: a key name to remove
 * @error: return location for a #GError or %NULL
 *
 * Removes @key in @group_name from the key file. 
 *
 * Since: 2.6
 **/
void
g_key_file_remove_key (GKeyFile     *key_file,
		       const gchar  *group_name,
		       const gchar  *key,
		       GError      **error)
{
  GKeyFileGroup *group;
  GKeyFileKeyValuePair *pair;

  pair = NULL;

  if (group_name == NULL)
    group = key_file->current_group;
  else
    group = g_key_file_lookup_group (key_file, group_name);

  if (!group)
    {
      g_set_error (error, G_KEY_FILE_ERROR,
                   G_KEY_FILE_ERROR_GROUP_NOT_FOUND,
                   _("Key file does not have group '%s'"),
                   group_name);
      return;
    }

  group->key_value_pairs = g_list_remove (group->key_value_pairs, key_file);
  pair = g_key_file_lookup_key_value_pair (key_file, group, key);

  if (!pair)
    {
      g_set_error (error, G_KEY_FILE_ERROR,
                   G_KEY_FILE_ERROR_KEY_NOT_FOUND,
                   _("Key file does not have key '%s'"), key);
      return;
    }

  g_hash_table_remove (group->lookup_map, pair->key);

  key_file->approximate_size -= strlen (pair->key) + strlen (pair->value) + 2;
  g_key_file_key_value_pair_free (pair);
}

static GKeyFileGroup *
g_key_file_lookup_group (GKeyFile    *key_file,
			 const gchar *group_name)
{
  GKeyFileGroup *group;
  GList *tmp;

  group = NULL;
  for (tmp = key_file->groups; tmp != NULL; tmp = tmp->next)
    {
      group = (GKeyFileGroup *) tmp->data;

      if (group && group->name && strcmp (group->name, group_name) == 0)
        break;

      group = NULL;
    }

  return group;
}

static GKeyFileKeyValuePair *
g_key_file_lookup_key_value_pair (GKeyFile      *key_file,
				  GKeyFileGroup *group,
				  const gchar   *key)
{
  return (GKeyFileKeyValuePair *) g_hash_table_lookup (group->lookup_map, key);
}

/* Lines starting with # or consisting entirely of whitespace are merely
 * recorded, not parsed. This function assumes all leading whitespace
 * has been stripped.
 */
static gboolean
g_key_file_line_is_comment (const gchar *line)
{
  return (*line == '#' || *line == '\0' || *line == '\n');
}

/* A group in a key file is made up of a starting '[' followed by one
 * or more letters making up the group name followed by ']'.
 */
static gboolean
g_key_file_line_is_group (const gchar *line)
{
  gchar *p;

  p = (gchar *) line;
  if (*p != '[')
    return FALSE;

  p = g_utf8_next_char (p);

  if (!*p)
    return FALSE;

  p = g_utf8_next_char (p);

  /* Group name must be non-empty
   */
  if (*p == ']')
    return FALSE;

  while (*p && *p != ']')
    p = g_utf8_next_char (p);

  if (!*p)
    return FALSE;

  return TRUE;
}

static gboolean
g_key_file_line_is_key_value_pair (const gchar *line)
{
  gchar *p;

  p = (gchar *) g_utf8_strchr (line, -1, '=');

  if (!p)
    return FALSE;

  /* Key must be non-empty
   */
  if (*p == line[0])
    return FALSE;

  return TRUE;
}

static gchar *
g_key_file_parse_value_as_string (GKeyFile     *key_file,
				  const gchar  *value,
				  GSList      **pieces,
				  GError      **error)
{
  GError *parse_error = NULL;
  gchar *string_value, *p, *q0, *q;

  string_value = g_new (gchar, strlen (value) + 1);

  p = (gchar *) value;
  q0 = q = string_value;
  while (*p)
    {
      if (*p == '\\')
        {
          p++;

          switch (*p)
            {
            case 's':
              *q = ' ';
              break;

            case 'n':
              *q = '\n';
              break;

            case 't':
              *q = '\t';
              break;

            case 'r':
              *q = '\r';
              break;

            case '\\':
              *q = '\\';
              break;

            default:
	      if (pieces && *p == key_file->list_separator)
		*q = key_file->list_separator;
	      else
		{
		  *q++ = '\\';
		  *q = *p;
		  
		  if (parse_error == NULL)
		    {
		      gchar sequence[3];
		      
		      sequence[0] = '\\';
		      sequence[1] = *p;
		      sequence[2] = '\0';
		      
		      g_set_error (error, G_KEY_FILE_ERROR,
				   G_KEY_FILE_ERROR_INVALID_VALUE,
				   _("Key file contains invalid escape "
				     "sequence '%s'"), sequence);
		    }
		}
              break;
            }
        }
      else
	{
	  *q = *p;
	  if (pieces && (*p == key_file->list_separator))
	    {
	      *pieces = g_slist_prepend (*pieces, g_strndup (q0, q - q0));
	      q0 = q + 1; 
	    }
	}

      q++;
      p++;
    }

  if (p[-1] == '\\' && error == NULL)
    g_set_error (error, G_KEY_FILE_ERROR,
		 G_KEY_FILE_ERROR_INVALID_VALUE,
		 _("Key file contains escape character at end of line"));

  *q = '\0';
  if (pieces)
  {
    if (q0 < q)
      *pieces = g_slist_prepend (*pieces, g_strndup (q0, q - q0));
    *pieces = g_slist_reverse (*pieces);
  }

  return string_value;
}

static gchar *
g_key_file_parse_string_as_value (GKeyFile    *key_file,
				  const gchar *string,
				  gboolean     escape_separator)
{
  gchar *value, *p, *q;
  gsize length;

  length = strlen (string) + 1;

  /* Worst case would be that every character needs to be escaped.
   * In other words every character turns to two characters
   */
  value = g_new (gchar, 2 * length);

  p = (gchar *) string;
  q = value;
  while (p < (string + length - 1))
    {
      gchar escaped_character[3] = { '\\', 0, 0 };

      switch (*p)
        {
        case ' ':
          escaped_character[1] = 's';
          strcpy (q, escaped_character);
          q += 2;
          break;
        case '\n':
          escaped_character[1] = 'n';
          strcpy (q, escaped_character);
          q += 2;
          break;
        case '\t':
          escaped_character[1] = 't';
          strcpy (q, escaped_character);
          q += 2;
          break;
        case '\r':
          escaped_character[1] = 'r';
          strcpy (q, escaped_character);
          q += 2;
          break;
        case '\\':
          escaped_character[1] = '\\';
          strcpy (q, escaped_character);
          q += 2;
          break;
        default:
	  if (escape_separator && *p == key_file->list_separator)
	    {
	      escaped_character[1] = key_file->list_separator;
	      strcpy (q, escaped_character);
	      q += 2;
	    }
	  else 
	    {
	      *q = *p;
	      q++;
	    }
          break;
        }
      p++;
    }
  *q = '\0';

  return value;
}

static gint
g_key_file_parse_value_as_integer (GKeyFile     *key_file,
				   const gchar  *value,
				   GError      **error)
{
  gchar *end_of_valid_int;
  gint int_value = 0;

  int_value = strtol (value, &end_of_valid_int, 0);

  if (*end_of_valid_int != '\0')
    g_set_error (error, G_KEY_FILE_ERROR,
		 G_KEY_FILE_ERROR_INVALID_VALUE,
		 _("Value '%s' cannot be interpreted as a number."), value);

  return int_value;
}

static gchar *
g_key_file_parse_integer_as_value (GKeyFile *key_file,
				   gint      value)

{
  return g_strdup_printf ("%d", value);
}

static gboolean
g_key_file_parse_value_as_boolean (GKeyFile     *key_file,
				   const gchar  *value,
				   GError      **error)
{
  if (value)
    {
      if (strcmp (value, "true") == 0 || strcmp (value, "1") == 0)
        return TRUE;
      else if (strcmp (value, "false") == 0 || strcmp (value, "0") == 0)
        return FALSE;
    }

  g_set_error (error, G_KEY_FILE_ERROR,
               G_KEY_FILE_ERROR_INVALID_VALUE,
               _("Value '%s' cannot be interpreted as a boolean."), value);

  return FALSE;
}

static gchar *
g_key_file_parse_boolean_as_value (GKeyFile *key_file,
				   gboolean  value)
{
  if (value)
    return g_strdup ("true");
  else
    return g_strdup ("false");
}