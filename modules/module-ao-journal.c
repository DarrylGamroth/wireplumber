/* WirePlumber
 * SPDX-License-Identifier: MIT
 */
#include <wp/wp.h>
#include <sqlite3.h>
#include <spa/pod/pod.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

WP_DEFINE_LOCAL_LOG_TOPIC ("m-ao-journal")

#define JOURNAL_QUEUE_LIMIT 8u
#define REQUEST_LIMIT (16u * 1024u)
#define REPLY_LIMIT (64u * 1024u)
#define BUSY_TIMEOUT_MS 1000

typedef struct {
  gint64 id;
  gchar *session;
  gchar *event;
  GBytes *record;
  gchar *client_name;
  gchar *operation_key;
  guint client_pid;
} JournalJob;

/* Only the core context touches admission and the FIFO. One worker at a time
 * performs all filesystem/SQLite work, preserving admission order. Each GTask
 * holds the plugin alive and returns on the core context. No worker accesses
 * PipeWire or emits a Lua signal. Jobs are never automatically retried. */
struct _WpAoJournal {
  WpPlugin parent;
  gchar *path;
  GQueue jobs;
  gint64 next_id;
  gboolean enabled;
  gboolean running;
};
G_DECLARE_FINAL_TYPE (WpAoJournal, wp_ao_journal, WP, AO_JOURNAL, WpPlugin)
G_DEFINE_TYPE (WpAoJournal, wp_ao_journal, WP_TYPE_PLUGIN)

enum { ACTION_APPEND, SIGNAL_COMPLETED, N_SIGNALS };
static guint signals[N_SIGNALS];
static void start_next (WpAoJournal *self, GMainContext *context);

static void
job_free (JournalJob *job)
{
  g_free (job->session);
  g_free (job->event);
  g_free (job->client_name);
  g_free (job->operation_key);
  g_bytes_unref (job->record);
  g_free (job);
}

/* The final directory and DB must belong to this user and be private. Never
 * chmod a preexisting user object, or follow a final-component symlink. */
