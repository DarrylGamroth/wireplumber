/* WirePlumber
 *
 * Copyright © 2020 Collabora Ltd.
 *    @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * SPDX-License-Identifier: MIT
 */

#include "../common/base-test-fixture.h"
#include <pipewire/filter.h>
#include <spa/param/ndarray-utils.h>

typedef struct {
  WpBaseTestFixture base;

  WpSessionItem *src_item;
  WpSessionItem *sink_item;

} TestFixture;

static WpSessionItem *
load_node (TestFixture * f, const gchar * factory, const gchar * media_class,
    const gchar * type)
{
  g_autoptr (WpNode) node = NULL;
  g_autoptr (WpSessionItem) adapter = NULL;

  /* create audiotestsrc adapter node */
  node = wp_node_new_from_factory (f->base.core,
      "adapter",
      wp_properties_new (
          "factory.name", factory,
          "node.name", factory,
          "media.class", media_class,
          "audio.channels", "2",
          "audio.position", "[ FL, FR ]",
          NULL));
  g_assert_nonnull (node);
  wp_object_activate (WP_OBJECT (node), WP_OBJECT_FEATURES_ALL,
      NULL, (GAsyncReadyCallback) test_object_activate_finish_cb, f);
  g_main_loop_run (f->base.loop);

  /* create adapter */
  adapter = wp_session_item_make (f->base.core, "si-audio-adapter");
  g_assert_nonnull (adapter);
  g_assert_true (WP_IS_SI_LINKABLE (adapter));

  /* configure */
  {
    WpProperties *props = wp_properties_new_empty ();
    wp_properties_setf (props, "item.node", "%p", node);
    wp_properties_set (props, "media.class", media_class);
    wp_properties_set (props, "item.node.type", type);
    g_assert_true (wp_session_item_configure (adapter, props));
    g_assert_true (wp_session_item_is_configured (adapter));
  }

  /* activate adapter */

  wp_object_activate (WP_OBJECT (adapter),
      WP_SESSION_ITEM_FEATURE_ACTIVE,
      NULL,  (GAsyncReadyCallback) test_object_activate_finish_cb, f);
  g_main_loop_run (f->base.loop);
  g_assert_cmphex (wp_object_get_active_features (WP_OBJECT (adapter)), ==,
      WP_SESSION_ITEM_FEATURE_ACTIVE);

  return g_steal_pointer (&adapter);
}

static void
on_plugin_loaded (WpCore * core, GAsyncResult * res, TestFixture *f)
{
  gboolean loaded;
  GError *error = NULL;

  loaded = wp_core_load_component_finish (core, res, &error);
  g_assert_no_error (error);
  g_assert_true (loaded);

  g_main_loop_quit (f->base.loop);
}

static void
test_si_standard_link_setup (TestFixture * f, gconstpointer user_data)
{
  wp_base_test_fixture_setup (&f->base, 0);

  /* load modules */
  {
    g_autoptr (WpTestServerLocker) lock =
        wp_test_server_locker_new (&f->base.server);

    g_assert_cmpint (pw_context_add_spa_lib (f->base.server.context,
            "audiotestsrc", "audiotestsrc/libspa-audiotestsrc"), ==, 0);
    if (!test_is_spa_lib_installed (&f->base, "audiotestsrc")) {
      g_test_skip ("The pipewire audiotestsrc factory was not found");
      return;
    }
    g_assert_nonnull (pw_context_load_module (f->base.server.context,
            "libpipewire-module-adapter", NULL, NULL));
    g_assert_nonnull (pw_context_load_module (f->base.server.context,
            "libpipewire-module-link-factory", "allow.link.passive=true", NULL));
  }
  {
    wp_core_load_component (f->base.core,
        "libwireplumber-module-si-audio-adapter", "module", NULL, NULL, NULL,
        (GAsyncReadyCallback) on_plugin_loaded, f);
    g_main_loop_run (f->base.loop);

    wp_core_load_component (f->base.core,
        "libwireplumber-module-si-standard-link", "module", NULL, NULL, NULL,
        (GAsyncReadyCallback) on_plugin_loaded, f);
    g_main_loop_run (f->base.loop);
  }

  if (test_is_spa_lib_installed (&f->base, "audiotestsrc"))
    f->src_item = load_node (f, "audiotestsrc", "Stream/Output/Audio", "stream");
  if (test_is_spa_lib_installed (&f->base, "support.null-audio-sink"))
    f->sink_item = load_node (f, "support.null-audio-sink", "Audio/Sink", "device");
}

static void
test_si_standard_link_teardown (TestFixture * f, gconstpointer user_data)
{
  g_clear_object (&f->sink_item);
  g_clear_object (&f->src_item);
  wp_base_test_fixture_teardown (&f->base);
}

