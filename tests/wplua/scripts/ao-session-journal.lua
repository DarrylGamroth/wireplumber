-- Real v1 PODs with deterministic fake transport, timers and async storage.
local control = require ("ao-control")
local ingress = require ("ao-session-control")
local now, timers = 1000000, {}
local test_core = { get_monotonic_time = function () return now end }
test_core.timeout_add = function (_, callback)
  local timer = { callback = callback, destroy = function (self) self.destroyed = true end }
  timers [#timers + 1] = timer
  return timer
end
local manager_new = function ()
  return { callbacks = {}, connect = function (self, signal, callback)
    self.callbacks [signal] = callback
  end, activate = function () end }
end
-- Required modules use separate sandbox environments, as in ao-connections.
for index = 1, 16 do
  local name, environment = debug.getupvalue (ingress.new, index)
  if name == "_ENV" then
    debug.setupvalue (ingress.new, index, setmetatable ({ Core = test_core,
      ObjectManager = manager_new }, { __index = environment }))
    break
  end
end
local function fake ()
  return { callbacks = {}, records = {}, connect = function (self, signal, callback)
    self.callbacks [signal] = callback
  end, call = function (self, action, ...)
    if action == "get-owner-pid" then return 99 end
    if action == "publish-records" then
      self.records [#self.records + 1] = { ... }; return true
    end
    error (action)
  end }
end
local function setup (journal_enabled)
  local endpoint, journal = fake (), fake ()
  journal.events, journal.next_id = {}, 0
  journal.call = function (self, action, session, event, pod, name, pid, key)
    assert (action == "append" and session == "test-session")
    assert (name == "test-controller" and pid == 1234)
    local header = control.decode (pod, event == "requested" and "request" or "completion")
    assert (key == table.concat ({ session, tostring (header.endpoint),
      tostring (header.global_id), tostring (header.serial), tostring (header.instance),
      tostring (header.token) }, ":"), "Journal key lost exact native identity")
    if self.reject then return 0 end
    self.next_id = self.next_id + 1
    self.events [#self.events + 1] = { id = self.next_id, event = event, pod = pod:copy () }
    return self.next_id
  end
  function journal:finish (id, ok)
    self.callbacks.completed (self, id, ok ~= false, ok == false and "disk unavailable" or "")
  end
  local effects, faults = 0, 0
  local api = ingress.new (endpoint, 777, function () effects = effects + 1 end,
      function () faults = faults + 1 end, journal_enabled and journal or nil, "test-session")
  local node = { ["bound-id"] = 42, properties = {
    ["object.serial"] = "55", ["node.name"] = "test-controller",
    ["pipewireao.rtc-control.instance"] = "66",
    ["pipewireao.rtc-control.owner-pid"] = "1234",
    ["pipewireao.rtc-control.protocol"] = "pipewireao.rtc-control/1",
    ["pipewireao.rtc-control.profile"] = "pipewireao.rtc.controller/1",
  }, get_n_input_ports = function () return 0 end,
  get_n_output_ports = function () return 0 end, connect = function () end }
  api.manager.callbacks ["object-added"] (api.manager, node)
  local function request (token, operation, payload)
    local pod = control.encode ("request", { endpoint = 777, global_id = 42,
      serial = 55, instance = 66, token = token or 1, operation = operation or 13,
      budget_ns = 1000000000 }, payload or Pod.Struct { "graph", Pod.Struct {} })
    endpoint.callbacks.parameter (endpoint, pod, now)
    return pod
  end
  return api, journal, request, function () return effects, faults end, node
end

local api, journal, request, counts = setup (true)
local raw = request ()
assert (counts () == 0 and #journal.events == 1 and journal.events [1].event == "requested")
assert (journal.events [1].pod:equals (raw), "Request bytes/types changed before journaling")
request () -- pending duplicate must neither dispatch nor append
assert (#journal.events == 1 and counts () == 0)
journal:finish (1)
assert (counts () == 1)
local ticket = api.pending
local old_completion = api.completion
local details = Pod.Struct { "graph", Pod.Struct {} }
ingress.complete (api, ticket, 5, details)
assert (#journal.events == 2 and journal.events [2].event == "completed")
assert (api.completion == old_completion and api.pending == ticket,
    "Completion was published before durable storage acknowledged")
ingress.complete (api, ticket, 5, details) -- duplicate callback
request () -- duplicate request while terminal storage is pending
assert (#journal.events == 2 and counts () == 1)
journal:finish (2)
assert (not api.pending and api.terminal == ticket)
assert (api.completion:equals (journal.events [2].pod))
request () -- terminal duplicate remains idempotent
assert (#journal.events == 2 and counts () == 1)

-- Rejected admissions and failed pre-effect writes never reach science/fault.
for _, admission in ipairs ({ true, false }) do
  api, journal, request, counts = setup (true)
  journal.reject = admission
  request ()
  if not admission then journal:finish (1, false) end
  local effects, faults = counts ()
  assert (effects == 0 and faults == 0 and not api.pending)
  local header = control.decode (api.completion, "completion")
  assert (header.result < 0)
end

-- An expired or disappeared controller cannot be resurrected by late storage.
for _, removed in ipairs ({ true, false }) do
  local node
  api, journal, request, counts, node = setup (true)
  request ()
  if removed then api.manager.callbacks ["object-removed"] (api.manager, node)
  else now = now + 1000001; api.pending.expiry.callback () end
  assert (not api.pending and counts () == 0)
  journal:finish (1)
  assert (counts () == 0 and #journal.events == 2)
  journal:finish (2)
end

-- Even a failed original append after cancellation cannot dispatch. A truthful
-- no-effect terminal record may survive by itself if storage becomes available.
api, journal, request, counts = setup (true)
request ()
now = now + 1000001; api.pending.expiry.callback ()
journal:finish (1, false); journal:finish (2)
local no_effects, no_faults = counts ()
assert (no_effects == 0 and no_faults == 0 and not api.pending)

-- Revalidate NodeInfo at durable acknowledgement even without notify delivery.
api, journal, request, counts, node = setup (true)
request ()
node.properties ["pipewireao.rtc-control.owner-pid"] = "9999"
journal:finish (1)
assert (not api.pending and counts () == 0 and #journal.events == 2)

-- Post-effect storage errors expose uncertainty without repeating the effect.
api, journal, request, counts = setup (true)
request (); journal:finish (1)
ingress.complete (api, api.pending, 5, details)
journal:finish (2, false)
local effects, faults = counts ()
assert (effects == 1 and faults == 1 and not api.pending)
local header, payload = control.decode (api.completion, "completion")
assert (header.result < 0 and control.scalar (control.fields (payload) [2], "String"):
    find ("effect outcome unknown", 1, true))
request ()
assert (counts () == 1 and #journal.events == 2)

-- Session fault cleanup can follow an issued scientific effect whose adoption
-- was never observed. Exercise a real-style fault callback with operation state,
-- cancellation, deferred link withdrawal, and a changed scientific value.
for _, effects_issued in ipairs ({ false, true }) do
  api, journal, request, counts = setup (true)
  local session = { epoch = 0, scientific_value = 0 }
  local cleanup, cancelled
  api.request = function (ticket)
    session.operation = { ticket = ticket, effects_issued = effects_issued }
    if effects_issued then session.scientific_value = 7 end
  end
  api.fault = function (reason)
    if session.faulted then return end
    session.faulted, session.epoch = true, session.epoch + 1
    local previous = session.operation
    cancelled = true
    session.operation = nil
    api.lifecycle = 5
    -- Mirrors the production fault's asynchronous withdraw callback. The
    -- uncertainty argument is the same seam used by ao/session.lua.
    cleanup = function ()
      if previous and previous.ticket and api.pending == previous.ticket then
        ingress.complete (api, previous.ticket, 0, nil, tostring (reason),
            previous.effects_issued)
      end
    end
  end
  request (); journal:finish (1)
  local faulted_ticket = api.pending
  api.fault ("Scientific adoption observation failed")
  assert (cancelled and session.faulted and not session.operation)
  assert (#journal.events == 1 and api.pending == faulted_ticket)
  cleanup ()
  assert (#journal.events == 2 and journal.events [2].event ==
      (effects_issued and "unknown" or "completed"),
      "Fault after an issued effect was recorded as a known failed update")
  local fault_header, fault_payload = control.decode (journal.events [2].pod, "completion")
  local message = control.scalar (control.fields (fault_payload) [2], "String")
  assert (fault_header.result < 0)
  assert ((message:find ("effect outcome unknown", 1, true) ~= nil) == effects_issued)
  assert (session.scientific_value == (effects_issued and 7 or 0),
      "Fault completion must not claim or perform scientific rollback")
  journal:finish (2)
  cleanup (); request ()
  assert (not api.pending and #journal.events == 2)
end

-- A slow terminal write cannot exceed the caller's deadline or publish late success.
api, journal, request, counts = setup (true)
request (); journal:finish (1)
ticket = api.pending
ingress.complete (api, ticket, 5, details)
now = now + 1000001; ticket.expiry.callback ()
local expired_completion = api.completion
assert (not api.pending and #journal.events == 3 and journal.events [3].event == "unknown")
journal:finish (2)
assert (api.completion == expired_completion and counts () == 1)
journal:finish (3)

-- Only the two update commands are journaled; malformed update arity rejects first.
api, journal, request, counts = setup (true)
request (1, 13, Pod.Struct { "graph" })
assert (not api.pending and #journal.events == 0 and counts () == 0)
request (2, 2, Pod.Struct {})
assert (#journal.events == 0 and counts () == 1)
ingress.complete (api, api.pending, 2, Pod.Struct {})
request (3, 14, Pod.Struct { "graph", "algorithm", "parameter", Pod.Id (1), Pod.Long (1), Pod.Struct {} })
assert (#journal.events == 1 and counts () == 1)
journal:finish (1)
assert (counts () == 2)

-- Configuration-free rollout preserves immediate request/completion behavior.
api, journal, request, counts = setup (false)
request ()
assert (counts () == 1 and #journal.events == 0)
ingress.complete (api, api.pending, 5, details)
assert (not api.pending and #journal.events == 0)
