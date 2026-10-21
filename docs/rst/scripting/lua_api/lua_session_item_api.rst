.. _lua_session_item_api:

Session Item
============

Lua objects that bind a :ref:`WpSessionItem <session_item_api>`
contain the following methods:

.. function:: SessionItem.get_associated_proxy(self, type)

   Binds :c:func:`wp_session_item_get_associated_proxy`

   :param self: the session item
   :param string type: the proxy type name
   :returns: the proxy object or nil

.. function:: SessionItem.reset(self)

   Binds :c:func:`wp_session_item_reset`

   :param self: the session item

.. function:: SessionItem.configure(self, properties)

   Binds :c:func:`wp_session_item_configure`

   :param self: the session item
   :param table properties: The configuration properties
   :returns: true on success, false on failure
   :rtype: boolean

.. function:: SessionItem.register(self)

   Binds :c:func:`wp_session_item_register`

   :param self: the session item

.. function:: SessionItem.remove(self)

   Binds :c:func:`wp_session_item_remove`

   :param self: the session item

.. function:: SessionItem.withdraw(self, callback)

   Binds :c:func:`wp_si_link_withdraw` on items implementing ``WpSiLink``.
   The callback receives ``(item, error)``; ``error`` is nil on success.
   This method has no immediate return value.

   Successful withdrawal proves that child activation callbacks have retired
   and their native proxies have been destroyed. It does not prove remote
   registry absence. Synchronize the manager Core and check the correlated
   registry objects when that proof is required. Cancellation and failure never
   reopen the withdrawn generation.

   ``si-standard-link`` implements this operation for ``link.exact`` mode with
   non-adapter linkables that have no acquisition interface and non-lingering
   children. Exact configuration rejects ``object.linger = true``. The item remains
   retired after withdrawal; create a fresh item for a new link. Other modes
   report an unsupported-operation error rather than treating synchronous
   deactivation as completed withdrawal.

   :param self: the link session item
   :param function callback: asynchronous completion callback
