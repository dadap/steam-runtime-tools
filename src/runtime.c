/*
 * Copyright © 2020 Collabora Ltd.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library. If not, see <http://www.gnu.org/licenses/>.
 */

#include "src/runtime.h"

#include <sysexits.h>

#include <gio/gio.h>

/* Include these before steam-runtime-tools.h so that their backport of
 * G_DEFINE_AUTOPTR_CLEANUP_FUNC will be visible to it */
#include "glib-backports.h"
#include "libglnx/libglnx.h"

#include <steam-runtime-tools/steam-runtime-tools.h>

#include "bwrap.h"
#include "bwrap-lock.h"
#include "elf-utils.h"
#include "enumtypes.h"
#include "flatpak-run-private.h"
#include "resolve-in-sysroot.h"
#include "utils.h"

/*
 * PvRuntime:
 *
 * Object representing a runtime to be used as the /usr for a game.
 */

struct _PvRuntime
{
  GObject parent;

  gchar *bubblewrap;
  gchar *source_files;
  gchar *tools_dir;
  PvBwrapLock *runtime_lock;

  gchar *mutable_parent;
  gchar *mutable_sysroot;
  gchar *tmpdir;
  gchar *overrides;
  const gchar *overrides_in_container;
  gchar *container_access;
  FlatpakBwrap *container_access_adverb;
  const gchar *runtime_files;   /* either source_files or mutable_sysroot */
  gchar *runtime_usr;           /* either runtime_files or that + "/usr" */
  const gchar *with_lock_in_container;

  PvRuntimeFlags flags;
  int mutable_parent_fd;
  int mutable_sysroot_fd;
  gboolean any_libc_from_host;
  gboolean all_libc_from_host;
  gboolean runtime_is_just_usr;
};

struct _PvRuntimeClass
{
  GObjectClass parent;
};

enum {
  PROP_0,
  PROP_BUBBLEWRAP,
  PROP_FLAGS,
  PROP_MUTABLE_PARENT,
  PROP_SOURCE_FILES,
  PROP_TOOLS_DIRECTORY,
  N_PROPERTIES
};

static GParamSpec *properties[N_PROPERTIES] = { NULL };

static void pv_runtime_initable_iface_init (GInitableIface *iface,
                                            gpointer unused);

G_DEFINE_TYPE_WITH_CODE (PvRuntime, pv_runtime, G_TYPE_OBJECT,
                         G_IMPLEMENT_INTERFACE (G_TYPE_INITABLE,
                                                pv_runtime_initable_iface_init))

/*
 * Return whether @path is likely to be visible in /run/host.
 * This needs to be kept approximately in sync with pv_bwrap_bind_usr()
 * and Flatpak's --filesystem=host-os special keyword.
 *
 * This doesn't currently handle /etc: we make the pessimistic assumption
 * that /etc/ld.so.cache, etc., are not shared.
 */
static gboolean
path_visible_in_run_host (const char *path)
{
  while (path[0] == '/')
    path++;

  if (g_str_has_prefix (path, "usr") &&
      (path[3] == '\0' || path[3] == '/'))
    return TRUE;

  if (g_str_has_prefix (path, "lib"))
    return TRUE;

  if (g_str_has_prefix (path, "bin") &&
      (path[3] == '\0' || path[3] == '/'))
    return TRUE;

  if (g_str_has_prefix (path, "sbin") ||
      (path[4] == '\0' || path[4] == '/'))
    return TRUE;

  return FALSE;
}

/*
 * Supported Debian-style multiarch tuples
 */
static const char * const multiarch_tuples[] =
{
  "x86_64-linux-gnu",
  "i386-linux-gnu",
  NULL
};

/*
 * Directories other than /usr/lib that we must search for loadable
 * modules, in the same order as multiarch_tuples
 */
static const char * const libquals[] =
{
  "lib64",
  "lib32"
};
G_STATIC_ASSERT (G_N_ELEMENTS (libquals)
                 == G_N_ELEMENTS (multiarch_tuples) - 1);

typedef struct
{
  gsize multiarch_index;
  const char *tuple;
  gchar *capsule_capture_libs_basename;
  gchar *capsule_capture_libs;
  gchar *libdir_on_host;
  gchar *libdir_in_container;
  const char *libqual;
  gchar *ld_so;
} RuntimeArchitecture;

static gboolean
runtime_architecture_init (RuntimeArchitecture *self,
                           PvRuntime *runtime)
{
  const gchar *argv[] = { NULL, "--print-ld.so", NULL };

  g_return_val_if_fail (self->multiarch_index < G_N_ELEMENTS (libquals), FALSE);
  g_return_val_if_fail (self->multiarch_index < G_N_ELEMENTS (multiarch_tuples) - 1,
                        FALSE);
  g_return_val_if_fail (self->tuple == NULL, FALSE);

  self->tuple = multiarch_tuples[self->multiarch_index];
  g_return_val_if_fail (self->tuple != NULL, FALSE);
  self->libqual = libquals[self->multiarch_index];

  self->capsule_capture_libs_basename = g_strdup_printf ("%s-capsule-capture-libs",
                                                         self->tuple);
  self->capsule_capture_libs = g_build_filename (runtime->tools_dir,
                                                 self->capsule_capture_libs_basename,
                                                 NULL);
  self->libdir_on_host = g_build_filename (runtime->overrides, "lib",
                                           self->tuple, NULL);
  self->libdir_in_container = g_build_filename (runtime->overrides_in_container,
                                                "lib", self->tuple, NULL);

  /* This has the side-effect of testing whether we can run binaries
   * for this architecture on the host system. */
  argv[0] = self->capsule_capture_libs;
  self->ld_so = pv_capture_output (argv, NULL);

  if (self->ld_so == NULL)
    {
      g_debug ("Cannot determine ld.so for %s", self->tuple);
      return FALSE;
    }

  return TRUE;
}

static gboolean
runtime_architecture_check_valid (RuntimeArchitecture *self)
{
  g_return_val_if_fail (self->multiarch_index < G_N_ELEMENTS (libquals), FALSE);
  g_return_val_if_fail (self->tuple == multiarch_tuples[self->multiarch_index], FALSE);
  g_return_val_if_fail (self->capsule_capture_libs_basename != NULL, FALSE);
  g_return_val_if_fail (self->capsule_capture_libs != NULL, FALSE);
  g_return_val_if_fail (self->libdir_on_host != NULL, FALSE);
  g_return_val_if_fail (self->libdir_in_container != NULL, FALSE);
  g_return_val_if_fail (self->libqual == libquals[self->multiarch_index], FALSE);
  g_return_val_if_fail (self->ld_so != NULL, FALSE);
  return TRUE;
}

static void
runtime_architecture_clear (RuntimeArchitecture *self)
{
  self->multiarch_index = G_MAXSIZE;
  self->tuple = NULL;
  self->libqual = NULL;
  g_clear_pointer (&self->capsule_capture_libs_basename, g_free);
  g_clear_pointer (&self->capsule_capture_libs, g_free);
  g_clear_pointer (&self->libdir_on_host, g_free);
  g_clear_pointer (&self->libdir_in_container, g_free);
  g_clear_pointer (&self->ld_so, g_free);
}

G_DEFINE_AUTO_CLEANUP_CLEAR_FUNC (RuntimeArchitecture,
                                  runtime_architecture_clear)

static gboolean pv_runtime_use_host_graphics_stack (PvRuntime *self,
                                                    FlatpakBwrap *bwrap,
                                                    GError **error);
static void pv_runtime_set_search_paths (PvRuntime *self,
                                         FlatpakBwrap *bwrap);

static void
pv_runtime_init (PvRuntime *self)
{
  self->any_libc_from_host = FALSE;
  self->all_libc_from_host = FALSE;
  self->mutable_parent_fd = -1;
  self->mutable_sysroot_fd = -1;
}