static gboolean
prepare_path (const gchar *path, GError **error)
{
  g_autofree gchar *directory = g_path_get_dirname (path);
  struct stat st;
  int fd;
  if (g_mkdir_with_parents (directory, 0700) < 0 ||
      lstat (directory, &st) < 0 || !S_ISDIR (st.st_mode) ||
      st.st_uid != getuid () || (st.st_mode & 0077))
    goto invalid;
  fd = open (path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
  if (fd < 0)
    goto invalid;
  gboolean valid = fstat (fd, &st) == 0 && S_ISREG (st.st_mode) &&
      st.st_uid == getuid () && !(st.st_mode & 0077) && st.st_nlink == 1;
  close (fd);
  if (valid)
    return TRUE;
invalid:
  g_set_error_literal (error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
      "journal requires a private owned directory and regular database file");
  return FALSE;
}

/* Called exclusively by the worker, also directly exercised by focused tests. */
static gboolean
append_record (const gchar *path, JournalJob *job, GError **error)
{
  sqlite3 *db = NULL;
  sqlite3_stmt *statement = NULL;
  gboolean ok = FALSE;
  gsize size;
  const void *bytes = g_bytes_get_data (job->record, &size);
  if (!prepare_path (path, error))
    return FALSE;
  int result = sqlite3_open_v2 (path, &db,
      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, NULL);
  if (result != SQLITE_OK)
    goto failed;
  sqlite3_busy_timeout (db, BUSY_TIMEOUT_MS);
  /* ConfigurationStore owns its separate configuration_schema table. This
   * additive table deliberately does not claim the database's user_version. */
  result = sqlite3_exec (db,
      "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; BEGIN IMMEDIATE;"
      "CREATE TABLE IF NOT EXISTS operation_events ("
      "sequence INTEGER PRIMARY KEY AUTOINCREMENT,"
      "session_id TEXT NOT NULL,"
      "event TEXT NOT NULL CHECK(event IN ('requested','completed','unknown')),"
      "record BLOB NOT NULL,"
      "operation_key TEXT NOT NULL,"
      "client_name TEXT NOT NULL DEFAULT '',"
      "client_pid INTEGER NOT NULL DEFAULT 0,"
      "recorded_at TEXT NOT NULL DEFAULT(strftime('%Y-%m-%dT%H:%M:%fZ','now')));",
      NULL, NULL, NULL);
  if (result != SQLITE_OK)
    goto failed;
  /* Upgrade older operation journals without touching configuration versions.
   * BEGIN IMMEDIATE above serializes this check with other store users. */
  result = sqlite3_prepare_v2 (db, "PRAGMA table_info(operation_events)", -1,
      &statement, NULL);
  if (result != SQLITE_OK)
    goto failed;
  gboolean has_name = FALSE, has_pid = FALSE, has_key = FALSE;
  while ((result = sqlite3_step (statement)) == SQLITE_ROW) {
    const gchar *column = (const gchar *) sqlite3_column_text (statement, 1);
    has_name |= g_strcmp0 (column, "client_name") == 0;
    has_pid |= g_strcmp0 (column, "client_pid") == 0;
    has_key |= g_strcmp0 (column, "operation_key") == 0;
  }
  sqlite3_finalize (statement);
  statement = NULL;
  if (result != SQLITE_DONE)
    goto failed;
  if (!has_name && (result = sqlite3_exec (db,
      "ALTER TABLE operation_events ADD COLUMN client_name TEXT NOT NULL DEFAULT ''",
      NULL, NULL, NULL)) != SQLITE_OK)
    goto failed;
  if (!has_pid && (result = sqlite3_exec (db,
      "ALTER TABLE operation_events ADD COLUMN client_pid INTEGER NOT NULL DEFAULT 0",
      NULL, NULL, NULL)) != SQLITE_OK)
    goto failed;
  if (!has_key && (result = sqlite3_exec (db,
      "ALTER TABLE operation_events ADD COLUMN operation_key TEXT NOT NULL DEFAULT '';"
      "UPDATE operation_events SET operation_key='legacy:' || sequence WHERE operation_key='';",
      NULL, NULL, NULL)) != SQLITE_OK)
    goto failed;
  result = sqlite3_exec (db,
      "CREATE UNIQUE INDEX IF NOT EXISTS operation_events_key_event ON operation_events(operation_key,event);"
      "CREATE INDEX IF NOT EXISTS operation_events_event_sequence ON operation_events(event,sequence);"
      "CREATE INDEX IF NOT EXISTS operation_events_session_event_sequence ON operation_events(session_id,event,sequence);",
      NULL, NULL, NULL);
  if (result != SQLITE_OK)
    goto failed;
  result = sqlite3_prepare_v2 (db,
      "INSERT INTO operation_events(session_id,event,record,client_name,client_pid,operation_key) VALUES (?,?,?,?,?,?)",
      -1, &statement, NULL);
  if (result != SQLITE_OK)
    goto failed;
  result = sqlite3_bind_text (statement, 1, job->session, -1, SQLITE_STATIC);
  if (result == SQLITE_OK)
    result = sqlite3_bind_text (statement, 2, job->event, -1, SQLITE_STATIC);
  if (result == SQLITE_OK)
    result = sqlite3_bind_blob (statement, 3, bytes, (int) size, SQLITE_STATIC);
  if (result == SQLITE_OK)
    result = sqlite3_bind_text (statement, 4, job->client_name, -1, SQLITE_STATIC);
  if (result == SQLITE_OK)
    result = sqlite3_bind_int64 (statement, 5, job->client_pid);
  if (result == SQLITE_OK)
    result = sqlite3_bind_text (statement, 6, job->operation_key, -1, SQLITE_STATIC);
  if (result == SQLITE_OK)
    result = sqlite3_step (statement);
  if (result != SQLITE_DONE)
    goto failed;
  result = sqlite3_exec (db, "COMMIT", NULL, NULL, NULL);
  if (result != SQLITE_OK)
    goto failed;
  ok = TRUE;
  goto out;
failed:
  /* Failure can be ambiguous after SQLite has issued writes; the caller must
   * never infer rollback or retry a scientific operation from this result. */
  g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
      "journal append failed (%d): %s", result,
      db ? sqlite3_errmsg (db) : "database unavailable");
out:
  sqlite3_finalize (statement);
  if (db) {
    if (!ok)
      sqlite3_exec (db, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_close (db);
  }
  return ok;
}

static void
append_worker (GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
  WpAoJournal *self = source;
  g_autoptr (GError) error = NULL;
  if (append_record (self->path, data, &error))
    g_task_return_boolean (task, TRUE);
  else
    g_task_return_error (task, g_steal_pointer (&error));
}

static void
append_done (GObject *object, GAsyncResult *result, gpointer data)
{
  WpAoJournal *self = WP_AO_JOURNAL (object);
  JournalJob *job = g_task_get_task_data (G_TASK (result));
  g_autoptr (GError) error = NULL;
  gboolean ok = g_task_propagate_boolean (G_TASK (result), &error);
  /* Keep running true across the signal: reentrant appends queue behind jobs
   * admitted earlier, and cannot launch a second concurrent writer. */
  g_signal_emit (self, signals[SIGNAL_COMPLETED], 0, job->id, ok,
      error ? error->message : "");
  self->running = FALSE;
  start_next (self, g_task_get_context (G_TASK (result)));
}

static void
start_next (WpAoJournal *self, GMainContext *context)
{
  g_autoptr (GTask) task = NULL;
  if (self->running || g_queue_is_empty (&self->jobs))
    return;
  self->running = TRUE;
  /* GTask retains this context across core destruction. Pass it along when
   * draining rather than dereferencing the plugin's weak WpCore reference. */
  g_main_context_push_thread_default (context);
  task = g_task_new (self, NULL, append_done, NULL);
  g_main_context_pop_thread_default (context);
  g_task_set_task_data (task, g_queue_pop_head (&self->jobs),
      (GDestroyNotify) job_free);
  g_task_run_in_thread (task, append_worker);
}

static gint64
wp_ao_journal_append (WpAoJournal *self, const gchar *session,
    const gchar *event, WpSpaPod *record, const gchar *client_name, guint client_pid,
    const gchar *operation_key)
{
  g_autoptr (WpCore) core = wp_object_get_core (WP_OBJECT (self));
  const struct spa_pod *pod = record ? wp_spa_pod_get_spa_pod (record) : NULL;
  guint limit = g_strcmp0 (event, "requested") == 0 ? REQUEST_LIMIT : REPLY_LIMIT;
  if (!core || !g_main_context_is_owner (wp_core_get_g_main_context (core)) ||
      !self->enabled || !session || !session[0] || strlen (session) > 128 ||
      !client_name || strlen (client_name) > 1024 || !client_pid ||
      !operation_key || !operation_key[0] || strlen (operation_key) > 512 ||
      !g_utf8_validate (operation_key, -1, NULL) ||
      !g_utf8_validate (client_name, -1, NULL) ||
      !g_utf8_validate (session, -1, NULL) || !pod ||
      (g_strcmp0 (event, "requested") && g_strcmp0 (event, "completed") &&
          g_strcmp0 (event, "unknown")) || pod->size > limit - sizeof (*pod) ||
      g_queue_get_length (&self->jobs) + self->running >= JOURNAL_QUEUE_LIMIT ||
      self->next_id == G_MAXINT64)
    return 0;
  JournalJob *job = g_new0 (JournalJob, 1);
  job->id = ++self->next_id;
  job->session = g_strdup (session);
  job->event = g_strdup (event);
  job->client_name = g_strdup (client_name);
  job->client_pid = client_pid;
  job->operation_key = g_strdup (operation_key);
  job->record = g_bytes_new (pod, SPA_POD_SIZE (pod));
  gint64 id = job->id;
  g_queue_push_tail (&self->jobs, job);
  start_next (self, wp_core_get_g_main_context (core));
  return id;
}

static void
wp_ao_journal_enable (WpPlugin *plugin, WpTransition *transition)
{
  WP_AO_JOURNAL (plugin)->enabled = TRUE;
  wp_object_update_features (WP_OBJECT (plugin), WP_PLUGIN_FEATURE_ENABLED, 0);
}

static void
wp_ao_journal_disable (WpPlugin *plugin)
{
  WpAoJournal *self = WP_AO_JOURNAL (plugin);
  self->enabled = FALSE;
  /* Admitted records still drain to disk. Dropping an in-flight terminal
   * record on disable could conceal an already-issued scientific effect. */
}

static void
wp_ao_journal_finalize (GObject *object)
{
  WpAoJournal *self = WP_AO_JOURNAL (object);
  g_queue_clear_full (&self->jobs, (GDestroyNotify) job_free);
  g_free (self->path);
  G_OBJECT_CLASS (wp_ao_journal_parent_class)->finalize (object);
}

static void
wp_ao_journal_init (WpAoJournal *self)
{
  g_queue_init (&self->jobs);
}

static void
wp_ao_journal_class_init (WpAoJournalClass *klass)
{
  G_OBJECT_CLASS (klass)->finalize = wp_ao_journal_finalize;
  WP_PLUGIN_CLASS (klass)->enable = wp_ao_journal_enable;
  WP_PLUGIN_CLASS (klass)->disable = wp_ao_journal_disable;
  signals[ACTION_APPEND] = g_signal_new_class_handler ("append",
      G_TYPE_FROM_CLASS (klass), G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
      (GCallback) wp_ao_journal_append, NULL, NULL, NULL, G_TYPE_INT64, 6,
      G_TYPE_STRING, G_TYPE_STRING, WP_TYPE_SPA_POD, G_TYPE_STRING, G_TYPE_UINT,
      G_TYPE_STRING);
  signals[SIGNAL_COMPLETED] = g_signal_new ("completed", G_TYPE_FROM_CLASS (klass),
      G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 3,
      G_TYPE_INT64, G_TYPE_BOOLEAN, G_TYPE_STRING);
}

WP_PLUGIN_EXPORT GObject *
wireplumber__module_init (WpCore *core, WpSpaJson *args, GError **error)
{
  g_autofree gchar *path = NULL, *name = NULL;
  if (!args || !wp_spa_json_is_object (args) ||
      !wp_spa_json_object_get (args, "path", "s", &path, NULL) ||
      !g_path_is_absolute (path)) {
    g_set_error_literal (error, WP_DOMAIN_LIBRARY, WP_LIBRARY_ERROR_INVALID_ARGUMENT,
        "ao-journal requires an absolute database path");
    return NULL;
  }
  wp_spa_json_object_get (args, "plugin.name", "s", &name, NULL);
  WpAoJournal *self = g_object_new (wp_ao_journal_get_type (),
      "name", name ? name : "ao-journal", "core", core, NULL);
  self->path = g_steal_pointer (&path);
  return G_OBJECT (self);
}