static void
test_si_standard_link_main (TestFixture * f, gconstpointer user_data)
{
  g_autoptr (WpSessionItem) link = NULL;

  /* skip the test if audiotestsrc could not be loaded */
  if (!f->src_item) {
    g_test_skip ("The pipewire audiotestsrc factory was not found");
    return;
  }

  /* skip the test if null-audio-sink could not be loaded */
  if (!f->sink_item) {
    g_test_skip ("The pipewire null-audio-sink factory was not found");
    return;
  }

  /* create the link */
  link = wp_session_item_make (f->base.core, "si-standard-link");
  g_assert_nonnull (link);

  /* configure the link */
  {
    g_autoptr (WpProperties) props = wp_properties_new_empty ();
    wp_properties_setf (props, "out.item", "%p", f->src_item);
    wp_properties_setf (props, "in.item", "%p", f->sink_item);
    wp_properties_set (props, "out.item.port.context", "output");
    wp_properties_set (props, "in.item.port.context", "input");
    g_assert_true (wp_session_item_configure (link, g_steal_pointer (&props)));
  }

  /* activate */
  wp_object_activate (WP_OBJECT (link), WP_SESSION_ITEM_FEATURE_ACTIVE,
      NULL, (GAsyncReadyCallback) test_object_activate_finish_cb, f);
  g_main_loop_run (f->base.loop);

  /* verify the graph state */
  {
    g_autoptr (WpNode) out_node = NULL;
    g_autoptr (WpNode) in_node = NULL;
    g_autoptr (WpIterator) it = NULL;
    g_auto (GValue) val = G_VALUE_INIT;
    g_autoptr (WpObjectManager) om = wp_object_manager_new ();
    guint total_links = 0;

    wp_object_manager_add_interest (om, WP_TYPE_NODE, NULL);
    wp_object_manager_add_interest (om, WP_TYPE_PORT, NULL);
    wp_object_manager_add_interest (om, WP_TYPE_LINK, NULL);
    wp_object_manager_request_object_features (om, WP_TYPE_PROXY,
        WP_PIPEWIRE_OBJECT_FEATURES_MINIMAL);
    test_ensure_object_manager_is_installed (om, f->base.core,
        f->base.loop);

    out_node = wp_object_manager_lookup (om, WP_TYPE_NODE,
        WP_CONSTRAINT_TYPE_PW_PROPERTY, "node.name", "=s", "audiotestsrc",
        NULL);
    g_assert_nonnull (out_node);
    in_node = wp_object_manager_lookup (om, WP_TYPE_NODE,
        WP_CONSTRAINT_TYPE_PW_PROPERTY, "node.name", "=s", "support.null-audio-sink",
        NULL);
    g_assert_nonnull (in_node);
    g_assert_cmpuint (wp_object_manager_get_n_objects (om), ==, 8);

    it = wp_object_manager_new_filtered_iterator (om, WP_TYPE_LINK, NULL);
    for (; wp_iterator_next (it, &val); g_value_unset (&val)) {
      guint32 out_nd_id, out_pt_id, in_nd_id, in_pt_id;
      g_autoptr (WpPort) out_port = NULL;
      g_autoptr (WpPort) in_port = NULL;
      WpLink *link = g_value_get_object (&val);
      wp_link_get_linked_object_ids (link, &out_nd_id, &out_pt_id, &in_nd_id,
          &in_pt_id);
      g_assert_cmpuint (out_nd_id, ==, wp_proxy_get_bound_id (WP_PROXY (out_node)));
      g_assert_cmpuint (in_nd_id, ==, wp_proxy_get_bound_id (WP_PROXY (in_node)));
      out_port = wp_object_manager_lookup (om, WP_TYPE_PORT,
          WP_CONSTRAINT_TYPE_G_PROPERTY, "bound-id", "=u", out_pt_id,
          WP_CONSTRAINT_TYPE_PW_PROPERTY, "node.id", "=u", out_nd_id,
          NULL);
      g_assert_nonnull (out_port);
      in_port = wp_object_manager_lookup (om, WP_TYPE_PORT,
          WP_CONSTRAINT_TYPE_G_PROPERTY, "bound-id", "=u", in_pt_id,
          WP_CONSTRAINT_TYPE_PW_PROPERTY, "node.id", "=u", in_nd_id,
          NULL);
      g_assert_nonnull (in_port);
      total_links++;
    }
    g_assert_cmpuint (total_links, ==, 2);
  }

  /* deactivate */
  wp_object_deactivate (WP_OBJECT (link), WP_SESSION_ITEM_FEATURE_ACTIVE);

  /* the links are destroyed on the server; wait for the registry to catch up */
  wp_core_sync (f->base.core, NULL, (GAsyncReadyCallback) test_core_done_cb,
      &f->base);
  g_main_loop_run (f->base.loop);

  /* verify the graph state */
  {
    g_autoptr (WpNode) out_node = NULL;
    g_autoptr (WpNode) in_node = NULL;
    g_autoptr (WpPort) out_port = NULL;
    g_autoptr (WpPort) in_port = NULL;
    g_autoptr (WpLink) link = NULL;
    g_autoptr (WpObjectManager) om = wp_object_manager_new ();

    wp_object_manager_add_interest (om, WP_TYPE_NODE, NULL);
    wp_object_manager_add_interest (om, WP_TYPE_PORT, NULL);
    wp_object_manager_add_interest (om, WP_TYPE_LINK, NULL);
    wp_object_manager_request_object_features (om, WP_TYPE_PROXY,
        WP_PIPEWIRE_OBJECT_FEATURES_MINIMAL);
    test_ensure_object_manager_is_installed (om, f->base.core,
        f->base.loop);

    out_node = wp_object_manager_lookup (om, WP_TYPE_NODE,
        WP_CONSTRAINT_TYPE_PW_PROPERTY, "node.name", "=s", "audiotestsrc",
        NULL);
    g_assert_nonnull (out_node);
    in_node = wp_object_manager_lookup (om, WP_TYPE_NODE,
        WP_CONSTRAINT_TYPE_PW_PROPERTY, "node.name", "=s", "support.null-audio-sink",
        NULL);
    g_assert_nonnull (in_node);
    out_port = wp_object_manager_lookup (om, WP_TYPE_PORT,
        WP_CONSTRAINT_TYPE_PW_PROPERTY, "port.direction", "=s", "out",
        NULL);
    g_assert_nonnull (out_port);
    in_port = wp_object_manager_lookup (om, WP_TYPE_PORT,
        WP_CONSTRAINT_TYPE_PW_PROPERTY, "port.direction", "=s", "in",
        NULL);
    g_assert_nonnull (in_port);
    link = wp_object_manager_lookup (om, WP_TYPE_LINK, NULL);
    g_assert_null (link);
    g_assert_cmpuint (wp_object_manager_get_n_objects (om), ==, 6);
  }
}