static void
pv_runtime_get_property (GObject *object,
                         guint prop_id,
                         GValue *value,
                         GParamSpec *pspec)
{
  PvRuntime *self = PV_RUNTIME (object);

  switch (prop_id)
    {
      case PROP_BUBBLEWRAP:
        g_value_set_string (value, self->bubblewrap);
        break;

      case PROP_FLAGS:
        g_value_set_flags (value, self->flags);
        break;

      case PROP_MUTABLE_PARENT:
        g_value_set_string (value, self->mutable_parent);
        break;

      case PROP_SOURCE_FILES:
        g_value_set_string (value, self->source_files);
        break;

      case PROP_TOOLS_DIRECTORY:
        g_value_set_string (value, self->tools_dir);
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pv_runtime_set_property (GObject *object,
                         guint prop_id,
                         const GValue *value,
                         GParamSpec *pspec)
{
  PvRuntime *self = PV_RUNTIME (object);
  const char *path;

  switch (prop_id)
    {
      case PROP_BUBBLEWRAP:
        /* Construct-only */
        g_return_if_fail (self->bubblewrap == NULL);
        self->bubblewrap = g_value_dup_string (value);
        break;

      case PROP_FLAGS:
        self->flags = g_value_get_flags (value);
        break;

      case PROP_MUTABLE_PARENT:
        /* Construct-only */
        g_return_if_fail (self->mutable_parent == NULL);
        path = g_value_get_string (value);

        if (path != NULL)
          {
            self->mutable_parent = realpath (path, NULL);

            if (self->mutable_parent == NULL)
              {
                /* It doesn't exist. Keep the non-canonical path so we
                 * can warn about it later */
                self->mutable_parent = g_strdup (path);
              }
          }

        break;

      case PROP_SOURCE_FILES:
        /* Construct-only */
        g_return_if_fail (self->source_files == NULL);
        path = g_value_get_string (value);

        if (path != NULL)
          {
            self->source_files = realpath (path, NULL);

            if (self->source_files == NULL)
              {
                /* It doesn't exist. Keep the non-canonical path so we
                 * can warn about it later */
                self->source_files = g_strdup (path);
              }
          }

        break;

      case PROP_TOOLS_DIRECTORY:
        /* Construct-only */
        g_return_if_fail (self->tools_dir == NULL);
        self->tools_dir = g_value_dup_string (value);
        break;

      default:
        G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
    }
}

static void
pv_runtime_constructed (GObject *object)
{
  PvRuntime *self = PV_RUNTIME (object);

  G_OBJECT_CLASS (pv_runtime_parent_class)->constructed (object);

  g_return_if_fail (self->bubblewrap != NULL);
  g_return_if_fail (self->source_files != NULL);
  g_return_if_fail (self->tools_dir != NULL);
}

static gboolean
pv_runtime_garbage_collect (PvRuntime *self,
                            PvBwrapLock *mutable_parent_lock,
                            GError **error)
{
  g_auto(GLnxDirFdIterator) iter = { FALSE };

  g_return_val_if_fail (PV_IS_RUNTIME (self), FALSE);
  g_return_val_if_fail (self->mutable_parent != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
  /* We don't actually *use* this: it just acts as an assertion that
   * we are holding the lock on the parent directory. */
  g_return_val_if_fail (mutable_parent_lock != NULL, FALSE);

  if (!glnx_dirfd_iterator_init_at (AT_FDCWD, self->mutable_parent,
                                    TRUE, &iter, error))
    return FALSE;

  while (TRUE)
    {
      g_autoptr(GError) local_error = NULL;
      g_autoptr(PvBwrapLock) temp_lock = NULL;
      g_autofree gchar *keep = NULL;
      g_autofree gchar *ref = NULL;
      struct dirent *dent;
      struct stat ignore;

      if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&iter, &dent,
                                                       NULL, error))
        return FALSE;

      if (dent == NULL)
        break;

      switch (dent->d_type)
        {
          case DT_DIR:
            break;

          case DT_BLK:
          case DT_CHR:
          case DT_FIFO:
          case DT_LNK:
          case DT_REG:
          case DT_SOCK:
          case DT_UNKNOWN:
          default:
            g_debug ("Ignoring %s/%s: not a directory",
                     self->mutable_parent, dent->d_name);
            continue;
        }

      if (!g_str_has_prefix (dent->d_name, "tmp-"))
        {
          g_debug ("Ignoring %s/%s: not tmp-*",
                   self->mutable_parent, dent->d_name);
          continue;
        }

      g_debug ("Found temporary runtime %s/%s, considering whether to "
               "delete it...",
               self->mutable_parent, dent->d_name);

      keep = g_build_filename (dent->d_name, "keep", NULL);

      if (glnx_fstatat (self->mutable_parent_fd, keep, &ignore,
                        AT_SYMLINK_NOFOLLOW, &local_error))
        {
          g_debug ("Not deleting \"%s/%s\": ./keep exists",
                   self->mutable_parent, dent->d_name);
          continue;
        }
      else if (!g_error_matches (local_error, G_IO_ERROR,
                                 G_IO_ERROR_NOT_FOUND))
        {
          /* EACCES or something? Give it the benefit of the doubt */
          g_warning ("Not deleting \"%s/%s\": unable to stat ./keep: %s",
                   self->mutable_parent, dent->d_name, local_error->message);
          g_clear_error (&local_error);
          continue;
        }

      g_clear_error (&local_error);

      ref = g_build_filename (dent->d_name, ".ref", NULL);
      temp_lock = pv_bwrap_lock_new (self->mutable_parent_fd, ref,
                                     (PV_BWRAP_LOCK_FLAGS_CREATE |
                                      PV_BWRAP_LOCK_FLAGS_WRITE),
                                     &local_error);

      if (temp_lock == NULL)
        {
          g_debug ("Ignoring \"%s/%s\": unable to get lock: %s",
                   self->mutable_parent, dent->d_name,
                   local_error->message);
          g_clear_error (&local_error);
          continue;
        }

      g_debug ("Deleting \"%s/%s\"...", self->mutable_parent, dent->d_name);

      /* We have the lock, which would not have happened if someone was
       * still using the runtime, so we can safely delete it. */
      if (!glnx_shutil_rm_rf_at (self->mutable_parent_fd, dent->d_name,
                                 NULL, &local_error))
        {
          g_debug ("Unable to delete %s/%s: %s",
                   self->mutable_parent, dent->d_name, local_error->message);
          g_clear_error (&local_error);
          continue;
        }
    }

  return TRUE;
}

static gboolean
pv_runtime_init_mutable (PvRuntime *self,
                         GError **error)
{
  g_autofree gchar *dest_usr = NULL;
  g_autofree gchar *source_usr_subdir = NULL;
  g_autofree gchar *temp_dir = NULL;
  g_autoptr(GDir) dir = NULL;
  g_autoptr(PvBwrapLock) copy_lock = NULL;
  g_autoptr(PvBwrapLock) mutable_lock = NULL;
  g_autoptr(PvBwrapLock) source_lock = NULL;
  const char *member;
  const char *source_usr;
  glnx_autofd int temp_dir_fd = -1;
  gboolean is_just_usr;

  /* Nothing to do in this case */
  if (self->mutable_parent == NULL)
    return TRUE;

  if (g_mkdir_with_parents (self->mutable_parent, 0700) != 0)
    return glnx_throw_errno_prefix (error, "Unable to create %s",
                                    self->mutable_parent);

  if (!glnx_opendirat (AT_FDCWD, self->mutable_parent, TRUE,
                       &self->mutable_parent_fd, error))
    return FALSE;

  /* Lock the parent directory. Anything that directly manipulates the
   * temporary runtimes is expected to do the same, so that
   * it cannot be deleting temporary runtimes at the same time we're
   * creating them.
   *
   * This is a read-mode lock: it's OK to create more than one temporary
   * runtime in parallel, as long as nothing is deleting them
   * concurrently. */
  mutable_lock = pv_bwrap_lock_new (self->mutable_parent_fd, ".ref",
                                    PV_BWRAP_LOCK_FLAGS_CREATE,
                                    error);

  if (mutable_lock == NULL)
    return glnx_prefix_error (error, "Unable to lock \"%s/%s\"",
                              self->mutable_parent, ".ref");

  /* GC old runtimes (if they have become unused) before we create a
   * new one. This means we should only ever have one temporary runtime
   * copy per game that is run concurrently. */
  if ((self->flags & PV_RUNTIME_FLAGS_GC_RUNTIMES) != 0 &&
      !pv_runtime_garbage_collect (self, mutable_lock, error))
    return FALSE;

  temp_dir = g_build_filename (self->mutable_parent, "tmp-XXXXXX", NULL);

  if (g_mkdtemp (temp_dir) == NULL)
    return glnx_throw_errno_prefix (error,
                                    "Cannot create temporary directory \"%s\"",
                                    temp_dir);

  source_usr_subdir = g_build_filename (self->source_files, "usr", NULL);
  dest_usr = g_build_filename (temp_dir, "usr", NULL);

  is_just_usr = !g_file_test (source_usr_subdir, G_FILE_TEST_IS_DIR);

  if (is_just_usr)
    {
      /* ${source_files}/usr does not exist, so assume it's a merged /usr,
       * for example ./scout/files. Copy ${source_files}/bin to
       * ${temp_dir}/usr/bin, etc. */
      source_usr = self->source_files;

      if (!pv_cheap_tree_copy (self->source_files, dest_usr, error))
        return FALSE;
    }
  else
    {
      /* ${source_files}/usr exists, so assume it's a complete sysroot.
       * Copy ${source_files}/bin to ${temp_dir}/bin, etc. */
      source_usr = source_usr_subdir;

      if (!pv_cheap_tree_copy (self->source_files, temp_dir, error))
        return FALSE;
    }

  if (!glnx_opendirat (-1, temp_dir, FALSE, &temp_dir_fd, error))
    return FALSE;

  /* We need to break the hard link for the lock file, otherwise the
   * temporary copy will share its locked/unlocked state with the
   * original. */
  if (TEMP_FAILURE_RETRY (unlinkat (temp_dir_fd, ".ref", 0)) != 0
      && errno != ENOENT)
    return glnx_throw_errno_prefix (error,
                                    "Cannot remove \"%s/.ref\"",
                                    temp_dir);

  if (TEMP_FAILURE_RETRY (unlinkat (temp_dir_fd, "usr/.ref", 0)) != 0
      && errno != ENOENT)
    return glnx_throw_errno_prefix (error,
                                    "Cannot remove \"%s/usr/.ref\"",
                                    temp_dir);

  /* Create the copy in a pre-locked state. After the lock on the parent
   * directory is released, the copy continues to have a read lock,
   * preventing it from being modified or deleted while in use (even if
   * a cleanup process successfully obtains a write lock on the parent).
   *
   * Because we control the structure of the runtime in this case, we
   * actually lock /usr/.ref instead of /.ref, and ensure that /.ref
   * is a symlink to it. This might become important if we pass the
   * runtime's /usr to Flatpak, which normally takes out a lock on
   * /usr/.ref (obviously this will only work if the runtime happens
   * to be merged-/usr). */
  copy_lock = pv_bwrap_lock_new (temp_dir_fd, "usr/.ref",
                                 PV_BWRAP_LOCK_FLAGS_CREATE,
                                 error);

  if (copy_lock == NULL)
    return glnx_prefix_error (error,
                              "Unable to lock \"%s/.ref\" in temporary runtime",
                              dest_usr);

  if (is_just_usr)
    {
      if (TEMP_FAILURE_RETRY (symlinkat ("usr/.ref",
                                         temp_dir_fd,
                                         ".ref")) != 0)
        return glnx_throw_errno_prefix (error,
                                        "Cannot create symlink \"%s/.ref\" -> usr/.ref",
                                        temp_dir);
    }

  dir = g_dir_open (source_usr, 0, error);

  if (dir == NULL)
    return FALSE;

  for (member = g_dir_read_name (dir);
       member != NULL;
       member = g_dir_read_name (dir))
    {
      /* Create symlinks ${temp_dir}/bin -> usr/bin, etc. if missing.
       *
       * Also make ${temp_dir}/etc, ${temp_dir}/var symlinks to etc
       * and var, for the benefit of tools like capsule-capture-libs
       * accessing /etc/ld.so.cache in the incomplete container (for the
       * final container command-line they get merged by bind_runtime()
       * instead). */
      if (g_str_equal (member, "bin") ||
          g_str_equal (member, "etc") ||
          (g_str_has_prefix (member, "lib") &&
           !g_str_equal (member, "libexec")) ||
          g_str_equal (member, "sbin") ||
          g_str_equal (member, "var"))
        {
          g_autofree gchar *dest = g_build_filename (temp_dir, member, NULL);
          g_autofree gchar *target = g_build_filename ("usr", member, NULL);

          if (symlink (target, dest) != 0)
            {
              /* Ignore EEXIST in the case where it was not just /usr:
               * it's fine if the runtime we copied from source_files
               * already had either directories or symlinks in its root
               * directory */
              if (is_just_usr || errno != EEXIST)
                return glnx_throw_errno_prefix (error,
                                                "Cannot create symlink \"%s\" -> %s",
                                                dest, target);
            }
        }
    }

  /* Hand over from holding a lock on the source to just holding a lock
   * on the copy. We'll release source_lock when we leave this scope */
  source_lock = g_steal_pointer (&self->runtime_lock);
  self->runtime_lock = g_steal_pointer (&copy_lock);
  self->mutable_sysroot = g_steal_pointer (&temp_dir);
  self->mutable_sysroot_fd = glnx_steal_fd (&temp_dir_fd);

  return TRUE;
}

static gboolean
pv_runtime_initable_init (GInitable *initable,
                          GCancellable *cancellable G_GNUC_UNUSED,
                          GError **error)
{
  PvRuntime *self = PV_RUNTIME (initable);
  g_autofree gchar *files_ref = NULL;

  g_return_val_if_fail (PV_IS_RUNTIME (self), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!g_file_test (self->bubblewrap, G_FILE_TEST_IS_EXECUTABLE))
    {
      return glnx_throw (error, "\"%s\" is not executable",
                         self->bubblewrap);
    }

  if (self->mutable_parent != NULL
      && !g_file_test (self->mutable_parent, G_FILE_TEST_IS_DIR))
    {
      return glnx_throw (error, "\"%s\" is not a directory",
                         self->mutable_parent);
    }

  if (!g_file_test (self->source_files, G_FILE_TEST_IS_DIR))
    {
      return glnx_throw (error, "\"%s\" is not a directory",
                         self->source_files);
    }

  if (!g_file_test (self->tools_dir, G_FILE_TEST_IS_DIR))
    {
      return glnx_throw (error, "\"%s\" is not a directory",
                         self->tools_dir);
    }

  /* Take a lock on the runtime until we're finished with setup,
   * to make sure it doesn't get deleted.
   *
   * If the runtime is mounted read-only in the container, it will
   * continue to be locked until all processes in the container exit.
   * If we make a temporary mutable copy, we only hold this lock until
   * setup has finished. */
  files_ref = g_build_filename (self->source_files, ".ref", NULL);
  self->runtime_lock = pv_bwrap_lock_new (AT_FDCWD, files_ref,
                                          PV_BWRAP_LOCK_FLAGS_CREATE,
                                          error);

  /* If the runtime is being deleted, ... don't use it, I suppose? */
  if (self->runtime_lock == NULL)
    return FALSE;

  if (!pv_runtime_init_mutable (self, error))
    return FALSE;

  if (self->mutable_sysroot != NULL)
    {
      self->overrides_in_container = "/usr/lib/pressure-vessel/overrides";
      self->overrides = g_build_filename (self->mutable_sysroot,
                                          self->overrides_in_container, NULL);
      self->runtime_files = self->mutable_sysroot;
    }
  else
    {
      /* We currently only need a temporary directory if we don't have
       * a mutable sysroot to work with. */
      self->tmpdir = g_dir_make_tmp ("pressure-vessel-wrap.XXXXXX", error);

      if (self->tmpdir == NULL)
        return FALSE;

      self->overrides = g_build_filename (self->tmpdir, "overrides", NULL);
      self->overrides_in_container = "/overrides";
      self->runtime_files = self->source_files;
    }

  g_mkdir (self->overrides, 0700);

  self->runtime_usr = g_build_filename (self->runtime_files, "usr", NULL);

  if (g_file_test (self->runtime_usr, G_FILE_TEST_IS_DIR))
    {
      self->runtime_is_just_usr = FALSE;
    }
  else
    {
      /* runtime_files is just a merged /usr. */
      self->runtime_is_just_usr = TRUE;
      g_free (self->runtime_usr);
      self->runtime_usr = g_strdup (self->runtime_files);
    }

  return TRUE;
}

void
pv_runtime_cleanup (PvRuntime *self)
{
  g_autoptr(GError) local_error = NULL;

  g_return_if_fail (PV_IS_RUNTIME (self));

  if (self->tmpdir != NULL &&
      !glnx_shutil_rm_rf_at (-1, self->tmpdir, NULL, &local_error))
    {
      g_warning ("Unable to delete temporary directory: %s",
                 local_error->message);
    }

  g_clear_pointer (&self->overrides, g_free);
  g_clear_pointer (&self->container_access, g_free);
  g_clear_pointer (&self->container_access_adverb, flatpak_bwrap_free);
  g_clear_pointer (&self->tmpdir, g_free);
}

static void
pv_runtime_finalize (GObject *object)
{
  PvRuntime *self = PV_RUNTIME (object);

  pv_runtime_cleanup (self);
  g_free (self->bubblewrap);
  glnx_close_fd (&self->mutable_parent_fd);
  g_free (self->mutable_parent);
  glnx_close_fd (&self->mutable_sysroot_fd);
  g_free (self->mutable_sysroot);
  g_free (self->runtime_usr);
  g_free (self->source_files);
  g_free (self->tools_dir);

  if (self->runtime_lock != NULL)
    pv_bwrap_lock_free (self->runtime_lock);

  G_OBJECT_CLASS (pv_runtime_parent_class)->finalize (object);
}

static void
pv_runtime_class_init (PvRuntimeClass *cls)
{
  GObjectClass *object_class = G_OBJECT_CLASS (cls);

  object_class->get_property = pv_runtime_get_property;
  object_class->set_property = pv_runtime_set_property;
  object_class->constructed = pv_runtime_constructed;
  object_class->finalize = pv_runtime_finalize;

  properties[PROP_BUBBLEWRAP] =
    g_param_spec_string ("bubblewrap", "Bubblewrap",
                         "Bubblewrap executable",
                         NULL,
                         (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_FLAGS] =
    g_param_spec_flags ("flags", "Flags",
                        "Flags affecting how we set up the runtime",
                        PV_TYPE_RUNTIME_FLAGS, PV_RUNTIME_FLAGS_NONE,
                        (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                         G_PARAM_STATIC_STRINGS));

  properties[PROP_MUTABLE_PARENT] =
    g_param_spec_string ("mutable-parent", "Mutable parent",
                         ("Path to a directory in which to create a "
                          "mutable copy of source-files, or NULL"),
                         NULL,
                         (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_SOURCE_FILES] =
    g_param_spec_string ("source-files", "Source files",
                         ("Path to read-only runtime files (merged-/usr "
                          "or sysroot) on host system"),
                         NULL,
                         (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS));

  properties[PROP_TOOLS_DIRECTORY] =
    g_param_spec_string ("tools-directory", "Tools directory",
                         "Path to pressure-vessel/bin on host system",
                         NULL,
                         (G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
                          G_PARAM_STATIC_STRINGS));

  g_object_class_install_properties (object_class, N_PROPERTIES, properties);
}

PvRuntime *
pv_runtime_new (const char *source_files,
                const char *mutable_parent,
                const char *bubblewrap,
                const char *tools_dir,
                PvRuntimeFlags flags,
                GError **error)
{
  g_return_val_if_fail (source_files != NULL, NULL);
  g_return_val_if_fail (bubblewrap != NULL, NULL);
  g_return_val_if_fail (tools_dir != NULL, NULL);
  g_return_val_if_fail ((flags & ~(PV_RUNTIME_FLAGS_MASK)) == 0, NULL);

  return g_initable_new (PV_TYPE_RUNTIME,
                         NULL,
                         error,
                         "bubblewrap", bubblewrap,
                         "mutable-parent", mutable_parent,
                         "source-files", source_files,
                         "tools-directory", tools_dir,
                         "flags", flags,
                         NULL);
}

/* If we are using a runtime, pass the lock fd to the executed process,
 * and make it act as a subreaper for the game itself.
 *
 * If we were using --unshare-pid then we could use bwrap --sync-fd
 * and rely on bubblewrap's init process for this, but we currently
 * can't do that without breaking gameoverlayrender.so's assumptions. */
void
pv_runtime_append_lock_adverb (PvRuntime *self,
                               FlatpakBwrap *bwrap)
{
  g_return_if_fail (PV_IS_RUNTIME (self));
  g_return_if_fail (!pv_bwrap_was_finished (bwrap));
  /* This will be true if pv_runtime_bind() was successfully called. */
  g_return_if_fail (self->with_lock_in_container != NULL);

  flatpak_bwrap_add_args (bwrap,
                          self->with_lock_in_container,
                          "--subreaper",
                          NULL);

  if (pv_bwrap_lock_is_ofd (self->runtime_lock))
    {
      int fd = pv_bwrap_lock_steal_fd (self->runtime_lock);
      g_autofree gchar *fd_str = NULL;

      g_debug ("Passing lock fd %d down to with-lock", fd);
      flatpak_bwrap_add_fd (bwrap, fd);
      fd_str = g_strdup_printf ("%d", fd);
      flatpak_bwrap_add_args (bwrap,
                              "--fd", fd_str,
                              NULL);
    }
  else
    {
      /*
       * We were unable to take out an open file descriptor lock,
       * so it will be released on fork(). Tell the with-lock process
       * to take out its own compatible lock instead. There will be
       * a short window during which we have lost our lock but the
       * with-lock process has not taken its lock - that's unavoidable
       * if we want to use exec() to replace ourselves with the
       * container.
       *
       * pv_bwrap_bind_usr() arranges for /.ref to either be a
       * symbolic link to /usr/.ref which is the runtime_lock
       * (if opt_runtime is a merged /usr), or the runtime_lock
       * itself (otherwise).
       */
      g_debug ("Telling process in container to lock /.ref");
      flatpak_bwrap_add_args (bwrap,
                              "--lock-file", "/.ref",
                              NULL);
    }

  flatpak_bwrap_add_args (bwrap,
                          "--",
                          NULL);
}

/*
 * Set self->container_access_adverb to a (possibly empty) command prefix
 * that will result in the container being available at
 * self->container_access, with write access to self->overrides, and
 * read-only access to everything else.
 */
static gboolean
pv_runtime_provide_container_access (PvRuntime *self,
                                     GError **error)
{
  if (self->container_access_adverb != NULL)
    return TRUE;

  if (!self->runtime_is_just_usr)
    {
      static const char * const need_top_level[] =
      {
        "bin",
        "etc",
        "lib",
        "sbin",
      };
      gsize i;

      /* If we are working with a runtime that has a root directory containing
       * /etc and /usr, we can just access it via its path - that's "the same
       * shape" that the final system is going to be.
       *
       * In particular, if we are working with a writeable copy of a runtime
       * that we are editing in-place, it's always like that. */
      g_debug ("%s: Setting up runtime without using bwrap",
               G_STRFUNC);
      self->container_access_adverb = flatpak_bwrap_new (NULL);
      self->container_access = g_strdup (self->runtime_files);

      /* This is going to go poorly for us if the runtime is not complete.
       * !self->runtime_is_just_usr means we know it has a /usr subdirectory,
       * but that doesn't guarantee that it has /bin, /lib, /sbin (either
       * in the form of real directories or symlinks into /usr) and /etc
       * (for at least /etc/alternatives and /etc/ld.so.cache).
       *
       * This check is not intended to be exhaustive, merely something
       * that will catch obvious mistakes like completely forgetting to
       * add the merged-/usr symlinks.
       *
       * In practice we also need /lib64 for 64-bit-capable runtimes,
       * but a pure 32-bit runtime would legitimately not have that,
       * so we don't check for it. */
      for (i = 0; i < G_N_ELEMENTS (need_top_level); i++)
        {
          g_autofree gchar *path = g_build_filename (self->runtime_files,
                                                     need_top_level[i],
                                                     NULL);

          if (!g_file_test (path, G_FILE_TEST_IS_DIR))
            g_warning ("%s does not exist, this probably won't work",
                       path);
        }
    }
  else
    {
      /* Otherwise, will we need to use bwrap to build a directory hierarchy
       * that is the same shape as the final system. */
      g_debug ("%s: Using bwrap to set up runtime that is just /usr",
               G_STRFUNC);

      /* By design, writeable copies of the runtime never need this:
       * the writeable copy is a complete sysroot, not just a merged /usr. */
      g_assert (self->mutable_sysroot == NULL);
      g_assert (self->tmpdir != NULL);

      self->container_access = g_build_filename (self->tmpdir, "mnt", NULL);
      g_mkdir (self->container_access, 0700);

      self->container_access_adverb = flatpak_bwrap_new (NULL);
      flatpak_bwrap_add_args (self->container_access_adverb,
                              self->bubblewrap,
                              "--ro-bind", "/", "/",
                              "--bind", self->overrides, self->overrides,
                              "--tmpfs", self->container_access,
                              NULL);
      if (!pv_bwrap_bind_usr (self->container_access_adverb,
                              self->runtime_files,
                              self->container_access,
                              error))
        return FALSE;
    }

  return TRUE;
}

static gboolean
try_bind_dri (PvRuntime *self,
              RuntimeArchitecture *arch,
              FlatpakBwrap *bwrap,
              const char *libdir,
              GError **error)
{
  g_autofree gchar *dri = g_build_filename (libdir, "dri", NULL);
  g_autofree gchar *s2tc = g_build_filename (libdir, "libtxc_dxtn.so", NULL);

  if (g_file_test (dri, G_FILE_TEST_IS_DIR))
    {
      g_autoptr(FlatpakBwrap) temp_bwrap = NULL;
      g_autofree gchar *expr = NULL;
      g_autoptr(GDir) dir = NULL;
      const char *member;

      g_debug ("Collecting dependencies of DRI drivers in \"%s\"...", dri);
      expr = g_strdup_printf ("only-dependencies:if-exists:path-match:%s/dri/*.so",
                              libdir);

      if (!pv_runtime_provide_container_access (self, error))
        return FALSE;

      temp_bwrap = pv_bwrap_copy (self->container_access_adverb);
      flatpak_bwrap_add_args (temp_bwrap,
                              arch->capsule_capture_libs,
                              "--container", self->container_access,
                              "--link-target", "/run/host",
                              "--dest", arch->libdir_on_host,
                              "--provider", "/",
                              expr,
                              NULL);
      flatpak_bwrap_finish (temp_bwrap);

      if (!pv_bwrap_run_sync (temp_bwrap, NULL, error))
        return FALSE;

      g_clear_pointer (&temp_bwrap, flatpak_bwrap_free);

      dir = g_dir_open (dri, 0, error);

      if (dir == NULL)
        return FALSE;

      for (member = g_dir_read_name (dir);
           member != NULL;
           member = g_dir_read_name (dir))
        {
          g_autofree gchar *target = g_build_filename ("/run/host", dri,
                                                      member, NULL);
          g_autofree gchar *dest = g_build_filename (arch->libdir_on_host,
                                                     "dri", member, NULL);

          g_debug ("Creating symbolic link \"%s\" -> \"%s\" for \"%s\" DRI driver",
                   dest, target, arch->tuple);

          /* Delete an existing symlink if any, like ln -f */
          if (unlink (dest) != 0 && errno != ENOENT)
            return glnx_throw_errno_prefix (error,
                                            "Unable to remove \"%s\"",
                                            dest);

          if (symlink (target, dest) != 0)
            return glnx_throw_errno_prefix (error,
                                            "Unable to create symlink \"%s\" -> \"%s\"",
                                            dest, target);
        }
    }

  if (g_file_test (s2tc, G_FILE_TEST_EXISTS))
    {
      g_autoptr(FlatpakBwrap) temp_bwrap = NULL;
      g_autofree gchar *expr = NULL;

      g_debug ("Collecting s2tc \"%s\" and its dependencies...", s2tc);
      expr = g_strdup_printf ("path-match:%s", s2tc);

      if (!pv_runtime_provide_container_access (self, error))
        return FALSE;

      temp_bwrap = pv_bwrap_copy (self->container_access_adverb);
      flatpak_bwrap_add_args (temp_bwrap,
                              arch->capsule_capture_libs,
                              "--container", self->container_access,
                              "--link-target", "/run/host",
                              "--dest", arch->libdir_on_host,
                              "--provider", "/",
                              expr,
                              NULL);
      flatpak_bwrap_finish (temp_bwrap);

      if (!pv_bwrap_run_sync (temp_bwrap, NULL, error))
        return FALSE;
    }

  return TRUE;
}

/*
 * Try to make sure we have all the locales we need, by running
 * the helper from steam-runtime-tools in the container. If this
 * fails, it isn't fatal - carry on anyway.
 *
 * @bwrap must be set up to have the same libc that we will be using
 * for the container.
 */
static void
ensure_locales (PvRuntime *self,
                gboolean on_host,
                FlatpakBwrap *bwrap)
{
  g_autoptr(GError) local_error = NULL;
  g_autoptr(FlatpakBwrap) run_locale_gen = NULL;
  g_autofree gchar *locale_gen = NULL;
  g_autofree gchar *locales = g_build_filename (self->overrides, "locales", NULL);
  g_autofree gchar *locales_in_container = g_build_filename (self->overrides_in_container,
                                                             "locales", NULL);
  g_autoptr(GDir) dir = NULL;
  int exit_status;

  /* bwrap can't own any fds yet, because if it did,
   * flatpak_bwrap_append_bwrap() would steal them. */
  g_return_if_fail (bwrap->fds == NULL || bwrap->fds->len == 0);

  g_mkdir (locales, 0700);

  run_locale_gen = flatpak_bwrap_new (NULL);

  if (on_host)
    {
      locale_gen = g_build_filename (self->tools_dir,
                                     "pressure-vessel-locale-gen",
                                     NULL);
      /* We don't actually need to use bwrap when we're just running on
       * the host system. */
      flatpak_bwrap_add_args (run_locale_gen,
                              locale_gen,
                              "--output-dir", locales,
                              "--verbose",
                              NULL);
    }
  else
    {
      locale_gen = g_build_filename ("/run/host/tools",
                                     "pressure-vessel-locale-gen",
                                     NULL);

      flatpak_bwrap_append_bwrap (run_locale_gen, bwrap);
      flatpak_bwrap_add_args (run_locale_gen,
                              "--ro-bind", self->overrides,
                              self->overrides_in_container,
                              NULL);

      if (!flatpak_bwrap_bundle_args (run_locale_gen, 1, -1, FALSE,
                                      &local_error))
        {
          g_warning ("Unable to set up locale-gen command: %s",
                     local_error->message);
          g_clear_error (&local_error);
        }

      flatpak_bwrap_add_args (run_locale_gen,
                              "--ro-bind", self->tools_dir, "/run/host/tools",
                              "--bind", locales, locales_in_container,
                              locale_gen,
                              "--output-dir", locales_in_container,
                              "--verbose",
                              NULL);
    }

  flatpak_bwrap_finish (run_locale_gen);

  /* locale-gen exits 72 (EX_OSFILE) if it had to correct for
   * missing locales at OS level. This is not an error. */
  if (!pv_bwrap_run_sync (run_locale_gen, &exit_status, &local_error))
    {
      if (exit_status == EX_OSFILE)
        g_debug ("pressure-vessel-locale-gen created missing locales");
      else
        g_warning ("Unable to generate locales: %s", local_error->message);

      g_clear_error (&local_error);
    }
  else
    {
      g_debug ("No locales generated");
    }

  dir = g_dir_open (locales, 0, NULL);

  /* If the directory is not empty, make it the container's LOCPATH */
  if (dir != NULL && g_dir_read_name (dir) != NULL)
    {
      g_autoptr(GString) locpath = NULL;

      g_debug ("%s is non-empty", locales);

      locpath = g_string_new (locales_in_container);
      pv_search_path_append (locpath, g_getenv ("LOCPATH"));
      flatpak_bwrap_add_args (bwrap,
                              "--setenv", "LOCPATH", locpath->str,
                              NULL);
    }
  else
    {
      g_debug ("%s is empty", locales);
    }
}

typedef enum
{
  ICD_KIND_NONEXISTENT,
  ICD_KIND_ABSOLUTE,
  ICD_KIND_SONAME
} IcdKind;

typedef struct
{
  /* (type SrtEglIcd) or (type SrtVulkanIcd) or (type SrtVdpauDriver)
   * or (type SrtVaApiDriver) */
  gpointer icd;
  gchar *resolved_library;
  /* Last entry is always NONEXISTENT.
   * For VA-API, we use [0] and ignore the other elements.
   * For the rest, this is keyed by the index of a multiarch tuple
   * in multiarch_tuples. */
  IcdKind kinds[G_N_ELEMENTS (multiarch_tuples)];
  /* Last entry is always NULL */
  gchar *paths_in_container[G_N_ELEMENTS (multiarch_tuples)];
} IcdDetails;

static IcdDetails *
icd_details_new (gpointer icd)
{
  IcdDetails *self;
  gsize i;

  g_return_val_if_fail (G_IS_OBJECT (icd), NULL);
  g_return_val_if_fail (SRT_IS_EGL_ICD (icd) ||
                        SRT_IS_VULKAN_ICD (icd) ||
                        SRT_IS_VDPAU_DRIVER (icd) ||
                        SRT_IS_VA_API_DRIVER (icd),
                        NULL);

  self = g_slice_new0 (IcdDetails);
  self->icd = g_object_ref (icd);
  self->resolved_library = NULL;

  for (i = 0; i < G_N_ELEMENTS (multiarch_tuples); i++)
    {
      self->kinds[i] = ICD_KIND_NONEXISTENT;
      self->paths_in_container[i] = NULL;
    }

  return self;
}

static void
icd_details_free (IcdDetails *self)
{
  gsize i;

  g_object_unref (self->icd);
  g_free (self->resolved_library);

  for (i = 0; i < G_N_ELEMENTS (multiarch_tuples); i++)
    g_free (self->paths_in_container[i]);

  g_slice_free (IcdDetails, self);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC (IcdDetails, icd_details_free)

/*
 * @sequence_number: numbered directory to use. Set to G_MAXSIZE to
 *  use just @subdir without a numbered sub directory
 */
static gboolean
bind_icd (PvRuntime *self,
          RuntimeArchitecture *arch,
          gsize sequence_number,
          const char *subdir,
          IcdDetails *details,
          GError **error)
{
  static const char options[] = "if-exists:if-same-abi";
  g_autofree gchar *on_host = NULL;
  g_autofree gchar *pattern = NULL;
  g_autofree gchar *dependency_pattern = NULL;
  g_autofree gchar *seq_str = NULL;
  const char *mode;
  g_autoptr(FlatpakBwrap) temp_bwrap = NULL;
  gsize multiarch_index;

  g_return_val_if_fail (runtime_architecture_check_valid (arch), FALSE);
  g_return_val_if_fail (subdir != NULL, FALSE);
  g_return_val_if_fail (details != NULL, FALSE);
  g_return_val_if_fail (details->resolved_library != NULL, FALSE);
  multiarch_index = arch->multiarch_index;
  g_return_val_if_fail (details->kinds[multiarch_index] == ICD_KIND_NONEXISTENT,
                        FALSE);
  g_return_val_if_fail (details->paths_in_container[multiarch_index] == NULL,
                        FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (g_path_is_absolute (details->resolved_library))
    {
      details->kinds[multiarch_index] = ICD_KIND_ABSOLUTE;
      mode = "path";

      /* Because the ICDs might have collisions among their
       * basenames (might differ only by directory), we put each
       * in its own numbered directory. */
      if (sequence_number != G_MAXSIZE)
        {
          seq_str = g_strdup_printf ("%" G_GSIZE_FORMAT, sequence_number);
          on_host = g_build_filename (arch->libdir_on_host, subdir, seq_str, NULL);
        }
      else
        {
          on_host = g_build_filename (arch->libdir_on_host, subdir, NULL);
        }

      g_debug ("Ensuring %s exists", on_host);

      if (g_mkdir_with_parents (on_host, 0700) != 0)
        return glnx_throw_errno_prefix (error, "Unable to create %s", on_host);
    }
  else
    {
      /* ICDs in the default search path by definition can't collide:
       * one of them is the first one we find, and we use that one. */
      details->kinds[multiarch_index] = ICD_KIND_SONAME;
      mode = "soname";
    }

  pattern = g_strdup_printf ("no-dependencies:even-if-older:%s:%s:%s",
                             options, mode, details->resolved_library);
  dependency_pattern = g_strdup_printf ("only-dependencies:%s:%s:%s",
                                        options, mode, details->resolved_library);

  if (!pv_runtime_provide_container_access (self, error))
    return FALSE;

  temp_bwrap = pv_bwrap_copy (self->container_access_adverb);
  flatpak_bwrap_add_args (temp_bwrap,
                          arch->capsule_capture_libs,
                          "--container", self->container_access,
                          "--link-target", "/run/host",
                          "--dest",
                            on_host == NULL ? arch->libdir_on_host : on_host,
                          "--provider", "/",
                          pattern,
                          NULL);
  flatpak_bwrap_finish (temp_bwrap);

  if (!pv_bwrap_run_sync (temp_bwrap, NULL, error))
    return FALSE;

  g_clear_pointer (&temp_bwrap, flatpak_bwrap_free);

  if (on_host != NULL)
    {
      /* Try to remove the directory we created. If it succeeds, then we
       * can optimize slightly by not capturing the dependencies: there's
       * no point, because we know we didn't create a symlink to the ICD
       * itself. (It must have been nonexistent or for a different ABI.) */
      if (g_rmdir (on_host) == 0)
        {
          details->kinds[multiarch_index] = ICD_KIND_NONEXISTENT;
          return TRUE;
        }
    }

  temp_bwrap = pv_bwrap_copy (self->container_access_adverb);
  flatpak_bwrap_add_args (temp_bwrap,
                          arch->capsule_capture_libs,
                          "--container", self->container_access,
                          "--link-target", "/run/host",
                          "--dest", arch->libdir_on_host,
                          "--provider", "/",
                          dependency_pattern,
                          NULL);
  flatpak_bwrap_finish (temp_bwrap);

  if (!pv_bwrap_run_sync (temp_bwrap, NULL, error))
    return FALSE;

  g_clear_pointer (&temp_bwrap, flatpak_bwrap_free);

  if (details->kinds[multiarch_index] == ICD_KIND_ABSOLUTE)
    {
      g_assert (on_host != NULL);
      details->paths_in_container[multiarch_index] = g_build_filename (arch->libdir_in_container,
                                                                       subdir,
                                                                       seq_str ? seq_str : "",
                                                                       glnx_basename (details->resolved_library),
                                                                       NULL);
    }

  return TRUE;
}

static gboolean
bind_runtime (PvRuntime *self,
              FlatpakBwrap *bwrap,
              GError **error)
{
  static const char * const bind_mutable[] =
  {
    "etc",
    "var/cache",
    "var/lib"
  };
  static const char * const dont_bind[] =
  {
    "/etc/group",
    "/etc/passwd",
    "/etc/host.conf",
    "/etc/hosts",
    "/etc/localtime",
    "/etc/machine-id",
    "/etc/resolv.conf",
    "/var/lib/dbus",
    "/var/lib/dhcp",
    "/var/lib/sudo",
    "/var/lib/urandom",
    NULL
  };
  g_autofree gchar *xrd = g_strdup_printf ("/run/user/%ld", (long) geteuid ());
  gsize i;
  const gchar *member;

  g_return_val_if_fail (PV_IS_RUNTIME (self), FALSE);
  g_return_val_if_fail (!pv_bwrap_was_finished (bwrap), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!pv_bwrap_bind_usr (bwrap, self->runtime_files, "/", error))
    return FALSE;

  /* In the case where we have a mutable sysroot, we mount the overrides
   * as part of /usr. Make /overrides a symbolic link, to be nice to
   * older steam-runtime-tools versions. */

  if (self->mutable_sysroot != NULL)
    {
      g_assert (self->overrides_in_container[0] == '/');
      g_assert (g_strcmp0 (self->overrides_in_container, "/overrides") != 0);
      flatpak_bwrap_add_args (bwrap,
                              "--symlink",
                              &self->overrides_in_container[1],
                              "/overrides",
                              NULL);

      /* Also make a matching symbolic link on disk, to make it easier
       * to inspect the sysroot. */
      if (TEMP_FAILURE_RETRY (symlinkat (&self->overrides_in_container[1],
                                         self->mutable_sysroot_fd,
                                         "overrides")) != 0)
        return glnx_throw_errno_prefix (error,
                                        "Unable to create symlink \"%s/overrides\" -> \"%s\"",
                                        self->mutable_sysroot,
                                        &self->overrides_in_container[1]);
    }

  flatpak_bwrap_add_args (bwrap,
                          "--setenv", "XDG_RUNTIME_DIR", xrd,
                          "--tmpfs", "/run",
                          "--tmpfs", "/tmp",
                          "--tmpfs", "/var",
                          "--symlink", "../run", "/var/run",
                          NULL);

  if (!pv_bwrap_bind_usr (bwrap, "/", "/run/host", error))
    return FALSE;

  for (i = 0; i < G_N_ELEMENTS (bind_mutable); i++)
    {
      g_autofree gchar *path = g_build_filename (self->runtime_files,
                                                 bind_mutable[i],
                                                 NULL);
      g_autoptr(GDir) dir = NULL;

      dir = g_dir_open (path, 0, NULL);

      if (dir == NULL)
        continue;

      for (member = g_dir_read_name (dir);
           member != NULL;
           member = g_dir_read_name (dir))
        {
          g_autofree gchar *dest = g_build_filename ("/", bind_mutable[i],
                                                     member, NULL);
          g_autofree gchar *full = NULL;
          g_autofree gchar *target = NULL;

          if (g_strv_contains (dont_bind, dest))
            continue;

          full = g_build_filename (self->runtime_files,
                                   bind_mutable[i],
                                   member,
                                   NULL);
          target = glnx_readlinkat_malloc (-1, full, NULL, NULL);

          if (target != NULL)
            flatpak_bwrap_add_args (bwrap, "--symlink", target, dest, NULL);
          else
            flatpak_bwrap_add_args (bwrap, "--ro-bind", full, dest, NULL);
        }
    }

  if (g_file_test ("/etc/machine-id", G_FILE_TEST_EXISTS))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", "/etc/machine-id", "/etc/machine-id",
                              "--symlink", "/etc/machine-id",
                              "/var/lib/dbus/machine-id",
                              NULL);
    }
  else if (g_file_test ("/var/lib/dbus/machine-id", G_FILE_TEST_EXISTS))
    {
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", "/var/lib/dbus/machine-id",
                              "/etc/machine-id",
                              "--symlink", "/etc/machine-id",
                              "/var/lib/dbus/machine-id",
                              NULL);
    }

  if (g_file_test ("/etc/resolv.conf", G_FILE_TEST_EXISTS))
    flatpak_bwrap_add_args (bwrap,
                            "--ro-bind", "/etc/resolv.conf", "/etc/resolv.conf",
                            NULL);
  if (g_file_test ("/etc/host.conf", G_FILE_TEST_EXISTS))
    flatpak_bwrap_add_args (bwrap,
                            "--ro-bind", "/etc/host.conf", "/etc/host.conf",
                            NULL);
  if (g_file_test ("/etc/hosts", G_FILE_TEST_EXISTS))
    flatpak_bwrap_add_args (bwrap,
                            "--ro-bind", "/etc/hosts", "/etc/hosts",
                            NULL);

  /* TODO: Synthesize a passwd with only the user and nobody,
   * like Flatpak does? */
  if (g_file_test ("/etc/passwd", G_FILE_TEST_EXISTS))
    flatpak_bwrap_add_args (bwrap,
                            "--ro-bind", "/etc/passwd", "/etc/passwd",
                            NULL);
  if (g_file_test ("/etc/group", G_FILE_TEST_EXISTS))
    flatpak_bwrap_add_args (bwrap,
                            "--ro-bind", "/etc/group", "/etc/group",
                            NULL);

  if (self->flags & PV_RUNTIME_FLAGS_HOST_GRAPHICS_STACK)
    {
      if (!pv_runtime_use_host_graphics_stack (self, bwrap, error))
        return FALSE;
    }

  /* This needs to be done after pv_runtime_use_host_graphics_stack()
   * has decided whether to bring in the host system's libc. */
  if (self->flags & PV_RUNTIME_FLAGS_GENERATE_LOCALES)
    ensure_locales (self, self->any_libc_from_host, bwrap);

  /* These can add data fds to @bwrap, so they must come last - after
   * other functions stop using @bwrap as a basis for their own bwrap
   * invocations with flatpak_bwrap_append_bwrap().
   * Otherwise, when flatpak_bwrap_append_bwrap() calls
   * flatpak_bwrap_steal_fds(), it will make the original FlatpakBwrap
   * unusable. */

  flatpak_run_add_wayland_args (bwrap);
  flatpak_run_add_x11_args (bwrap, TRUE);
  flatpak_run_add_pulseaudio_args (bwrap);
  flatpak_run_add_session_dbus_args (bwrap);
  flatpak_run_add_system_dbus_args (bwrap);

  if (self->mutable_sysroot == NULL)
    {
      /* self->overrides is in a temporary directory that will be
       * cleaned up before we enter the container, so we need to convert
       * it into a series of --dir and --symlink instructions.
       *
       * We have to do this late, because it adds data fds. */
      pv_bwrap_copy_tree (bwrap, self->overrides, self->overrides_in_container);
    }

  /* /etc/localtime and /etc/resolv.conf can not exist (or be symlinks to
   * non-existing targets), in which case we don't want to attempt to create
   * bogus symlinks or bind mounts, as that will cause flatpak run to fail.
   */
  if (g_file_test ("/etc/localtime", G_FILE_TEST_EXISTS))
    {
      g_autofree char *target = NULL;
      gboolean is_reachable = FALSE;
      g_autofree char *tz = flatpak_get_timezone ();
      g_autofree char *timezone_content = g_strdup_printf ("%s\n", tz);

      target = glnx_readlinkat_malloc (-1, "/etc/localtime", NULL, NULL);

      if (target != NULL)
        {
          g_autoptr(GFile) base_file = NULL;
          g_autoptr(GFile) target_file = NULL;
          g_autofree char *target_canonical = NULL;

          base_file = g_file_new_for_path ("/etc");
          target_file = g_file_resolve_relative_path (base_file, target);
          target_canonical = g_file_get_path (target_file);

          is_reachable = g_str_has_prefix (target_canonical, "/usr/");
        }

      if (is_reachable)
        {
          flatpak_bwrap_add_args (bwrap,
                                  "--symlink", target, "/etc/localtime",
                                  NULL);
        }
      else
        {
          flatpak_bwrap_add_args (bwrap,
                                  "--ro-bind", "/etc/localtime", "/etc/localtime",
                                  NULL);
        }

      flatpak_bwrap_add_args_data (bwrap, "timezone",
                                   timezone_content, -1, "/etc/timezone",
                                   NULL);
    }

  return TRUE;
}

typedef enum
{
  TAKE_FROM_HOST_FLAGS_IF_DIR = (1 << 0),
  TAKE_FROM_HOST_FLAGS_IF_EXISTS = (1 << 1),
  TAKE_FROM_HOST_FLAGS_IF_CONTAINER_COMPATIBLE = (1 << 2),
  TAKE_FROM_HOST_FLAGS_COPY_FALLBACK = (1 << 3),
  TAKE_FROM_HOST_FLAGS_NONE = 0
} TakeFromHostFlags;

static gboolean
pv_runtime_take_from_host (PvRuntime *self,
                           FlatpakBwrap *bwrap,
                           const char *source_in_host,
                           const char *dest_in_container,
                           TakeFromHostFlags flags,
                           GError **error)
{
  g_return_val_if_fail (PV_IS_RUNTIME (self), FALSE);
  g_return_val_if_fail (!pv_bwrap_was_finished (bwrap), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (flags & TAKE_FROM_HOST_FLAGS_IF_DIR)
    {
      if (!g_file_test (source_in_host, G_FILE_TEST_IS_DIR))
        return TRUE;
    }

  if (flags & TAKE_FROM_HOST_FLAGS_IF_EXISTS)
    {
      if (!g_file_test (source_in_host, G_FILE_TEST_EXISTS))
        return TRUE;
    }

  if (self->mutable_sysroot != NULL)
    {
      /* Replace ${mutable_sysroot}/usr/lib/locale with a symlink to
       * /run/host/usr/lib/locale, or similar */
      g_autofree gchar *parent_in_container = NULL;
      g_autofree gchar *target = NULL;
      const char *base;
      glnx_autofd int parent_dirfd = -1;

      parent_in_container = g_path_get_dirname (dest_in_container);
      parent_dirfd = pv_resolve_in_sysroot (self->mutable_sysroot_fd,
                                            parent_in_container,
                                            PV_RESOLVE_FLAGS_MKDIR_P,
                                            NULL, error);

      if (parent_dirfd < 0)
        return FALSE;

      base = glnx_basename (dest_in_container);

      if (!glnx_shutil_rm_rf_at (parent_dirfd, base, NULL, error))
        return FALSE;

      /* If it isn't in /usr, /lib, etc., then the symlink will be
       * dangling and this probably isn't going to work. */
      if (!path_visible_in_run_host (source_in_host))
        {
          if (flags & TAKE_FROM_HOST_FLAGS_COPY_FALLBACK)
            {
              return glnx_file_copy_at (AT_FDCWD, source_in_host, NULL,
                                        parent_dirfd, base,
                                        GLNX_FILE_COPY_OVERWRITE,
                                        NULL, error);
            }
          else
            {
              g_warning ("\"%s\" is unlikely to appear in /run/host",
                         source_in_host);
              /* ... but try it anyway, it can't hurt */
            }
        }

      target = g_build_filename ("/run/host", source_in_host, NULL);

      if (TEMP_FAILURE_RETRY (symlinkat (target, parent_dirfd, base)) != 0)
        return glnx_throw_errno_prefix (error,
                                        "Unable to create symlink \"%s/%s\" -> \"%s\"",
                                        self->mutable_sysroot,
                                        dest_in_container, target);
    }
  else
    {
      /* We can't edit the runtime in-place, so tell bubblewrap to mount
       * a new version over the top */

      if (flags & TAKE_FROM_HOST_FLAGS_IF_CONTAINER_COMPATIBLE)
        {
          g_autofree gchar *dest = NULL;

          if (g_str_has_prefix (dest_in_container, "/usr/"))
            dest = g_build_filename (self->runtime_usr,
                                     dest_in_container + strlen ("/usr/"),
                                     NULL);
          else
            dest = g_build_filename (self->runtime_files,
                                     dest_in_container,
                                     NULL);

          if (g_file_test (source_in_host, G_FILE_TEST_IS_DIR))
            {
              if (!g_file_test (dest, G_FILE_TEST_IS_DIR))
                {
                  g_warning ("Not mounting \"%s\" over non-directory file or "
                             "nonexistent path \"%s\"",
                             source_in_host, dest);
                  return TRUE;
                }
            }
          else
            {
              if (!(g_file_test (dest, G_FILE_TEST_EXISTS) &&
                    g_file_test (dest, G_FILE_TEST_IS_DIR)))
                {
                  g_warning ("Not mounting \"%s\" over directory or "
                             "nonexistent path \"%s\"",
                             source_in_host, dest);
                  return TRUE;
                }
            }
        }

      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind", source_in_host, dest_in_container,
                              NULL);
    }

  return TRUE;
}

static gboolean
pv_runtime_remove_overridden_libraries (PvRuntime *self,
                                        RuntimeArchitecture *arch,
                                        GError **error)
{
  static const char * const libdirs[] = { "lib", "usr/lib", "usr/lib/mesa" };
  GHashTable *delete[G_N_ELEMENTS (libdirs)] = { NULL };
  GLnxDirFdIterator iters[G_N_ELEMENTS (libdirs)] = { { FALSE } };
  gchar *multiarch_libdirs[G_N_ELEMENTS (libdirs)] = { NULL };
  gboolean ret = FALSE;
  gsize i;

  g_return_val_if_fail (PV_IS_RUNTIME (self), FALSE);
  g_return_val_if_fail (arch != NULL, FALSE);
  g_return_val_if_fail (arch->ld_so != NULL, FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  /* Not applicable/possible if we don't have a mutable sysroot */
  g_return_val_if_fail (self->mutable_sysroot != NULL, FALSE);

  /* We have to figure out what we want to delete before we delete anything,
   * because we can't tell whether a symlink points to a library of a
   * particular SONAME if we already deleted the library. */
  for (i = 0; i < G_N_ELEMENTS (libdirs); i++)
    {
      glnx_autofd int libdir_fd = -1;
      struct dirent *dent;

      multiarch_libdirs[i] = g_build_filename (libdirs[i], arch->tuple, NULL);

      /* Mostly ignore error: if the library directory cannot be opened,
       * presumably we don't need to do anything with it... */
        {
          g_autoptr(GError) local_error = NULL;

          libdir_fd = pv_resolve_in_sysroot (self->mutable_sysroot_fd,
                                             multiarch_libdirs[i],
                                             PV_RESOLVE_FLAGS_READABLE,
                                             NULL, &local_error);

          if (libdir_fd < 0)
            {
              g_debug ("Cannot resolve \"%s\" in \"%s\", so no need to delete "
                       "libraries from it: %s",
                       multiarch_libdirs[i], self->mutable_sysroot,
                       local_error->message);
              g_clear_error (&local_error);
              continue;
            }
        }

      g_debug ("Removing overridden %s libraries from \"%s\" in \"%s\"...",
               arch->tuple, multiarch_libdirs[i], self->mutable_sysroot);

      if (!glnx_dirfd_iterator_init_take_fd (&libdir_fd, &iters[i], error))
        {
          glnx_prefix_error (error, "Unable to start iterating \"%s/%s\"",
                             self->mutable_sysroot,
                             multiarch_libdirs[i]);
          goto out;
        }

      delete[i] = g_hash_table_new_full (g_str_hash, g_str_equal,
                                         g_free, g_free);

      while (TRUE)
        {
          g_autoptr(Elf) elf = NULL;
          g_autoptr(GError) local_error = NULL;
          glnx_autofd int libfd = -1;
          g_autofree gchar *path = NULL;
          g_autofree gchar *soname = NULL;
          g_autofree gchar *target = NULL;

          if (!glnx_dirfd_iterator_next_dent_ensure_dtype (&iters[i], &dent,
                                                           NULL, error))
            return glnx_prefix_error (error, "Unable to iterate over \"%s/%s\"",
                                      self->mutable_sysroot,
                                      multiarch_libdirs[i]);

          if (dent == NULL)
            break;

          switch (dent->d_type)
            {
              case DT_REG:
              case DT_LNK:
                break;

              case DT_BLK:
              case DT_CHR:
              case DT_DIR:
              case DT_FIFO:
              case DT_SOCK:
              case DT_UNKNOWN:
              default:
                continue;
            }

          if (!g_str_has_prefix (dent->d_name, "lib"))
            continue;

          if (!g_str_has_suffix (dent->d_name, ".so") &&
              strstr (dent->d_name, ".so.") == NULL)
            continue;

          path = g_build_filename (multiarch_libdirs[i], dent->d_name, NULL);

          /* scope for soname_link */
            {
              g_autofree gchar *soname_link = NULL;

              soname_link = g_build_filename (arch->libdir_on_host,
                                              dent->d_name, NULL);

              /* If we found libfoo.so.1 in the container, and libfoo.so.1
               * also exists among the overrides, delete it. */
              if (g_file_test (soname_link, G_FILE_TEST_IS_SYMLINK))
                {
                  g_hash_table_replace (delete[i],
                                        g_strdup (dent->d_name),
                                        g_steal_pointer (&soname_link));
                  continue;
                }
            }

          target = glnx_readlinkat_malloc (iters[i].fd, dent->d_name,
                                           NULL, NULL);

          if (target != NULL)
            {
              g_autofree gchar *soname_link = NULL;

              soname_link = g_build_filename (arch->libdir_on_host,
                                              glnx_basename (target),
                                              NULL);

              /* If the symlink in the container points to
               * /foo/bar/libfoo.so.1, and libfoo.so.1 also exists among
               * the overrides, delete it. */
              if (g_file_test (soname_link, G_FILE_TEST_IS_SYMLINK))
                {
                  g_hash_table_replace (delete[i],
                                        g_strdup (dent->d_name),
                                        g_steal_pointer (&soname_link));
                  continue;
                }
            }

          libfd = pv_resolve_in_sysroot (self->mutable_sysroot_fd, path,
                                         PV_RESOLVE_FLAGS_READABLE, NULL,
                                         &local_error);

          if (libfd < 0)
            {
              g_warning ("Unable to open %s/%s for reading: %s",
                         self->mutable_sysroot, path, local_error->message);
              g_clear_error (&local_error);
              continue;
            }

          elf = pv_elf_open_fd (libfd, &local_error);

          if (elf != NULL)
            soname = pv_elf_get_soname (elf, &local_error);

          if (soname == NULL)
            {
              g_warning ("Unable to get SONAME of %s/%s: %s",
                         self->mutable_sysroot, path, local_error->message);
              g_clear_error (&local_error);
              continue;
            }

          /* If we found a library with SONAME libfoo.so.1 in the
           * container, and libfoo.so.1 also exists among the overrides,
           * delete it. */
            {
              g_autofree gchar *soname_link = NULL;

              soname_link = g_build_filename (arch->libdir_on_host, soname,
                                              NULL);

              if (g_file_test (soname_link, G_FILE_TEST_IS_SYMLINK))
                {
                  g_hash_table_replace (delete[i],
                                        g_strdup (dent->d_name),
                                        g_steal_pointer (&soname_link));
                  continue;
                }
            }
        }
    }

  for (i = 0; i < G_N_ELEMENTS (libdirs); i++)
    {
      if (delete[i] == NULL)
        continue;

      g_assert (iters[i].initialized);
      g_assert (iters[i].fd >= 0);

      GLNX_HASH_TABLE_FOREACH_KV (delete[i],
                                  const char *, name,
                                  const char *, reason)
        {
          g_autoptr(GError) local_error = NULL;

          g_debug ("Deleting %s/%s/%s because %s replaces it",
                   self->mutable_sysroot, multiarch_libdirs[i], name, reason);

          if (!glnx_unlinkat (iters[i].fd, name, 0, &local_error))
            {
              g_warning ("Unable to delete %s/%s/%s: %s",
                         self->mutable_sysroot, multiarch_libdirs[i],
                         name, local_error->message);
              g_clear_error (&local_error);
            }
        }
    }

  ret = TRUE;

out:
  for (i = 0; i < G_N_ELEMENTS (libdirs); i++)
    {
      g_clear_pointer (&delete[i], g_hash_table_unref);
      glnx_dirfd_iterator_clear (&iters[i]);
      g_free (multiarch_libdirs[i]);
    }

  return ret;
}

static gboolean
pv_runtime_take_ld_so_from_host (PvRuntime *self,
                                 RuntimeArchitecture *arch,
                                 const gchar *ld_so_in_runtime,
                                 FlatpakBwrap *bwrap,
                                 GError **error)
{
  g_autofree gchar *ld_so_in_host = NULL;

  g_debug ("Making host ld.so visible in container");

  ld_so_in_host = realpath (arch->ld_so, NULL);

  if (ld_so_in_host == NULL)
    return glnx_throw_errno_prefix (error,
                                    "Unable to determine host path to %s",
                                    arch->ld_so);

  g_debug ("Host path: %s -> %s", arch->ld_so, ld_so_in_host);
  /* Might be either absolute, or relative to the root */
  g_debug ("Container path: %s -> %s", arch->ld_so, ld_so_in_runtime);

  /* If we have a mutable sysroot, we can delete the interoperable path
   * and replace it with a symlink to what we want.
   * For example, overwrite /lib/ld-linux.so.2 with a symlink to
   * /run/host/lib/i386-linux-gnu/ld-2.30.so, or similar. This avoids
   * having to dereference a long chain of symlinks every time we run
   * an executable. */
  if (self->mutable_sysroot != NULL &&
      !pv_runtime_take_from_host (self, bwrap, ld_so_in_host,
                                  arch->ld_so, TAKE_FROM_HOST_FLAGS_NONE,
                                  error))
    return FALSE;

  /* If we don't have a mutable sysroot, we cannot replace symlinks,
   * and we also cannot mount onto symlinks (they get dereferenced),
   * so our only choice is to bind-mount
   * /lib/i386-linux-gnu/ld-2.30.so onto
   * /lib/i386-linux-gnu/ld-2.15.so and so on.
   *
   * In the mutable sysroot case, we don't strictly need to
   * overwrite /lib/i386-linux-gnu/ld-2.15.so with a symlink to
   * /run/host/lib/i386-linux-gnu/ld-2.30.so, but we might as well do
   * it anyway, for extra robustness: if we ever run a ld.so that
   * doesn't match the libc we are using (perhaps via an OS-specific,
   * non-standard path), that's pretty much a disaster, because it will
   * just crash. However, all of those (chains of) non-standard symlinks
   * will end up pointing to ld_so_in_runtime. */
  return pv_runtime_take_from_host (self, bwrap, ld_so_in_host,
                                    ld_so_in_runtime,
                                    TAKE_FROM_HOST_FLAGS_NONE, error);
}

static gboolean
pv_runtime_use_host_graphics_stack (PvRuntime *self,
                                    FlatpakBwrap *bwrap,
                                    GError **error)
{
  gsize i, j;
  g_autoptr(GString) dri_path = g_string_new ("");
  g_autoptr(GString) egl_path = g_string_new ("");
  g_autoptr(GString) vulkan_path = g_string_new ("");
  g_autoptr(GString) va_api_path = g_string_new ("");
  gboolean any_architecture_works = FALSE;
  g_autofree gchar *localedef = NULL;
  g_autofree gchar *ldconfig = NULL;
  g_autofree gchar *locale = NULL;
  g_autofree gchar *dir_on_host = NULL;
  g_autoptr(SrtSystemInfo) system_info = srt_system_info_new (NULL);
  g_autoptr(SrtObjectList) egl_icds = NULL;
  g_autoptr(SrtObjectList) vulkan_icds = NULL;
  g_autoptr(GPtrArray) egl_icd_details = NULL;      /* (element-type IcdDetails) */
  g_autoptr(GPtrArray) vulkan_icd_details = NULL;   /* (element-type IcdDetails) */
  g_autoptr(GPtrArray) va_api_icd_details = NULL;   /* (element-type IcdDetails) */
  guint n_egl_icds;
  guint n_vulkan_icds;
  const GList *icd_iter;
  gboolean all_libdrm_from_host = TRUE;
  g_autoptr(GHashTable) libdrm_data_from_host = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                                       g_free, NULL);
  g_autoptr(GHashTable) gconv_from_host = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                                 g_free, NULL);
  g_autofree gchar *best_libdrm_data_from_host = NULL;
  GHashTableIter iter;
  const gchar *gconv_path;

  g_return_val_if_fail (PV_IS_RUNTIME (self), FALSE);
  g_return_val_if_fail (!pv_bwrap_was_finished (bwrap), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  if (!pv_runtime_provide_container_access (self, error))
    return FALSE;

  g_debug ("Enumerating EGL ICDs on host system...");
  egl_icds = srt_system_info_list_egl_icds (system_info, multiarch_tuples);
  n_egl_icds = g_list_length (egl_icds);

  egl_icd_details = g_ptr_array_new_full (n_egl_icds,
                                          (GDestroyNotify) G_CALLBACK (icd_details_free));

  for (icd_iter = egl_icds, j = 0;
       icd_iter != NULL;
       icd_iter = icd_iter->next, j++)
    {
      SrtEglIcd *icd = icd_iter->data;
      const gchar *path = srt_egl_icd_get_json_path (icd);
      GError *local_error = NULL;

      if (!srt_egl_icd_check_error (icd, &local_error))
        {
          g_debug ("Failed to load EGL ICD #%" G_GSIZE_FORMAT  " from %s: %s",
                   j, path, local_error->message);
          g_clear_error (&local_error);
          continue;
        }

      g_debug ("EGL ICD #%" G_GSIZE_FORMAT " at %s: %s",
               j, path, srt_egl_icd_get_library_path (icd));

      g_ptr_array_add (egl_icd_details, icd_details_new (icd));
    }

  g_debug ("Enumerating Vulkan ICDs on host system...");
  vulkan_icds = srt_system_info_list_vulkan_icds (system_info,
                                                  multiarch_tuples);
  n_vulkan_icds = g_list_length (vulkan_icds);

  vulkan_icd_details = g_ptr_array_new_full (n_vulkan_icds,
                                             (GDestroyNotify) G_CALLBACK (icd_details_free));

  for (icd_iter = vulkan_icds, j = 0;
       icd_iter != NULL;
       icd_iter = icd_iter->next, j++)
    {
      SrtVulkanIcd *icd = icd_iter->data;
      const gchar *path = srt_vulkan_icd_get_json_path (icd);
      GError *local_error = NULL;

      if (!srt_vulkan_icd_check_error (icd, &local_error))
        {
          g_debug ("Failed to load Vulkan ICD #%" G_GSIZE_FORMAT " from %s: %s",
                   j, path, local_error->message);
          g_clear_error (&local_error);
          continue;
        }

      g_debug ("Vulkan ICD #%" G_GSIZE_FORMAT " at %s: %s",
               j, path, srt_vulkan_icd_get_library_path (icd));

      g_ptr_array_add (vulkan_icd_details, icd_details_new (icd));
    }

  /* We set this FALSE later if we decide not to use the host libc for
   * some architecture. */
  self->all_libc_from_host = TRUE;

  g_assert (multiarch_tuples[G_N_ELEMENTS (multiarch_tuples) - 1] == NULL);

  for (i = 0; i < G_N_ELEMENTS (multiarch_tuples) - 1; i++)
    {
      g_auto (RuntimeArchitecture) arch_on_stack = { i };
      RuntimeArchitecture *arch = &arch_on_stack;

      g_debug ("Checking for %s libraries...", multiarch_tuples[i]);

      if (runtime_architecture_init (arch, self))
        {
          g_auto(GStrv) dirs = NULL;
          g_autoptr(FlatpakBwrap) temp_bwrap = NULL;
          g_autofree gchar *this_dri_path_on_host = g_build_filename (arch->libdir_on_host,
                                                                      "dri", NULL);
          g_autofree gchar *this_dri_path_in_container = g_build_filename (arch->libdir_in_container,
                                                                           "dri", NULL);
          g_autofree gchar *libc = NULL;
          /* Can either be relative to the sysroot, or absolute */
          g_autofree gchar *ld_so_in_runtime = NULL;
          g_autofree gchar *libdrm = NULL;
          g_autoptr(SrtObjectList) vdpau_drivers = NULL;
          g_autoptr(SrtObjectList) va_api_drivers = NULL;

          if (self->mutable_sysroot != NULL)
            {
              glnx_autofd int fd = -1;

              fd = pv_resolve_in_sysroot (self->mutable_sysroot_fd,
                                          arch->ld_so,
                                          PV_RESOLVE_FLAGS_NONE,
                                          &ld_so_in_runtime,
                                          error);

              if (fd < 0)
                return FALSE;
            }
          else
            {
              /* Do it the hard way, by asking a process running in the
               * container (or at least a container resembling the one we
               * are going to use) to resolve it for us */
              temp_bwrap = flatpak_bwrap_new (NULL);
              flatpak_bwrap_add_args (temp_bwrap,
                                      self->bubblewrap,
                                      NULL);

              if (!pv_bwrap_bind_usr (temp_bwrap,
                                      self->runtime_files,
                                      "/",
                                      error))
                return FALSE;

              if (!pv_bwrap_bind_usr (temp_bwrap, "/", "/run/host", error))
                return FALSE;

              flatpak_bwrap_add_args (temp_bwrap,
                                      "env", "PATH=/usr/bin:/bin",
                                      "readlink", "-e", arch->ld_so,
                                      NULL);
              flatpak_bwrap_finish (temp_bwrap);

              ld_so_in_runtime = pv_capture_output (
                  (const char * const *) temp_bwrap->argv->pdata, NULL);

              g_clear_pointer (&temp_bwrap, flatpak_bwrap_free);
            }

          if (ld_so_in_runtime == NULL)
            {
              g_debug ("Container does not have %s so it cannot run "
                       "%s binaries",
                       arch->ld_so, arch->tuple);
              continue;
            }

          any_architecture_works = TRUE;
          g_debug ("Container path: %s -> %s",
                   arch->ld_so, ld_so_in_runtime);

          pv_search_path_append (dri_path, this_dri_path_in_container);

          g_mkdir_with_parents (arch->libdir_on_host, 0755);
          g_mkdir_with_parents (this_dri_path_on_host, 0755);

          g_debug ("Collecting graphics drivers from host system...");

          temp_bwrap = pv_bwrap_copy (self->container_access_adverb);
          flatpak_bwrap_add_args (temp_bwrap,
                                  arch->capsule_capture_libs,
                                  "--container", self->container_access,
                                  "--link-target", "/run/host",
                                  "--dest", arch->libdir_on_host,
                                  "--provider", "/",
                                  /* Mesa GLX, etc. */
                                  "gl:",
                                  /* Vulkan */
                                  "if-exists:if-same-abi:soname:libvulkan.so.1",
                                  /* VDPAU */
                                  "if-exists:if-same-abi:soname:libvdpau.so.1",
                                  /* VA-API */
                                  "if-exists:if-same-abi:soname:libva.so.1",
                                  "if-exists:if-same-abi:soname:libva-drm.so.1",
                                  "if-exists:if-same-abi:soname:libva-glx.so.1",
                                  "if-exists:if-same-abi:soname:libva-x11.so.1",
                                  "if-exists:if-same-abi:soname:libva.so.2",
                                  "if-exists:if-same-abi:soname:libva-drm.so.2",
                                  "if-exists:if-same-abi:soname:libva-glx.so.2",
                                  "if-exists:if-same-abi:soname:libva-x11.so.2",
                                  /* NVIDIA proprietary stack */
                                  "if-exists:even-if-older:soname-match:libEGL.so.*",
                                  "if-exists:even-if-older:soname-match:libEGL_nvidia.so.*",
                                  "if-exists:even-if-older:soname-match:libGL.so.*",
                                  "if-exists:even-if-older:soname-match:libGLESv1_CM.so.*",
                                  "if-exists:even-if-older:soname-match:libGLESv1_CM_nvidia.so.*",
                                  "if-exists:even-if-older:soname-match:libGLESv2.so.*",
                                  "if-exists:even-if-older:soname-match:libGLESv2_nvidia.so.*",
                                  "if-exists:even-if-older:soname-match:libGLX.so.*",
                                  "if-exists:even-if-older:soname-match:libGLX_nvidia.so.*",
                                  "if-exists:even-if-older:soname-match:libGLX_indirect.so.*",
                                  "if-exists:even-if-older:soname-match:libGLdispatch.so.*",
                                  "if-exists:even-if-older:soname-match:libOpenGL.so.*",
                                  "if-exists:even-if-older:soname-match:libcuda.so.*",
                                  "if-exists:even-if-older:soname-match:libglx.so.*",
                                  "if-exists:even-if-older:soname-match:libnvidia-cbl.so.*",
                                  "if-exists:even-if-older:soname-match:libnvidia-cfg.so.*",
                                  "if-exists:even-if-older:soname-match:libnvidia-compiler.so.*",
                                  "if-exists:even-if-older:soname-match:libnvidia-egl-wayland.so.*",
                                  "if-exists:even-if-older:soname-match:libnvidia-eglcore.so.*",
                                  "if-exists:even-if-older:soname-match:libnvidia-encode.so.*",
                                  "if-exists:even-if-older:soname-match:libnvidia-fatbinaryloader.so.*",
                                  "if-exists:even-if-older:soname-match:libnvidia-fbc.so.*",
                                  "if-exists:even-if-older:soname-match:libnvidia-glcore.so.*",
                                  "if-exists:even-if-older:soname-match:libnvidia-glsi.so.*",
                                  "if-exists:even-if-older:soname-match:libnvidia-glvkspirv.so.*",
                                  "if-exists:even-if-older:soname-match:libnvidia-ifr.so.*",
                                  "if-exists:even-if-older:soname-match:libnvidia-ml.so.*",
                                  "if-exists:even-if-older:soname-match:libnvidia-opencl.so.*",
                                  "if-exists:even-if-older:soname-match:libnvidia-opticalflow.so.*",
                                  "if-exists:even-if-older:soname-match:libnvidia-ptxjitcompiler.so.*",
                                  "if-exists:even-if-older:soname-match:libnvidia-rtcore.so.*",
                                  "if-exists:even-if-older:soname-match:libnvidia-tls.so.*",
                                  "if-exists:even-if-older:soname-match:libOpenCL.so.*",
                                  "if-exists:even-if-older:soname-match:libvdpau_nvidia.so.*",
                                  NULL);
          flatpak_bwrap_finish (temp_bwrap);

          if (!pv_bwrap_run_sync (temp_bwrap, NULL, error))
            return FALSE;

          g_clear_pointer (&temp_bwrap, flatpak_bwrap_free);

          g_debug ("Collecting %s EGL drivers from host system...",
                   arch->tuple);

          for (j = 0; j < egl_icd_details->len; j++)
            {
              IcdDetails *details = g_ptr_array_index (egl_icd_details, j);
              SrtEglIcd *icd = SRT_EGL_ICD (details->icd);

              if (!srt_egl_icd_check_error (icd, NULL))
                continue;

              details->resolved_library = srt_egl_icd_resolve_library_path (icd);
              g_assert (details->resolved_library != NULL);

              if (!bind_icd (self, arch, j, "glvnd", details, error))
                return FALSE;
            }

          g_debug ("Collecting %s Vulkan drivers from host system...",
                   arch->tuple);

          for (j = 0; j < vulkan_icd_details->len; j++)
            {
              IcdDetails *details = g_ptr_array_index (vulkan_icd_details, j);
              SrtVulkanIcd *icd = SRT_VULKAN_ICD (details->icd);

              if (!srt_vulkan_icd_check_error (icd, NULL))
                continue;

              details->resolved_library = srt_vulkan_icd_resolve_library_path (icd);
              g_assert (details->resolved_library != NULL);

              if (!bind_icd (self, arch, j, "vulkan", details, error))
                return FALSE;
            }

          g_debug ("Enumerating %s VDPAU ICDs on host system...", arch->tuple);
          vdpau_drivers = srt_system_info_list_vdpau_drivers (system_info,
                                                              arch->tuple,
                                                              SRT_DRIVER_FLAGS_NONE);

          for (icd_iter = vdpau_drivers; icd_iter != NULL; icd_iter = icd_iter->next)
            {
              g_autoptr(IcdDetails) details = icd_details_new (icd_iter->data);
              details->resolved_library = srt_vdpau_driver_resolve_library_path (details->icd);
              g_assert (details->resolved_library != NULL);
              g_assert (g_path_is_absolute (details->resolved_library));

              /* We avoid using the sequence number for VDPAU because they can only
               * be located in a single directory, so by definition we can't have
               * collisions */
              if (!bind_icd (self, arch, G_MAXSIZE, "vdpau", details, error))
                return FALSE;
            }

          g_debug ("Enumerating %s VA-API drivers on host system...",
                   arch->tuple);
          va_api_drivers = srt_system_info_list_va_api_drivers (system_info,
                                                                arch->tuple,
                                                                SRT_DRIVER_FLAGS_NONE);

          /* Guess that there will be about the same number of VA-API ICDs
           * for each word size. This only needs to be approximately right:
           * g_ptr_array_add() will resize the allocated buffer if needed. */
          if (va_api_icd_details == NULL)
            va_api_icd_details = g_ptr_array_new_full (g_list_length (va_api_drivers) * (G_N_ELEMENTS (multiarch_tuples) - 1),
                                                       (GDestroyNotify) G_CALLBACK (icd_details_free));

          for (icd_iter = va_api_drivers, j = 0; icd_iter != NULL; icd_iter = icd_iter->next, j++)
            {
              g_autoptr(IcdDetails) details = icd_details_new (icd_iter->data);
              details->resolved_library = srt_va_api_driver_resolve_library_path (details->icd);
              g_assert (details->resolved_library != NULL);
              g_assert (g_path_is_absolute (details->resolved_library));

              if (!bind_icd (self, arch, j, "dri", details, error))
                return FALSE;

              g_ptr_array_add (va_api_icd_details, g_steal_pointer (&details));
            }

          if (self->mutable_sysroot != NULL &&
              !pv_runtime_remove_overridden_libraries (self, arch, error))
            return FALSE;

          libc = g_build_filename (arch->libdir_on_host, "libc.so.6", NULL);

          /* If we are going to use the host system's libc6 (likely)
           * then we have to use its ld.so too. */
          if (g_file_test (libc, G_FILE_TEST_IS_SYMLINK))
            {
              g_autofree char *libc_target = NULL;

              if (!pv_runtime_take_ld_so_from_host (self, arch,
                                                    ld_so_in_runtime,
                                                    bwrap, error))
                return FALSE;

              /* Collect miscellaneous libraries that libc might dlopen.
               * At the moment this is just libidn2. */
              g_assert (temp_bwrap == NULL);
              temp_bwrap = pv_bwrap_copy (self->container_access_adverb);
              flatpak_bwrap_add_args (temp_bwrap,
                                      arch->capsule_capture_libs,
                                      "--container", self->container_access,
                                      "--link-target", "/run/host",
                                      "--dest", arch->libdir_on_host,
                                      "--provider", "/",
                                      "if-exists:libidn2.so.0",
                                      NULL);
              flatpak_bwrap_finish (temp_bwrap);

              if (!pv_bwrap_run_sync (temp_bwrap, NULL, error))
                return FALSE;

              g_clear_pointer (&temp_bwrap, flatpak_bwrap_free);

              libc_target = glnx_readlinkat_malloc (-1, libc, NULL, NULL);
              if (libc_target != NULL)
                {
                  g_autofree gchar *dir = NULL;
                  g_autofree gchar *gconv_dir_in_host = NULL;
                  gboolean found = FALSE;

                  dir = g_path_get_dirname (libc_target);

                  if (g_str_has_prefix (dir, "/run/host/"))
                    memmove (dir, dir + strlen ("/run/host"), strlen (dir) - strlen ("/run/host") + 1);

                  /* We are assuming that in the glibc "Makeconfig", $(libdir) was the same as
                   * $(slibdir) (this is the upstream default) or the same as "/usr$(slibdir)"
                   * (like in Debian without the mergerd /usr). We also assume that $(gconvdir)
                   * had its default value "$(libdir)/gconv".
                   * We check /usr first because otherwise, if the host is merged-/usr and the
                   * container is not, we might end up binding /lib instead of /usr/lib
                   * and that could cause issues. */
                  if (!g_str_has_prefix (dir, "/usr/"))
                    {
                      gconv_dir_in_host = g_build_filename ("/usr", dir, "gconv", NULL);
                      if (g_file_test (gconv_dir_in_host, G_FILE_TEST_IS_DIR))
                        {
                          g_hash_table_add (gconv_from_host, g_steal_pointer (&gconv_dir_in_host));
                          found = TRUE;
                        }
                    }
                  else
                    {
                      gconv_dir_in_host = g_build_filename (dir, "gconv", NULL);
                      if (g_file_test (gconv_dir_in_host, G_FILE_TEST_IS_DIR))
                        {
                          g_hash_table_add (gconv_from_host, g_steal_pointer (&gconv_dir_in_host));
                          found = TRUE;
                        }
                    }

                  if (!found)
                    {
                      g_debug ("We were expecting to have the gconv modules directory in the "
                               "host to be located in \"%s/gconv\", but instead it is missing",
                               dir);
                    }
                }

              self->any_libc_from_host = TRUE;
            }
          else
            {
              self->all_libc_from_host = FALSE;
            }

          libdrm = g_build_filename (arch->libdir_on_host, "libdrm.so.2", NULL);

          /* If we have libdrm.so.2 in overrides we also want to mount
           * ${prefix}/share/libdrm from the host. ${prefix} is derived from
           * the absolute path of libdrm.so.2 */
          if (g_file_test (libdrm, G_FILE_TEST_IS_SYMLINK))
            {
              g_autofree char *target = NULL;
              target = glnx_readlinkat_malloc (-1, libdrm, NULL, NULL);

              if (target != NULL)
                {
                  g_autofree gchar *dir = NULL;
                  g_autofree gchar *lib_multiarch = NULL;
                  g_autofree gchar *libdrm_dir_in_host = NULL;

                  dir = g_path_get_dirname (target);

                  lib_multiarch = g_build_filename ("/lib", arch->tuple, NULL);
                  if (g_str_has_suffix (dir, lib_multiarch))
                    dir[strlen (dir) - strlen (lib_multiarch)] = '\0';
                  else if (g_str_has_suffix (dir, "/lib64"))
                    dir[strlen (dir) - strlen ("/lib64")] = '\0';
                  else if (g_str_has_suffix (dir, "/lib32"))
                    dir[strlen (dir) - strlen ("/lib32")] = '\0';
                  else if (g_str_has_suffix (dir, "/lib"))
                    dir[strlen (dir) - strlen ("/lib")] = '\0';

                  if (g_str_has_prefix (dir, "/run/host"))
                    memmove (dir, dir + strlen ("/run/host"), strlen (dir) - strlen ("/run/host") + 1);

                  libdrm_dir_in_host = g_build_filename (dir, "share", "libdrm", NULL);

                  if (g_file_test (libdrm_dir_in_host, G_FILE_TEST_IS_DIR))
                    {
                      g_hash_table_add (libdrm_data_from_host, g_steal_pointer (&libdrm_dir_in_host));
                    }
                  else
                    {
                      g_debug ("We were expecting to have the libdrm directory in the host "
                               "to be located in \"%s\", but instead it is missing",
                               libdrm_dir_in_host);
                    }
                }
            }
          else
            {
              /* For at least a single architecture, libdrm is newer in the container */
              all_libdrm_from_host = FALSE;
            }

          /* Order matters: drivers from a later entry will overwrite
           * drivers from an earlier entry. Because we don't know whether
           * /lib and /usr/lib are 32- or 64-bit, we need to prioritize
           * libQUAL higher. Prioritize Debian-style multiarch higher
           * still, because it's completely unambiguous. */
          dirs = g_new0 (gchar *, 7);
          dirs[0] = g_strdup ("/lib");
          dirs[1] = g_strdup ("/usr/lib");
          dirs[2] = g_build_filename ("/", arch->libqual, NULL);
          dirs[3] = g_build_filename ("/usr", arch->libqual, NULL);
          dirs[4] = g_build_filename ("/lib", arch->tuple, NULL);
          dirs[5] = g_build_filename ("/usr", "lib", arch->tuple, NULL);

          for (j = 0; j < 6; j++)
            {
              if (!try_bind_dri (self, arch, bwrap, dirs[j], error))
                return FALSE;
            }
        }
    }

  if (!any_architecture_works)
    {
      GString *archs = g_string_new ("");

      g_assert (multiarch_tuples[G_N_ELEMENTS (multiarch_tuples) - 1] == NULL);

      for (i = 0; i < G_N_ELEMENTS (multiarch_tuples) - 1; i++)
        {
          if (archs->len > 0)
            g_string_append (archs, ", ");

          g_string_append (archs, multiarch_tuples[i]);
        }

      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "None of the supported CPU architectures are common to "
                   "the host system and the container (tried: %s)",
                   archs->str);
      g_string_free (archs, TRUE);
      return FALSE;
    }

  if (self->any_libc_from_host && !self->all_libc_from_host)
    {
      /*
       * This shouldn't happen. It would mean that there exist at least
       * two architectures (let's say aaa and bbb) for which we have:
       * host libc6:aaa < container libc6 < host libc6:bbb
       * (we know that the container's libc6:aaa and libc6:bbb are
       * constrained to be the same version because that's how multiarch
       * works).
       *
       * If the host system locales work OK with both the aaa and bbb
       * versions, let's assume they will also work with the intermediate
       * version from the container...
       */
      g_warning ("Using glibc from host system for some but not all "
                 "architectures! Arbitrarily using host locales.");
    }

  if (self->any_libc_from_host)
    {
      g_debug ("Making host locale data visible in container");

      if (!pv_runtime_take_from_host (self, bwrap,
                                      "/usr/lib/locale",
                                      "/usr/lib/locale",
                                      TAKE_FROM_HOST_FLAGS_IF_EXISTS,
                                      error))
        return FALSE;

      if (!pv_runtime_take_from_host (self, bwrap,
                                      "/usr/share/i18n",
                                      "/usr/share/i18n",
                                      TAKE_FROM_HOST_FLAGS_IF_EXISTS,
                                      error))
        return FALSE;

      localedef = g_find_program_in_path ("localedef");

      if (localedef == NULL)
        {
          g_warning ("Cannot find localedef in PATH");
        }
      else if (!pv_runtime_take_from_host (self, bwrap, localedef,
                                           "/usr/bin/localedef",
                                           TAKE_FROM_HOST_FLAGS_IF_CONTAINER_COMPATIBLE,
                                           error))
        {
          return FALSE;
        }

      locale = g_find_program_in_path ("locale");

      if (locale == NULL)
        {
          g_warning ("Cannot find locale in PATH");
        }
      else if (!pv_runtime_take_from_host (self, bwrap, locale,
                                           "/usr/bin/locale",
                                           TAKE_FROM_HOST_FLAGS_IF_CONTAINER_COMPATIBLE,
                                           error))
        {
          return FALSE;
        }

      ldconfig = g_find_program_in_path ("ldconfig");

      if (ldconfig == NULL
          && g_file_test ("/sbin/ldconfig", G_FILE_TEST_IS_EXECUTABLE))
        ldconfig = g_strdup ("/sbin/ldconfig");

      if (ldconfig == NULL
          && g_file_test ("/usr/sbin/ldconfig", G_FILE_TEST_IS_EXECUTABLE))
        ldconfig = g_strdup ("/usr/sbin/ldconfig");

      if (ldconfig == NULL)
        {
          g_warning ("Cannot find ldconfig in PATH, /sbin or /usr/sbin");
        }
      else if (!pv_runtime_take_from_host (self, bwrap,
                                           ldconfig,
                                           "/sbin/ldconfig",
                                           TAKE_FROM_HOST_FLAGS_NONE,
                                           error))
        {
          return FALSE;
        }

      g_debug ("Making host gconv modules visible in container");

      g_hash_table_iter_init (&iter, gconv_from_host);
      while (g_hash_table_iter_next (&iter, (gpointer *)&gconv_path, NULL))
        {
          if (!pv_runtime_take_from_host (self, bwrap,
                                          gconv_path,
                                          gconv_path,
                                          TAKE_FROM_HOST_FLAGS_IF_DIR,
                                          error))
            return FALSE;
        }
    }
  else
    {
      g_debug ("Using included locale data from container");
      g_debug ("Using included gconv modules from container");
    }

  if (g_hash_table_size (libdrm_data_from_host) > 0 && !all_libdrm_from_host)
    {
      /* See the explanation in the similar "any_libc_from_host && !all_libc_from_host"
       * case, above */
      g_warning ("Using libdrm.so.2 from host system for some but not all "
                 "architectures! Will take /usr/share/libdrm from host.");
    }

  if (g_hash_table_size (libdrm_data_from_host) == 1)
    {
      best_libdrm_data_from_host = g_strdup (pv_hash_table_get_arbitrary_key (libdrm_data_from_host));
    }
  else if (g_hash_table_size (libdrm_data_from_host) > 1)
    {
      g_warning ("Found more than one possible libdrm data directory from host");
      /* Prioritize "/usr/share/libdrm" if available. Otherwise randomly pick
       * the first directory in the hash table */
      if (g_hash_table_contains (libdrm_data_from_host, "/usr/share/libdrm"))
        best_libdrm_data_from_host = g_strdup ("/usr/share/libdrm");
      else
        best_libdrm_data_from_host = g_strdup (pv_hash_table_get_arbitrary_key (libdrm_data_from_host));
    }

  if (best_libdrm_data_from_host != NULL)
    {
      if (!pv_runtime_take_from_host (self, bwrap,
                                      best_libdrm_data_from_host,
                                      "/usr/share/libdrm",
                                      TAKE_FROM_HOST_FLAGS_IF_CONTAINER_COMPATIBLE,
                                      error))
        return FALSE;
    }

  g_debug ("Setting up EGL ICD JSON...");

  dir_on_host = g_build_filename (self->overrides,
                                  "share", "glvnd", "egl_vendor.d", NULL);

  if (g_mkdir_with_parents (dir_on_host, 0700) != 0)
    {
      glnx_throw_errno_prefix (error, "Unable to create %s", dir_on_host);
      return FALSE;
    }

  for (j = 0; j < egl_icd_details->len; j++)
    {
      IcdDetails *details = g_ptr_array_index (egl_icd_details, j);
      SrtEglIcd *icd = SRT_EGL_ICD (details->icd);
      gboolean need_host_json = FALSE;

      if (!srt_egl_icd_check_error (icd, NULL))
        continue;

      for (i = 0; i < G_N_ELEMENTS (multiarch_tuples) - 1; i++)
        {
          g_assert (i < G_N_ELEMENTS (details->kinds));
          g_assert (i < G_N_ELEMENTS (details->paths_in_container));

          if (details->kinds[i] == ICD_KIND_ABSOLUTE)
            {
              g_autoptr(SrtEglIcd) replacement = NULL;
              g_autofree gchar *json_on_host = NULL;
              g_autofree gchar *json_in_container = NULL;
              g_autofree gchar *json_base = NULL;

              g_assert (details->paths_in_container[i] != NULL);

              json_base = g_strdup_printf ("%" G_GSIZE_FORMAT "-%s.json",
                                           j, multiarch_tuples[i]);
              json_on_host = g_build_filename (dir_on_host, json_base, NULL);
              json_in_container = g_build_filename (self->overrides_in_container,
                                                    "share", "glvnd",
                                                    "egl_vendor.d",
                                                    json_base, NULL);

              replacement = srt_egl_icd_new_replace_library_path (icd,
                                                                  details->paths_in_container[i]);

              if (!srt_egl_icd_write_to_file (replacement, json_on_host,
                                                 error))
                return FALSE;

              pv_search_path_append (egl_path, json_in_container);
            }
          else if (details->kinds[i] == ICD_KIND_SONAME)
            {
              need_host_json = TRUE;
            }
        }

      if (need_host_json)
        {
          g_autofree gchar *json_in_container = NULL;
          g_autofree gchar *json_base = NULL;
          const char *json_on_host = srt_egl_icd_get_json_path (icd);

          json_base = g_strdup_printf ("%" G_GSIZE_FORMAT ".json", j);
          json_in_container = g_build_filename (self->overrides_in_container,
                                                "share", "glvnd",
                                                "egl_vendor.d",
                                                json_base, NULL);

          if (!pv_runtime_take_from_host (self, bwrap,
                                          json_on_host,
                                          json_in_container,
                                          TAKE_FROM_HOST_FLAGS_COPY_FALLBACK,
                                          error))
            return FALSE;

          pv_search_path_append (egl_path, json_in_container);
        }
    }

  g_debug ("Setting up Vulkan ICD JSON...");

  dir_on_host = g_build_filename (self->overrides,
                                  "share", "vulkan", "icd.d", NULL);

  if (g_mkdir_with_parents (dir_on_host, 0700) != 0)
    {
      glnx_throw_errno_prefix (error, "Unable to create %s", dir_on_host);
      return FALSE;
    }

  for (j = 0; j < vulkan_icd_details->len; j++)
    {
      IcdDetails *details = g_ptr_array_index (vulkan_icd_details, j);
      SrtVulkanIcd *icd = SRT_VULKAN_ICD (details->icd);
      gboolean need_host_json = FALSE;

      if (!srt_vulkan_icd_check_error (icd, NULL))
        continue;

      for (i = 0; i < G_N_ELEMENTS (multiarch_tuples) - 1; i++)
        {
          g_assert (i < G_N_ELEMENTS (details->kinds));
          g_assert (i < G_N_ELEMENTS (details->paths_in_container));

          if (details->kinds[i] == ICD_KIND_ABSOLUTE)
            {
              g_autoptr(SrtVulkanIcd) replacement = NULL;
              g_autofree gchar *json_on_host = NULL;
              g_autofree gchar *json_in_container = NULL;
              g_autofree gchar *json_base = NULL;

              g_assert (details->paths_in_container[i] != NULL);

              json_base = g_strdup_printf ("%" G_GSIZE_FORMAT "-%s.json",
                                           j, multiarch_tuples[i]);
              json_on_host = g_build_filename (dir_on_host, json_base, NULL);
              json_in_container = g_build_filename (self->overrides_in_container,
                                                    "share", "vulkan", "icd.d",
                                                    json_base, NULL);

              replacement = srt_vulkan_icd_new_replace_library_path (icd,
                                                                     details->paths_in_container[i]);

              if (!srt_vulkan_icd_write_to_file (replacement, json_on_host,
                                                 error))
                return FALSE;

              pv_search_path_append (vulkan_path, json_in_container);
            }
          else if (details->kinds[i] == ICD_KIND_SONAME)
            {
              need_host_json = TRUE;
            }
        }

      if (need_host_json)
        {
          g_autofree gchar *json_in_container = NULL;
          g_autofree gchar *json_base = NULL;
          const char *json_on_host = srt_vulkan_icd_get_json_path (icd);

          json_base = g_strdup_printf ("%" G_GSIZE_FORMAT ".json", j);
          json_in_container = g_build_filename (self->overrides_in_container,
                                                "share", "vulkan", "icd.d",
                                                json_base, NULL);

          if (!pv_runtime_take_from_host (self, bwrap,
                                          json_on_host,
                                          json_in_container,
                                          TAKE_FROM_HOST_FLAGS_COPY_FALLBACK,
                                          error))
            return FALSE;

          pv_search_path_append (vulkan_path, json_in_container);
        }
    }

  for (j = 0; j < va_api_icd_details->len; j++)
    {
      IcdDetails *details = g_ptr_array_index (va_api_icd_details, j);

      for (i = 0; i < G_N_ELEMENTS (multiarch_tuples) - 1; i++)
        {
          g_assert (i < G_N_ELEMENTS (details->kinds));
          g_assert (i < G_N_ELEMENTS (details->paths_in_container));

          if (details->kinds[i] == ICD_KIND_NONEXISTENT)
            {
              continue;
            }
          else
            {
              g_autofree gchar *parent = NULL;

              g_assert (details->kinds[i] == ICD_KIND_ABSOLUTE);

              parent = g_path_get_dirname (details->paths_in_container[i]);
              pv_search_path_append (va_api_path, parent);
            }
        }
    }

  if (dri_path->len != 0)
      flatpak_bwrap_add_args (bwrap,
                              "--setenv", "LIBGL_DRIVERS_PATH", dri_path->str,
                              NULL);
  else
      flatpak_bwrap_add_args (bwrap,
                              "--unsetenv", "LIBGL_DRIVERS_PATH",
                              NULL);

  if (egl_path->len != 0)
      flatpak_bwrap_add_args (bwrap,
                              "--setenv", "__EGL_VENDOR_LIBRARY_FILENAMES",
                              egl_path->str, NULL);
  else
      flatpak_bwrap_add_args (bwrap,
                              "--unsetenv", "__EGL_VENDOR_LIBRARY_FILENAMES",
                              NULL);

  flatpak_bwrap_add_args (bwrap,
                          "--unsetenv", "__EGL_VENDOR_LIBRARY_DIRS",
                          NULL);

  if (vulkan_path->len != 0)
      flatpak_bwrap_add_args (bwrap,
                              "--setenv", "VK_ICD_FILENAMES",
                              vulkan_path->str, NULL);
  else
      flatpak_bwrap_add_args (bwrap,
                              "--unsetenv", "VK_ICD_FILENAMES",
                              NULL);

  if (va_api_path->len != 0)
      flatpak_bwrap_add_args (bwrap,
                              "--setenv", "LIBVA_DRIVERS_PATH",
                              va_api_path->str, NULL);
  else
      flatpak_bwrap_add_args (bwrap,
                              "--unsetenv", "LIBVA_DRIVERS_PATH",
                              NULL);

  /* We binded the VDPAU drivers in "%{libdir}/vdpau".
   * Unfortunately VDPAU_DRIVER_PATH can hold just a single path, so we can't
   * easily list both x86_64 and i386 drivers path.
   * As a workaround we set VDPAU_DRIVER_PATH to
   * "/overrides/lib/${PLATFORM}-linux-gnu/vdpau". And because we can't control
   * the ${PLATFORM} placeholder value we also create symlinks from `i486`, up
   * to `i686`, to the library directory `i386` that we expect to have
   * already. */
  flatpak_bwrap_add_args (bwrap,
                          "--setenv", "VDPAU_DRIVER_PATH",
                          NULL);
  flatpak_bwrap_add_arg_printf (bwrap,
                                "%s/lib/${PLATFORM}-linux-gnu/vdpau",
                                self->overrides_in_container);

  static const char * const extra_multiarch_tuples[] =
  {
    "i486-linux-gnu",
    "i586-linux-gnu",
    "i686-linux-gnu",
    NULL
  };

  g_autofree gchar *i386_libdir_on_host = g_build_filename (self->overrides, "lib",
                                                            "i386-linux-gnu",
                                                            NULL);

  for (i = 0; i < G_N_ELEMENTS (extra_multiarch_tuples) - 1; i++)
    {
      g_autofree gchar *extra_libdir_on_host = g_build_filename (self->overrides, "lib",
                                                                 extra_multiarch_tuples[i],
                                                                 NULL);

      if (!g_file_test (extra_libdir_on_host, G_FILE_TEST_EXISTS) &&
          g_file_test (i386_libdir_on_host, G_FILE_TEST_IS_DIR))
        {
          g_unlink (extra_libdir_on_host);

          if (symlink ("i386-linux-gnu", extra_libdir_on_host) != 0)
            return glnx_throw_errno_prefix (error,
                                            "Unable to create symlink %s -> i386-linux-gnu",
                                            extra_libdir_on_host);
        }
    }

  return TRUE;
}

gboolean
pv_runtime_bind (PvRuntime *self,
                 FlatpakBwrap *bwrap,
                 GError **error)
{
  g_autofree gchar *pressure_vessel_prefix = NULL;

  g_return_val_if_fail (PV_IS_RUNTIME (self), FALSE);
  g_return_val_if_fail (!pv_bwrap_was_finished (bwrap), FALSE);
  g_return_val_if_fail (error == NULL || *error == NULL, FALSE);

  /* Start with just the root tmpfs (which appears automatically)
   * and the standard API filesystems */
  pv_bwrap_add_api_filesystems (bwrap);

  if (!bind_runtime (self, bwrap, error))
    return FALSE;

  /* steam-runtime-system-info uses this to detect pressure-vessel, so we
   * need to create it even if it will be empty */
  flatpak_bwrap_add_args (bwrap,
                          "--dir",
                          "/run/pressure-vessel",
                          NULL);

  pressure_vessel_prefix = g_path_get_dirname (self->tools_dir);

  /* Make sure pressure-vessel itself is visible there. */
  if (self->mutable_sysroot != NULL)
    {
      g_autofree gchar *dest = NULL;
      glnx_autofd int parent_dirfd = -1;

      parent_dirfd = pv_resolve_in_sysroot (self->mutable_sysroot_fd,
                                            "/usr/lib/pressure-vessel",
                                            PV_RESOLVE_FLAGS_MKDIR_P,
                                            NULL, error);

      if (parent_dirfd < 0)
        return FALSE;

      if (!glnx_shutil_rm_rf_at (parent_dirfd, "from-host", NULL, error))
        return FALSE;

      dest = glnx_fdrel_abspath (parent_dirfd, "from-host");

      if (!pv_cheap_tree_copy (pressure_vessel_prefix, dest, error))
        return FALSE;

      self->with_lock_in_container = "/usr/lib/pressure-vessel/from-host/bin/pressure-vessel-with-lock";
    }
  else
    {
      flatpak_bwrap_add_args (bwrap,
                              "--ro-bind",
                              pressure_vessel_prefix,
                              "/run/pressure-vessel/pv-from-host",
                              NULL);
      self->with_lock_in_container = "/run/pressure-vessel/pv-from-host/bin/pressure-vessel-with-lock";
    }

  pv_runtime_set_search_paths (self, bwrap);

  return TRUE;
}

void
pv_runtime_set_search_paths (PvRuntime *self,
                             FlatpakBwrap *bwrap)
{
  g_autoptr(GString) ld_library_path = g_string_new ("");
  gsize i;

  /* TODO: Adapt the use_ld_so_cache code from Flatpak instead
   * of setting LD_LIBRARY_PATH, for better robustness against
   * games that set their own LD_LIBRARY_PATH ignoring what they
   * got from the environment */
  g_assert (multiarch_tuples[G_N_ELEMENTS (multiarch_tuples) - 1] == NULL);

    for (i = 0; i < G_N_ELEMENTS (multiarch_tuples) - 1; i++)
      {
        g_autofree gchar *ld_path = NULL;

        ld_path = g_build_filename (self->overrides_in_container, "lib",
                                    multiarch_tuples[i], NULL);

        pv_search_path_append (ld_library_path, ld_path);
      }

  /* This would be filtered out by a setuid bwrap, so we have to go
   * via --setenv. */
  flatpak_bwrap_add_args (bwrap,
                          /* The PATH from outside the container doesn't
                           * really make sense inside the container:
                           * in principle the layout could be totally
                           * different. */
                          "--setenv", "PATH", "/usr/bin:/bin",
                          "--setenv", "LD_LIBRARY_PATH",
                          ld_library_path->str,
                          NULL);
}

static void
pv_runtime_initable_iface_init (GInitableIface *iface,
                                gpointer unused G_GNUC_UNUSED)
{
  iface->init = pv_runtime_initable_init;
}
