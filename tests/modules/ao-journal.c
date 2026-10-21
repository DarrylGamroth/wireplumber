/* WirePlumber
 * SPDX-License-Identifier: MIT
 * Exercise the worker with a disconnected private core; no live service.
 */
#include "../../modules/module-ao-journal.c"
#include <glib/gstdio.h>

typedef struct {
  GMainContext *context;
  WpCore *core;
  WpAoJournal *journal;
  gchar *directory;
  gchar *path;
  guint completed;
  guint failed;
} Fixture;

static void
on_completed (WpAoJournal *journal, gint64 id, gboolean ok,
    const gchar *error, Fixture *f)
{
  g_assert_true (g_main_context_is_owner (f->context));
  g_assert_cmpint (id, ==, ++f->completed);
  if (!ok) {
    g_assert_nonnull (error);
    f->failed++;
  }
}

static void
setup (Fixture *f, gconstpointer unused)
{
  g_autoptr (GError) error = NULL;
  f->directory = g_dir_make_tmp ("ao-journal-test-XXXXXX", &error);
  g_assert_no_error (error);
  f->path = g_build_filename (f->directory, "history.sqlite3", NULL);
  f->context = g_main_context_new ();
  f->core = wp_core_new (f->context, NULL, NULL);
  g_assert_nonnull (f->core);
  f->journal = g_object_new (wp_ao_journal_get_type (),
      "name", "ao-journal-test", "core", f->core, NULL);
  f->journal->path = g_strdup (f->path);
  f->journal->enabled = TRUE;
  g_signal_connect (f->journal, "completed", G_CALLBACK (on_completed), f);
}

static void
teardown (Fixture *f, gconstpointer unused)
{
  g_assert_false (f->journal->running);
  g_assert_true (g_queue_is_empty (&f->journal->jobs));
  g_object_unref (f->journal);
  g_clear_object (&f->core);
  g_main_context_unref (f->context);
  g_autofree gchar *wal = g_strconcat (f->path, "-wal", NULL);
  g_autofree gchar *shm = g_strconcat (f->path, "-shm", NULL);
  g_remove (wal);
  g_remove (shm);
  g_remove (f->path);
  g_rmdir (f->directory);
  g_free (f->directory);
  g_free (f->path);
}

static gint64
submit (Fixture *f, const gchar *event, WpSpaPod *pod)
{
  gint64 id = 0;
  g_assert_true (g_main_context_acquire (f->context));
  g_autofree gchar *key = g_strdup_printf ("session:777:42:55:66:%" G_GINT64_FORMAT,
      f->journal->next_id + 1);
  g_signal_emit_by_name (f->journal, "append", "session", event, pod,
      "test-controller", 1234u, key, &id);
  g_main_context_release (f->context);
  return id;
}

static void
wait_for (Fixture *f, guint count)
{
  gint64 deadline = g_get_monotonic_time () + 5000000;
  while (f->completed < count && g_get_monotonic_time () < deadline)
    g_main_context_iteration (f->context, TRUE);
  g_assert_cmpuint (f->completed, ==, count);
}

static void
ordered_bytes_and_bounds (Fixture *f, gconstpointer unused)
{
  g_autoptr (WpSpaPod) pod = wp_spa_pod_new_string ("owned input");
  const struct spa_pod *raw = wp_spa_pod_get_spa_pod (pod);
  g_autoptr (GBytes) expected = g_bytes_new (raw, SPA_POD_SIZE (raw));
  for (guint i = 1; i <= JOURNAL_QUEUE_LIMIT; i++)
    g_assert_cmpint (submit (f, i == 1 ? "requested" : "completed", pod), ==, i);
  g_assert_cmpint (submit (f, "requested", pod), ==, 0);
  /* The queued record owns its bytes, independently of the caller's POD. */
  g_autoptr (WpSpaPod) changed = wp_spa_pod_new_string ("changed");
  g_assert_true (wp_spa_pod_set_pod (pod, changed));
  wait_for (f, JOURNAL_QUEUE_LIMIT);
  g_assert_cmpuint (f->failed, ==, 0);
  sqlite3 *db;
  sqlite3_stmt *statement;
  g_assert_cmpint (sqlite3_open (f->path, &db), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_prepare_v2 (db,
      "SELECT sequence,event,record,client_name,client_pid FROM operation_events ORDER BY sequence",
      -1, &statement, NULL), ==, SQLITE_OK);
  for (guint i = 1; i <= JOURNAL_QUEUE_LIMIT; i++) {
    g_assert_cmpint (sqlite3_step (statement), ==, SQLITE_ROW);
    g_assert_cmpint (sqlite3_column_int64 (statement, 0), ==, i);
    g_assert_cmpstr ((const gchar *) sqlite3_column_text (statement, 1), ==,
        i == 1 ? "requested" : "completed");
    gsize size;
    const void *bytes = g_bytes_get_data (expected, &size);
    g_assert_cmpmem (sqlite3_column_blob (statement, 2),
        sqlite3_column_bytes (statement, 2), bytes, size);
    g_assert_cmpstr ((const gchar *) sqlite3_column_text (statement, 3), ==, "test-controller");
    g_assert_cmpint (sqlite3_column_int64 (statement, 4), ==, 1234);
  }
  g_assert_cmpint (sqlite3_step (statement), ==, SQLITE_DONE);
  sqlite3_finalize (statement);
  sqlite3_close (db);
  g_assert_cmpint (submit (f, "bad-event", pod), ==, 0);
  f->journal->enabled = FALSE;
  g_assert_cmpint (submit (f, "requested", pod), ==, 0);
}