/* Real inactive NDArray filters provide multiple non-audio ports in opposite
 * declaration orders. No processing callback, data thread or GPU is required. */
static struct pw_filter *
make_ndarray_filter (TestFixture * f, const gchar * name, enum pw_direction direction)
{
  struct pw_filter *filter = pw_filter_new (wp_core_get_pw_core (f->base.core),
      name, pw_properties_new (PW_KEY_NODE_NAME, name, PW_KEY_MEDIA_CLASS,
          direction == PW_DIRECTION_OUTPUT ? "Stream/Output/NDArray" : "Stream/Input/NDArray", NULL));
  guint8 buffer[1024];
  struct spa_pod_builder builder = SPA_POD_BUILDER_INIT (buffer, sizeof (buffer));
  const guint32 shape[] = { 4 };
  const struct spa_pod *format = spa_pod_builder_add_object (&builder,
      SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
      SPA_FORMAT_mediaType, SPA_POD_Id (SPA_MEDIA_TYPE_application),
      SPA_FORMAT_mediaSubtype, SPA_POD_Id (SPA_MEDIA_SUBTYPE_ndarray),
      SPA_FORMAT_NDARRAY_elementType, SPA_POD_Id (SPA_ELEMENT_TYPE_F32_LE),
      SPA_FORMAT_NDARRAY_shape, SPA_POD_Array (sizeof (guint32), SPA_TYPE_Int, 1, shape),
      SPA_FORMAT_NDARRAY_layout, SPA_POD_Id (SPA_NDARRAY_LAYOUT_ROW_MAJOR),
      SPA_FORMAT_NDARRAY_rate, SPA_POD_Fraction (&SPA_FRACTION (10, 1)),
      SPA_FORMAT_NDARRAY_schema, SPA_POD_String ("test.exact/1"));
  g_assert_nonnull (filter);
  for (guint i = 0; i < 2; i++) {
    guint index = direction == PW_DIRECTION_OUTPUT ? 1 - i : i;
    g_autofree gchar *port_name = g_strdup_printf ("science-%u", index);
    g_assert_nonnull (pw_filter_add_port (filter, direction, PW_FILTER_PORT_FLAG_NONE,
        1, pw_properties_new (PW_KEY_PORT_NAME, port_name, PW_KEY_PORT_CONTROL,
            direction == PW_DIRECTION_OUTPUT && index == 1 ? "true" : "false", NULL), &format, 1));
  }
  g_assert_cmpint (pw_filter_connect (filter, PW_FILTER_FLAG_INACTIVE, NULL, 0), ==, 0);
  return filter;
}

