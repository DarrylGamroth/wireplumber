.. _session_item_api:

Session Items
=============
.. graphviz::
  :align: center

   digraph inheritance {
      rankdir=LR;
      GObject -> WpObject;
      WpObject -> WpSessionItem;
   }

.. doxygenstruct:: WpSessionItem

.. doxygenstruct:: _WpSessionItemClass

.. doxygengroup:: wpsessionitem
   :content-only:

Exact port links
----------------

The existing ``si-node`` factory wraps one captured ``WpNode``. Its linkable
port contexts ``input`` and ``output`` retain their directional behavior.
``input:<id>`` and ``output:<id>`` select one resolved global port ID within
that node and direction. The ID must contain only decimal digits in the
``guint32`` range. Missing, foreign, wrong-direction and malformed IDs return
an empty port array; they never fall back to registry order. Exact contexts
also permit an explicitly selected control port, which the ordinary
directional contexts omit. Port-name resolution and owner identity checks
remain the caller's responsibility.

The existing ``si-standard-link`` factory accepts ``link.exact = true`` to
require one selected output tuple and one selected input tuple. It uses the
same child-Link creator as ordinary audio linking, without audio-channel
matching or partial pairing. Exact mode accepts non-adapter linkables with
no acquisition interface and non-lingering child Links. Configuration rejects
``object.linger = true`` before creating a child. ``out.item.port.context`` and
``in.item.port.context`` carry the selected contexts. The child receives only
``link.passive``, ``object.path`` and ``object.linger`` from item configuration;
its four endpoint IDs come from the selected tuples. Exact configuration
rejects attempts to override those endpoint properties. Ordinary audio
pairing remains the default.

While an exact child is retained, :c:func:`wp_session_item_get_associated_proxy`
with ``WP_TYPE_LINK`` returns that child. The caller can observe its actual
endpoints, properties, state and negotiated Format through the existing Link
API. An inactive producer may not have negotiated a Format yet; child
activation alone does not establish a scientific format contract.

:c:func:`wp_si_link_withdraw` retires the exact generation before cancelling
its pending work. Completion waits for all child activation callbacks and
local proxy destruction, including creation that had not reached binding.
Reconfiguration of an existing exact generation is refused without effects
until the caller explicitly resets/deactivates it and retirement has settled.
This also applies to a configured generation whose activation has not started.
Withdrawal permanently retires that item, including a cancelled result; use a fresh item for another
link. Synchronous reset/deactivation also fence exact pending activation but
provide no asynchronous completion receipt. Unsupported link modes report
``G_IO_ERROR_NOT_SUPPORTED``. A caller that requires remote absence must still
synchronize its Core and verify exact correlated registry absence after a
successful withdrawal.