static void
record_size_limits (Fixture *f, gconstpointer unused)
{
  g_autofree guint8 *bytes = g_malloc0 (REPLY_LIMIT);
  g_autoptr (WpSpaPod) request = wp_spa_pod_new_bytes (bytes,
      REQUEST_LIMIT - sizeof (struct spa_pod));
  g_autoptr (WpSpaPod) request_large = wp_spa_pod_new_bytes (bytes,
      REQUEST_LIMIT - sizeof (struct spa_pod) + 1);
  g_autoptr (WpSpaPod) reply = wp_spa_pod_new_bytes (bytes,
      REPLY_LIMIT - sizeof (struct spa_pod));
  g_autoptr (WpSpaPod) reply_large = wp_spa_pod_new_bytes (bytes,
      REPLY_LIMIT - sizeof (struct spa_pod) + 1);
  g_assert_cmpint (submit (f, "requested", request), ==, 1);
  g_assert_cmpint (submit (f, "requested", request_large), ==, 0);
  g_assert_cmpint (submit (f, "completed", reply), ==, 2);
  g_assert_cmpint (submit (f, "unknown", reply_large), ==, 0);
  wait_for (f, 2);
  g_assert_cmpuint (f->failed, ==, 0);
}

static void
unique_operation_events (Fixture *f, gconstpointer unused)
{
  g_autoptr (WpSpaPod) pod = wp_spa_pod_new_int (42);
  const gchar *events[] = { "requested", "completed", "unknown", "requested" };
  for (guint i = 0; i < G_N_ELEMENTS (events); i++) {
    gint64 id = 0;
    g_assert_true (g_main_context_acquire (f->context));
    g_signal_emit_by_name (f->journal, "append", "session", events[i], pod,
        "test-controller", 1234u, "session:777:42:-1:66:1", &id);
    g_main_context_release (f->context);
    g_assert_cmpint (id, ==, i + 1);
  }
  wait_for (f, 4);
  g_assert_cmpuint (f->failed, ==, 1);
  sqlite3 *db;
  sqlite3_stmt *statement;
  sqlite3_open (f->path, &db);
  sqlite3_prepare_v2 (db, "SELECT count(*) FROM operation_events", -1, &statement, NULL);
  g_assert_cmpint (sqlite3_step (statement), ==, SQLITE_ROW);
  g_assert_cmpint (sqlite3_column_int (statement, 0), ==, 3);
  sqlite3_finalize (statement);
  sqlite3_close (db);
}

static gboolean
heartbeat (gpointer data)
{
  (*(guint *) data)++;
  return G_SOURCE_CONTINUE;
}