struct exact_fixture {
  struct pw_filter *source;
  struct pw_filter *sink;
  WpObjectManager *manager;
  WpSessionItem *out;
  WpSessionItem *in;
  guint32 output_id;
  guint32 input_id;
};

static WpSessionItem *
wrap_node (TestFixture * f, WpNode * node)
{
  WpSessionItem *item = wp_session_item_make (f->base.core, "si-node");
  WpProperties *properties = wp_properties_new_empty ();
  wp_properties_setf (properties, "item.node", "%p", node);
  g_assert_true (wp_session_item_configure (item, properties));
  wp_object_activate (WP_OBJECT (item), WP_SESSION_ITEM_FEATURE_ACTIVE, NULL,
      (GAsyncReadyCallback) test_object_activate_finish_cb, f);
  g_main_loop_run (f->base.loop);
  return item;
}

static guint32
named_port_id (WpNode * node, const gchar * name)
{
  g_autoptr (WpIterator) iterator = wp_node_new_ports_iterator (node);
  g_auto (GValue) value = G_VALUE_INIT;
  while (wp_iterator_next (iterator, &value)) {
    WpPort *port = g_value_get_object (&value);
    g_autoptr (WpProperties) props = wp_pipewire_object_get_properties (WP_PIPEWIRE_OBJECT (port));
    if (!g_strcmp0 (wp_properties_get (props, PW_KEY_PORT_NAME), name))
      return wp_proxy_get_bound_id (WP_PROXY (port));
    g_value_unset (&value);
  }
  g_assert_not_reached ();
}

static void
exact_fixture_init (TestFixture * f, struct exact_fixture * exact)
{
  wp_core_load_component (f->base.core, "libwireplumber-module-si-node", "module",
      NULL, NULL, NULL, (GAsyncReadyCallback) on_plugin_loaded, f);
  g_main_loop_run (f->base.loop);
  exact->source = make_ndarray_filter (f, "exact-source", PW_DIRECTION_OUTPUT);
  exact->sink = make_ndarray_filter (f, "exact-sink", PW_DIRECTION_INPUT);
  exact->manager = wp_object_manager_new ();
  wp_object_manager_add_interest (exact->manager, WP_TYPE_NODE, NULL);
  wp_object_manager_add_interest (exact->manager, WP_TYPE_LINK, NULL);
  wp_object_manager_request_object_features (exact->manager, WP_TYPE_NODE,
      WP_PIPEWIRE_OBJECT_FEATURES_MINIMAL | WP_NODE_FEATURE_PORTS);
  wp_object_manager_request_object_features (exact->manager, WP_TYPE_LINK,
      WP_PIPEWIRE_OBJECT_FEATURES_MINIMAL);
  test_ensure_object_manager_is_installed (exact->manager, f->base.core, f->base.loop);
  g_autoptr (WpNode) source = NULL;
  g_autoptr (WpNode) sink = NULL;
  do {
    g_clear_object (&source);
    g_clear_object (&sink);
    source = wp_object_manager_lookup (exact->manager, WP_TYPE_NODE,
        WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_NODE_NAME, "=s", "exact-source", NULL);
    sink = wp_object_manager_lookup (exact->manager, WP_TYPE_NODE,
        WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_NODE_NAME, "=s", "exact-sink", NULL);
    if (!source || !sink)
      g_main_context_iteration (f->base.context, TRUE);
  } while (!source || !sink);
  exact->out = wrap_node (f, source);
  exact->in = wrap_node (f, sink);
  exact->output_id = named_port_id (source, "science-0");
  guint32 control_id = named_port_id (source, "science-1");
  g_autofree gchar *control_context = g_strdup_printf ("output:%u", control_id);
  g_autoptr (GVariant) control_ports = wp_si_linkable_get_ports (WP_SI_LINKABLE (exact->out), control_context);
  g_assert_cmpuint (g_variant_n_children (control_ports), ==, 1);
  g_autoptr (GVariant) standard_ports = wp_si_linkable_get_ports (WP_SI_LINKABLE (exact->out), "output");
  g_assert_cmpuint (g_variant_n_children (standard_ports), ==, 1);
  exact->input_id = named_port_id (sink, "science-1");
}

static void
exact_fixture_clear (struct exact_fixture * exact)
{
  g_clear_object (&exact->out);
  g_clear_object (&exact->in);
  g_clear_object (&exact->manager);
  pw_filter_destroy (exact->source);
  pw_filter_destroy (exact->sink);
}

static WpProperties *
exact_properties (struct exact_fixture * exact)
{
  WpProperties *props = wp_properties_new ("link.exact", "true",
      PW_KEY_LINK_PASSIVE, "true", PW_KEY_OBJECT_LINGER, "false",
      PW_KEY_OBJECT_PATH, "test/exact-link", "unsupported.forwarding", "must-not-leak", NULL);
  wp_properties_setf (props, "out.item", "%p", exact->out);
  wp_properties_setf (props, "in.item", "%p", exact->in);
  wp_properties_setf (props, "out.item.port.context", "output:%u", exact->output_id);
  wp_properties_setf (props, "in.item.port.context", "input:%u", exact->input_id);
  return props;
}

struct operation_result {
  guint calls;
  gboolean success;
  GError *error;
};

static void
activation_finished (WpObject * object, GAsyncResult * res, struct operation_result * result)
{
  result->calls++;
  result->success = wp_object_activate_finish (object, res, &result->error);
}

static void
withdrawal_finished (WpSiLink * item, GAsyncResult * res, struct operation_result * result)
{
  result->calls++;
  result->success = wp_si_link_withdraw_finish (item, res, &result->error);
}

static void
wait_operation (TestFixture * f, struct operation_result * result)
{
  while (!result->calls)
    g_main_context_iteration (f->base.context, TRUE);
  g_assert_cmpuint (result->calls, ==, 1);
}

static void
assert_link_absent (TestFixture * f, struct exact_fixture * exact)
{
  wp_core_sync (f->base.core, NULL, (GAsyncReadyCallback) test_core_done_cb, &f->base);
  g_main_loop_run (f->base.loop);
  g_autoptr (WpLink) link = wp_object_manager_lookup (exact->manager, WP_TYPE_LINK,
      WP_CONSTRAINT_TYPE_PW_PROPERTY, PW_KEY_OBJECT_PATH, "=s", "test/exact-link", NULL);
  g_assert_null (link);
}

static void
test_si_standard_link_exact (TestFixture * f, gconstpointer data)
{
  struct exact_fixture exact = { 0 };
  exact_fixture_init (f, &exact);
  g_autoptr (WpSessionItem) item = wp_session_item_make (f->base.core, "si-standard-link");
  g_assert_true (wp_session_item_configure (item, exact_properties (&exact)));
  struct operation_result activation = { 0 }, withdrawal = { 0 };
  wp_object_activate (WP_OBJECT (item), WP_SESSION_ITEM_FEATURE_ACTIVE, NULL,
      (GAsyncReadyCallback) activation_finished, &activation);
  wait_operation (f, &activation);
  g_assert_no_error (activation.error);
  g_assert_true (activation.success);
  g_autoptr (WpLink) child = wp_session_item_get_associated_proxy (item, WP_TYPE_LINK);
  g_assert_nonnull (child);
  guint32 out, in;
  wp_link_get_linked_object_ids (child, NULL, &out, NULL, &in);
  g_assert_cmpuint (out, ==, exact.output_id);
  g_assert_cmpuint (in, ==, exact.input_id);
  g_autoptr (WpProperties) props = wp_pipewire_object_get_properties (WP_PIPEWIRE_OBJECT (child));
  g_assert_cmpstr (wp_properties_get (props, PW_KEY_LINK_PASSIVE), ==, "true");
  g_assert_cmpstr (wp_properties_get (props, PW_KEY_OBJECT_PATH), ==, "test/exact-link");
  g_assert_cmpstr (wp_properties_get (props, PW_KEY_OBJECT_LINGER), ==, "false");
  g_assert_null (wp_properties_get (props, "unsupported.forwarding"));
  /* The selected NDArray tuples carry no synthetic audio-channel identity.
   * Inactive filters intentionally do not negotiate streaming buffers here. */
  g_autoptr (GVariant) ports = wp_si_linkable_get_ports (WP_SI_LINKABLE (exact.in), "input");
  for (guint i = 0; i < g_variant_n_children (ports); i++) {
    guint32 channel;
    g_variant_get_child (ports, i, "(uuu)", NULL, NULL, &channel);
    g_assert_cmpuint (channel, ==, 0);
  }
  wp_si_link_withdraw (WP_SI_LINK (item), NULL,
      (GAsyncReadyCallback) withdrawal_finished, &withdrawal);
  wait_operation (f, &withdrawal);
  g_assert_no_error (withdrawal.error);
  g_assert_true (withdrawal.success);
  g_assert_null (wp_proxy_get_pw_proxy (WP_PROXY (child)));
  g_assert_null (wp_session_item_get_associated_proxy (item, WP_TYPE_LINK));
  g_assert_false (wp_object_test_active_features (WP_OBJECT (item), WP_SESSION_ITEM_FEATURE_ACTIVE));
  assert_link_absent (f, &exact);
  g_assert_false (wp_session_item_configure (item, exact_properties (&exact)));
  exact_fixture_clear (&exact);
}