static void
busy_worker_keeps_context_responsive (Fixture *f, gconstpointer unused)
{
  g_autoptr (WpSpaPod) pod = wp_spa_pod_new_int (42);
  g_assert_cmpint (submit (f, "requested", pod), ==, 1);
  wait_for (f, 1);
  sqlite3 *db;
  g_assert_cmpint (sqlite3_open (f->path, &db), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_exec (db, "BEGIN IMMEDIATE", NULL, NULL, NULL), ==, SQLITE_OK);
  guint beats = 0;
  GSource *timer = g_timeout_source_new (10);
  g_source_set_callback (timer, heartbeat, &beats, NULL);
  g_source_attach (timer, f->context);
  gint64 start = g_get_monotonic_time ();
  g_assert_cmpint (submit (f, "completed", pod), ==, 2);
  wait_for (f, 2);
  g_assert_cmpint (g_get_monotonic_time () - start, <, 3000000);
  g_assert_cmpuint (f->failed, ==, 1);
  g_assert_cmpuint (beats, >, 20);
  g_source_destroy (timer);
  g_source_unref (timer);
  sqlite3_exec (db, "ROLLBACK", NULL, NULL, NULL);
  sqlite3_close (db);
  g_assert_cmpint (submit (f, "unknown", pod), ==, 3);
  wait_for (f, 3);
  g_assert_cmpuint (f->failed, ==, 1);
}

static void
upgrade_preserves_database_version (Fixture *f, gconstpointer unused)
{
  sqlite3 *db;
  g_assert_true (prepare_path (f->path, NULL));
  g_assert_cmpint (sqlite3_open (f->path, &db), ==, SQLITE_OK);
  g_assert_cmpint (sqlite3_exec (db,
      "PRAGMA user_version=37; CREATE TABLE operation_events ("
      "sequence INTEGER PRIMARY KEY AUTOINCREMENT,session_id TEXT NOT NULL,"
      "event TEXT NOT NULL,record BLOB NOT NULL,recorded_at TEXT NOT NULL DEFAULT 'old')",
      NULL, NULL, NULL), ==, SQLITE_OK);
  sqlite3_close (db);
  g_autoptr (WpSpaPod) pod = wp_spa_pod_new_int (42);
  g_assert_cmpint (submit (f, "requested", pod), ==, 1);
  wait_for (f, 1);
  g_assert_cmpuint (f->failed, ==, 0);
  sqlite3_stmt *statement;
  sqlite3_open (f->path, &db);
  sqlite3_prepare_v2 (db, "PRAGMA user_version", -1, &statement, NULL);
  g_assert_cmpint (sqlite3_step (statement), ==, SQLITE_ROW);
  g_assert_cmpint (sqlite3_column_int (statement, 0), ==, 37);
  sqlite3_finalize (statement);
  sqlite3_close (db);
}

static void
core_destroyed_while_draining (Fixture *f, gconstpointer unused)
{
  g_autoptr (WpSpaPod) pod = wp_spa_pod_new_int (42);
  for (guint i = 1; i <= 3; i++)
    g_assert_cmpint (submit (f, i == 1 ? "requested" : "completed", pod), ==, i);
  wp_ao_journal_disable (WP_PLUGIN (f->journal));
  g_clear_object (&f->core);
  /* GTask retains the exact core context; admitted records continue draining
   * after disable/core destruction without dereferencing the weak core. */
  wait_for (f, 3);
  g_assert_cmpuint (f->failed, ==, 0);
}

static void
insecure_store_rejected (Fixture *f, gconstpointer unused)
{
  g_autoptr (WpSpaPod) pod = wp_spa_pod_new_int (42);
  g_assert_cmpint (g_chmod (f->directory, 0755), ==, 0);
  g_assert_cmpint (submit (f, "requested", pod), ==, 1);
  wait_for (f, 1);
  g_assert_cmpuint (f->failed, ==, 1);
  g_assert_false (g_file_test (f->path, G_FILE_TEST_EXISTS));
}

int main (int argc, char **argv)
{
  g_test_init (&argc, &argv, NULL);
  wp_init (WP_INIT_ALL);
  g_test_add ("/ao-journal/ordered-bytes-and-bounds", Fixture, NULL,
      setup, ordered_bytes_and_bounds, teardown);
  g_test_add ("/ao-journal/record-size-limits", Fixture, NULL,
      setup, record_size_limits, teardown);
  g_test_add ("/ao-journal/unique-operation-events", Fixture, NULL,
      setup, unique_operation_events, teardown);
  g_test_add ("/ao-journal/busy-worker-responsive", Fixture, NULL,
      setup, busy_worker_keeps_context_responsive, teardown);
  g_test_add ("/ao-journal/upgrade-version", Fixture, NULL,
      setup, upgrade_preserves_database_version, teardown);
  g_test_add ("/ao-journal/core-destroy-drains", Fixture, NULL,
      setup, core_destroyed_while_draining, teardown);
  g_test_add ("/ao-journal/private-store", Fixture, NULL,
      setup, insecure_store_rejected, teardown);
  return g_test_run ();
}