static void
test_si_standard_link_exact_refusal (TestFixture * f, gconstpointer data)
{
  struct exact_fixture exact = { 0 };
  exact_fixture_init (f, &exact);
  const gchar *contexts[][2] = { { "output", "input" }, { "output:4294967295", "input" } };
  for (guint i = 0; i < G_N_ELEMENTS (contexts); i++) {
    g_autoptr (WpSessionItem) item = wp_session_item_make (f->base.core, "si-standard-link");
    WpProperties *props = exact_properties (&exact);
    wp_properties_set (props, "out.item.port.context", contexts[i][0]);
    wp_properties_set (props, "in.item.port.context", contexts[i][1]);
    g_assert_true (wp_session_item_configure (item, props));
    struct operation_result activation = { 0 }, withdrawal = { 0 };
    wp_object_activate (WP_OBJECT (item), WP_SESSION_ITEM_FEATURE_ACTIVE, NULL,
        (GAsyncReadyCallback) activation_finished, &activation);
    wait_operation (f, &activation);
    g_assert_false (activation.success);
    g_clear_error (&activation.error);
    wp_si_link_withdraw (WP_SI_LINK (item), NULL,
        (GAsyncReadyCallback) withdrawal_finished, &withdrawal);
    wait_operation (f, &withdrawal);
    g_assert_true (withdrawal.success);
    g_assert_null (wp_session_item_get_associated_proxy (item, WP_TYPE_LINK));
  }
  const gchar *ids[] = { PW_KEY_LINK_OUTPUT_NODE, PW_KEY_LINK_OUTPUT_PORT,
    PW_KEY_LINK_INPUT_NODE, PW_KEY_LINK_INPUT_PORT };
  for (guint i = 0; i < G_N_ELEMENTS (ids); i++) {
    g_autoptr (WpSessionItem) item = wp_session_item_make (f->base.core, "si-standard-link");
    WpProperties *props = exact_properties (&exact);
    wp_properties_set (props, ids[i], "1");
    g_assert_false (wp_session_item_configure (item, props));
  }
  const gchar *lingering[] = { "true", "1" };
  for (guint i = 0; i < G_N_ELEMENTS (lingering); i++) {
    g_autoptr (WpSessionItem) item = wp_session_item_make (f->base.core, "si-standard-link");
    WpProperties *props = exact_properties (&exact);
    wp_properties_set (props, PW_KEY_OBJECT_LINGER, lingering[i]);
    g_assert_false (wp_session_item_configure (item, props));
    g_assert_null (wp_session_item_get_associated_proxy (item, WP_TYPE_LINK));
  }
  {
    g_autoptr (WpSessionItem) item = wp_session_item_make (f->base.core, "si-standard-link");
    WpProperties *props = exact_properties (&exact);
    wp_properties_setf (props, "out.item", "%p", f->src_item);
    g_assert_false (wp_session_item_configure (item, props));
  }
  g_autoptr (GVariant) wrong_node = NULL;
  g_autofree gchar *foreign = g_strdup_printf ("output:%u", exact.input_id);
  wrong_node = wp_si_linkable_get_ports (WP_SI_LINKABLE (exact.out), foreign);
  g_assert_cmpuint (g_variant_n_children (wrong_node), ==, 0);
  assert_link_absent (f, &exact);
  exact_fixture_clear (&exact);
}

struct withdraw_trigger {
  WpSessionItem *item;
  GCancellable *cancellable;
  struct operation_result *withdrawal;
  struct operation_result *rejected;
  guint calls;
};

static void
withdraw_on_created (WpProxy * proxy, gpointer native, struct withdraw_trigger * trigger)
{
  trigger->calls++;
  g_assert_nonnull (wp_proxy_get_pw_proxy (proxy));
  g_assert_false (wp_object_test_active_features (WP_OBJECT (proxy), WP_PROXY_FEATURE_BOUND));
  wp_si_link_withdraw (WP_SI_LINK (trigger->item), trigger->cancellable,
      (GAsyncReadyCallback) withdrawal_finished, trigger->withdrawal);
  wp_object_activate (WP_OBJECT (trigger->item), WP_SESSION_ITEM_FEATURE_ACTIVE, NULL,
      (GAsyncReadyCallback) activation_finished, trigger->rejected);
}

static void
withdraw_on_bound (WpProxy * proxy, guint id, struct withdraw_trigger * trigger)
{
  trigger->calls++;
  g_assert_true (wp_object_test_active_features (WP_OBJECT (proxy), WP_PROXY_FEATURE_BOUND));
  g_assert_false (wp_object_test_active_features (WP_OBJECT (proxy), WP_PIPEWIRE_OBJECT_FEATURE_INFO));
  wp_si_link_withdraw (WP_SI_LINK (trigger->item), trigger->cancellable,
      (GAsyncReadyCallback) withdrawal_finished, trigger->withdrawal);
  wp_object_activate (WP_OBJECT (trigger->item), WP_SESSION_ITEM_FEATURE_ACTIVE, NULL,
      (GAsyncReadyCallback) activation_finished, trigger->rejected);
}

static void
test_si_standard_link_exact_race (TestFixture * f, gconstpointer data)
{
  const gchar *stage = data;
  struct exact_fixture exact = { 0 };
  exact_fixture_init (f, &exact);
  g_autoptr (WpSessionItem) item = wp_session_item_make (f->base.core, "si-standard-link");
  g_assert_true (wp_session_item_configure (item, exact_properties (&exact)));
  struct operation_result activation = { 0 }, withdrawal = { 0 }, rejected = { 0 };
  g_autoptr (GCancellable) cancellable = g_cancellable_new ();
  wp_object_activate (WP_OBJECT (item), WP_SESSION_ITEM_FEATURE_ACTIVE, NULL,
      (GAsyncReadyCallback) activation_finished, &activation);
  g_autoptr (WpLink) child = NULL;
  if (g_strcmp0 (stage, "queued")) {
    while (!(child = wp_session_item_get_associated_proxy (item, WP_TYPE_LINK)))
      g_main_context_iteration (f->base.context, TRUE);
    g_assert_null (wp_proxy_get_pw_proxy (WP_PROXY (child)));
  }
  struct withdraw_trigger trigger = { item, cancellable, &withdrawal, &rejected, 0 };
  if (!g_strcmp0 (stage, "created")) {
    g_signal_connect (child, "pw-proxy-created", G_CALLBACK (withdraw_on_created), &trigger);
  } else if (!g_strcmp0 (stage, "bound")) {
    g_signal_connect (child, "bound", G_CALLBACK (withdraw_on_bound), &trigger);
  } else {
    if (!g_strcmp0 (stage, "child-error"))
      wp_object_abort_activation (WP_OBJECT (child), "Injected native child failure");
    if (!g_strcmp0 (stage, "reset"))
      wp_session_item_reset (item);
    else if (!g_strcmp0 (stage, "deactivate"))
      wp_object_deactivate (WP_OBJECT (item), WP_SESSION_ITEM_FEATURE_ACTIVE);
    if (!g_strcmp0 (stage, "cancelled"))
      g_cancellable_cancel (cancellable);
    wp_si_link_withdraw (WP_SI_LINK (item), cancellable,
        (GAsyncReadyCallback) withdrawal_finished, &withdrawal);
    wp_object_activate (WP_OBJECT (item), WP_SESSION_ITEM_FEATURE_ACTIVE, NULL,
        (GAsyncReadyCallback) activation_finished, &rejected);
  }
  g_assert_cmpuint (withdrawal.calls, ==, 0);
  /* Configuration cannot replace storage while retirement callbacks are pending. */
  if (g_strcmp0 (stage, "created") && g_strcmp0 (stage, "bound"))
    g_assert_false (wp_session_item_configure (item, exact_properties (&exact)));
  wait_operation (f, &withdrawal);
  wait_operation (f, &activation);
  if (!g_strcmp0 (stage, "created") || !g_strcmp0 (stage, "bound"))
    g_assert_cmpuint (trigger.calls, ==, 1);
  g_assert_false (activation.success);
  g_clear_error (&activation.error);
  if (!g_strcmp0 (stage, "cancelled")) {
    g_assert_false (withdrawal.success);
    g_assert_error (withdrawal.error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error (&withdrawal.error);
  } else {
    g_assert_no_error (withdrawal.error);
    g_assert_true (withdrawal.success);
  }
  g_assert_cmpuint (activation.calls, ==, 1);
  if (child)
    g_assert_null (wp_proxy_get_pw_proxy (WP_PROXY (child)));
  wait_operation (f, &rejected);
  g_assert_false (rejected.success);
  g_clear_error (&rejected.error);
  assert_link_absent (f, &exact);
  exact_fixture_clear (&exact);
}

static gboolean
mark_idle (gpointer data)
{
  *((gboolean *) data) = TRUE;
  return G_SOURCE_REMOVE;
}

static void
test_si_standard_link_reconfigure (TestFixture * f, gconstpointer data)
{
  struct exact_fixture exact = { 0 };
  exact_fixture_init (f, &exact);
  for (guint active = 0; active < 2; active++) {
    g_autoptr (WpSessionItem) item = wp_session_item_make (f->base.core, "si-standard-link");
    g_assert_true (wp_session_item_configure (item, exact_properties (&exact)));
    struct operation_result activation = { 0 }, withdrawal = { 0 };
    if (active) {
      wp_object_activate (WP_OBJECT (item), WP_SESSION_ITEM_FEATURE_ACTIVE, NULL,
          (GAsyncReadyCallback) activation_finished, &activation);
      wait_operation (f, &activation);
      g_assert_true (activation.success);
      g_assert_no_error (activation.error);
    }
    g_autoptr (WpLink) before = wp_session_item_get_associated_proxy (item, WP_TYPE_LINK);
    g_assert_false (wp_session_item_configure (item, exact_properties (&exact)));
    g_assert_true (wp_session_item_is_configured (item));
    g_autoptr (WpLink) after = wp_session_item_get_associated_proxy (item, WP_TYPE_LINK);
    g_assert_true (before == after);
    g_assert_cmpint (wp_object_test_active_features (WP_OBJECT (item),
        WP_SESSION_ITEM_FEATURE_ACTIVE), ==, active);
    wp_session_item_reset (item);
    gboolean idle = FALSE;
    wp_core_idle_add (f->base.core, NULL, mark_idle, &idle, NULL);
    while (!idle)
      g_main_context_iteration (f->base.context, TRUE);
    g_assert_true (wp_session_item_configure (item, exact_properties (&exact)));
    wp_si_link_withdraw (WP_SI_LINK (item), NULL,
        (GAsyncReadyCallback) withdrawal_finished, &withdrawal);
    wait_operation (f, &withdrawal);
    g_assert_true (withdrawal.success);
    g_assert_no_error (withdrawal.error);
  }
  assert_link_absent (f, &exact);
  exact_fixture_clear (&exact);
}

static void
test_si_standard_link_unsupported (TestFixture * f, gconstpointer data)
{
  g_autoptr (WpSessionItem) item = wp_session_item_make (f->base.core, "si-standard-link");
  WpProperties *props = wp_properties_new ("out.item.port.context", "output",
      "in.item.port.context", "input", NULL);
  wp_properties_setf (props, "out.item", "%p", f->src_item);
  wp_properties_setf (props, "in.item", "%p", f->sink_item);
  g_assert_true (wp_session_item_configure (item, props));
  struct operation_result withdrawal = { 0 };
  wp_si_link_withdraw (WP_SI_LINK (item), NULL,
      (GAsyncReadyCallback) withdrawal_finished, &withdrawal);
  wait_operation (f, &withdrawal);
  g_assert_false (withdrawal.success);
  g_assert_error (withdrawal.error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
  g_clear_error (&withdrawal.error);
}

gint
main (gint argc, gchar *argv[])
{
  g_test_init (&argc, &argv, NULL);
  wp_init (WP_INIT_ALL);

  g_test_add ("/modules/si-standard-link/main",
      TestFixture, NULL,
      test_si_standard_link_setup,
      test_si_standard_link_main,
      test_si_standard_link_teardown);

  g_test_add ("/modules/si-standard-link/exact", TestFixture, NULL,
      test_si_standard_link_setup, test_si_standard_link_exact, test_si_standard_link_teardown);
  g_test_add ("/modules/si-standard-link/exact-refusal", TestFixture, NULL,
      test_si_standard_link_setup, test_si_standard_link_exact_refusal, test_si_standard_link_teardown);
  g_test_add ("/modules/si-standard-link/unsupported-withdrawal", TestFixture, NULL,
      test_si_standard_link_setup, test_si_standard_link_unsupported, test_si_standard_link_teardown);
  g_test_add ("/modules/si-standard-link/reconfigure", TestFixture, NULL,
      test_si_standard_link_setup, test_si_standard_link_reconfigure, test_si_standard_link_teardown);
  const gchar *stages[] = { "queued", "unbound", "created", "bound", "reset", "deactivate", "cancelled", "child-error" };
  for (guint i = 0; i < G_N_ELEMENTS (stages); i++) {
    g_autofree gchar *path = g_strdup_printf ("/modules/si-standard-link/race/%s", stages[i]);
    g_test_add (path, TestFixture, stages[i], test_si_standard_link_setup,
        test_si_standard_link_exact_race, test_si_standard_link_teardown);
  }
  return g_test_run ();
}
