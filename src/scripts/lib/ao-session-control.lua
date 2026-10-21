-- WirePlumber
-- SPDX-License-Identifier: MIT

-- Native public session control ingress. The policy executes operations;
-- this adapter only proves controller incarnations and publishes typed records.
local control = require ("ao-control")
local server = {}
local PREFIX = "pipewireao.rtc.session."
local PROFILE = "pipewireao.rtc.controller/1"
local INFO = Feature.Proxy.BOUND | Feature.PipewireObject.INFO
local prove

local function identity_equal (a, b)
  return a.global_id == b.global_id and a.serial == b.serial and a.instance == b.instance
end

local function capability (self)
  local controllers = {}
  for _, row in ipairs (self.controllers) do
    if not row.retired then
      controllers [#controllers + 1] = Pod.Struct { Pod.Id (row.global_id),
        Pod.Long (row.serial), Pod.Long (row.instance) }
    end
  end
  return Pod.Object { "Spa:Pod:Object:Param:Props", "Props", params = Pod.Struct {
    PREFIX .. "version", Pod.Int (1), PREFIX .. "instance", Pod.Long (self.instance),
    PREFIX .. "owner-pid", Pod.Id (self.endpoint:call ("get-owner-pid")),
    PREFIX .. "lifecycle", Pod.Id (self.lifecycle),
    PREFIX .. "last-token", Pod.Long (self.last_token),
    PREFIX .. "controllers", Pod.Struct (controllers),
  } }
end

function server.publish (self)
  if self.transport_lost then return false end
  if self.endpoint:call ("publish-records", capability (self),
      self.completion, self.rejection) then return true end
  self.transport_lost = true
  pcall (self.fault, "Session control publication failed")
  self.endpoint:call ("disconnect")
  return false
end

local function reply_header (self, request, result)
  return { endpoint = self.instance, global_id = request and request.global_id or 0,
    serial = request and request.serial or 0, instance = request and request.instance or 0,
    token = request and request.token or 0, operation = request and request.operation or 0,
    result = result }
end

function server.reject (self, header, result, message)
  self.rejection = control.encode ("rejection", reply_header (self, header, result),
      Pod.Struct { "session", message, Pod.Id (self.lifecycle) })
  server.publish (self)
end

function server.check_ticket (self, ticket)
  assert (not self.transport_lost, "Session control transport lost")
  assert (self.pending == ticket, "Superseded public session operation")
  assert (Core.get_monotonic_time () < ticket.deadline, "Session request expired")
  for _, row in ipairs (self.controllers) do
    if not row.retired and identity_equal (row, ticket.header) then
      local fresh = prove (row.node)
      assert (identity_equal (row, fresh) and fresh.pid == row.pid and
          fresh.name == row.name, "Session controller identity changed")
      return row
    end
  end
  error ("Session controller disappeared or changed")
end

-- append is admission-only; all I/O and completion delivery belong to the
-- journal plugin. A zero job id is a bounded queue/configuration rejection.
local function journal_append (self, ticket, event, record, callback)
  local ok, id = pcall (self.journal.call, self.journal, "append", self.session_id,
      event, record, ticket.client_name, ticket.client_pid, ticket.operation_key)
  if not ok or not id or id <= 0 then
    callback (false, ok and "Journal admission rejected" or tostring (id))
    return
  end
  self.journal_jobs [id] = callback
end

local function terminal (self, ticket, record)
  if self.pending ~= ticket then return end
  self.pending, self.terminal = nil, ticket
  if ticket.expiry then ticket.expiry:destroy () end
  self.completion = record
  server.publish (self)
end

local function failure_record (self, ticket, message)
  return control.encode ("completion", reply_header (self, ticket.header, -5),
      Pod.Struct { "session", message, Pod.Id (self.lifecycle) })
end

-- Deadline/disappearance can race either durable append. Fence the ticket
-- before any late worker callback, and never dispatch an expired request.
local function cancel_journal_ticket (self, ticket, message)
  if self.pending ~= ticket then return end
  ticket.finishing = true
  local unknown = ticket.dispatched == true
  local record = failure_record (self, ticket, message)
  terminal (self, ticket, record)
  -- Best effort terminal evidence, ordered after the original append. Failure
  -- leaves a requested record unresolved; there is deliberately no replay.
  journal_append (self, ticket, unknown and "unknown" or "completed", record,
      function () end)
  if unknown then self.fault (message) end
end

function server.complete (self, ticket, outcome, details, error, uncertain)
  assert (self.pending == ticket, "Superseded public session operation")
  if ticket.finishing then return end -- exactly one terminal append per attempt
  if ticket.journaled and uncertain then
    -- Fault cleanup can hold/withdraw the graph but cannot establish whether an
    -- already-issued scientific update was adopted or undo that update.
    ticket.unknown = true
    error = tostring (error or "Scientific operation outcome unavailable")
    if not error:find ("effect outcome unknown", 1, true) then
      error = error .. "; effect outcome unknown"
    end
  end
  if not error then
    local valid, reason = pcall (server.check_ticket, self, ticket)
    if not valid then
      error = tostring (reason)
      if ticket.journaled then
        ticket.unknown = true
        error = error .. "; effect outcome unknown"
      end
      self.fault (error)
      if self.pending ~= ticket then return end
    end
  end
  local payload = error and Pod.Struct { "session", error, Pod.Id (self.lifecycle) } or
      Pod.Struct { Pod.Id (self.lifecycle), Pod.Id (outcome), details }
  local record = control.encode ("completion", reply_header (self, ticket.header,
      error and -5 or 0), payload)
  if not ticket.journaled then
    terminal (self, ticket, record)
    return
  end
  ticket.finishing = true
  journal_append (self, ticket, ticket.unknown and "unknown" or "completed", record,
      function (ok, reason)
    if self.pending ~= ticket then return end
    if Core.get_monotonic_time () >= ticket.deadline then
      cancel_journal_ticket (self, ticket, "Session operation expired; effect outcome unknown")
    elseif ok then terminal (self, ticket, record)
    else
      -- A completed scientific effect cannot be rolled back by a storage
      -- failure. Expose uncertainty and never automatically repeat the effect.
      terminal (self, ticket, failure_record (self, ticket,
          "Journal persistence failed; effect outcome unknown: " .. tostring (reason)))
      self.fault ("Journal persistence failed after dispatch; effect outcome unknown")
    end
  end)
end

function server.complete_admin (self, ticket, lifecycle, warmed)
  server.check_ticket (self, ticket)
  assert (ticket.header.operation == 15 or ticket.header.operation == 16,
      "Not a resource admission operation")
  self.pending, self.terminal, self.lifecycle = nil, ticket, lifecycle
  if ticket.expiry then ticket.expiry:destroy () end
  self.completion = control.encode ("completion", reply_header (self, ticket.header, 0),
      Pod.Struct { Pod.Id (lifecycle), Pod.Boolean (warmed) })
  server.publish (self)
end

function server.set_lifecycle (self, state)
  self.lifecycle = state
  server.publish (self)
end

prove = function (node)
  local props = node ["properties"]
  local identity = control.identity (node)
  local instance, pid = tonumber (props ["pipewireao.rtc-control.instance"]),
      tonumber (props ["pipewireao.rtc-control.owner-pid"])
  assert (props ["pipewireao.rtc-control.protocol"] == "pipewireao.rtc-control/1" and
      props ["pipewireao.rtc-control.profile"] == PROFILE and
      math.type (instance) == "integer" and instance > 0 and
      math.type (pid) == "integer" and pid > 0 and pid < 0xffffffff and
      node:get_n_input_ports () == 0 and node:get_n_output_ports () == 0,
      "Invalid native controller NodeInfo")
  identity.instance, identity.pid, identity.node, identity.name =
      instance, pid, node, props ["node.name"]
  assert (identity.name and #identity.name > 0, "Controller node name missing")
  return identity
end

local function request (self, pod, received_us)
  local ok, header, payload = pcall (control.decode, pod, "request")
  if not ok then server.reject (self, nil, -22, "Malformed session request"); return end
  local controller
  for _, row in ipairs (self.controllers) do
    if not row.retired and identity_equal (row, header) then controller = row end
  end
  if header.endpoint ~= self.instance or not controller then
    server.reject (self, header, -116, "Stale endpoint/controller incarnation"); return
  end
  local valid, fresh = pcall (prove, controller.node)
  if not valid or not identity_equal (controller, fresh) or fresh.pid ~= controller.pid or
      fresh.name ~= controller.name then
    controller.retired = true
    server.reject (self, header, -116, "Controller NodeInfo changed")
    return
  end
  for _, previous in ipairs ({ self.pending or false, self.terminal or false }) do
    if previous and header.token == previous.header.token and
        header.operation == previous.header.operation and
        identity_equal (header, previous.header) and
        header.budget_ns == previous.header.budget_ns and payload:equals (previous.payload) then
      server.publish (self)
      return
    end
  end
  if self.closing then
    server.reject (self, header, -108, "Session shutdown already accepted"); return
  end
  if self.pending then
    server.reject (self, header, -16, "Session operation already pending"); return
  end
  if header.token <= self.last_token then
    server.reject (self, header, -116, "Session token already used or superseded"); return
  end
  local bounded, deadline = pcall (control.deadline, header.budget_ns)
  if not bounded then server.reject (self, header, -22, "Invalid request budget"); return end
  deadline = received_us + ((header.budget_ns + 999) // 1000)
  if Core.get_monotonic_time () >= deadline then
    server.reject (self, header, -110, "Request budget expired in transport handoff"); return
  end
  local journaled = self.journal and (header.operation == 13 or header.operation == 14)
  if journaled then
    local shape = pcall (control.fields, payload, header.operation == 13 and 2 or 6)
    if not shape then
      server.reject (self, header, -22, "Invalid session command fields"); return
    end
  end
  local ticket = { header = header, payload = payload, deadline = deadline,
    journaled = journaled, client_name = controller.name, client_pid = controller.pid,
    operation_key = journaled and table.concat ({ self.session_id, tostring (header.endpoint),
      tostring (header.global_id), tostring (header.serial), tostring (header.instance),
      tostring (header.token) }, ":") or nil }
  self.last_token, self.pending = header.token, ticket
  ticket.expiry = Core.timeout_add (math.max (1, math.ceil (
      (deadline - Core.get_monotonic_time ()) / 1000)), function ()
    if self.pending ~= ticket then return false end
    if ticket.journaled then
      cancel_journal_ticket (self, ticket, ticket.dispatched and
          "Session operation expired; effect outcome unknown" or
          "Session request expired before dispatch; no scientific effect issued")
      return false
    end
    self.fault ("Public session operation expired; effect outcome unknown")
    if self.pending == ticket then
      server.complete (self, ticket, 0, nil, "Session operation expired; effect outcome unknown")
    end
    return false
  end)
  local function dispatch ()
    if self.pending ~= ticket or ticket.finishing then return end
    local valid, reason = true, nil
    if ticket.journaled then valid, reason = pcall (server.check_ticket, self, ticket) end
    if not valid then
      cancel_journal_ticket (self, ticket,
          "Session request cancelled before dispatch: " .. tostring (reason))
      return
    end
    ticket.dispatched = true
    local applied, error = pcall (self.request, ticket)
    if not applied then
      self.fault ("Session handler failed: " .. tostring (error))
      if self.pending == ticket then server.complete (self, ticket, 0, nil, tostring (error)) end
    end
  end
  if journaled then
    -- Preserve the entire v1 request envelope, including both incarnations,
    -- token, operation, deadline budget, and the exact typed input payload.
    journal_append (self, ticket, "requested", pod, function (ok, reason)
      if self.pending ~= ticket or ticket.finishing then return end
      if not ok then
        terminal (self, ticket, failure_record (self, ticket,
            "Journal failed before dispatch; no scientific effect issued: " .. tostring (reason)))
        return -- a storage failure cannot fault a healthy scientific session
      end
      dispatch ()
    end)
  else dispatch () end
end

function server.new (endpoint, instance, request_handler, fault_handler, journal, session_id)
  local self = { endpoint = endpoint, instance = instance, request = request_handler,
    fault = fault_handler, lifecycle = 1, last_token = 0, controllers = {},
    journal = journal, session_id = session_id, journal_jobs = {} }
  if journal then
    assert (type (session_id) == "string" and #session_id > 0, "Journal requires session.uuid")
    journal:connect ("completed", function (_, id, ok, reason)
      local callback = self.journal_jobs [id]
      self.journal_jobs [id] = nil
      if callback then callback (ok, reason) end
    end)
  end
  self.completion = control.encode ("completion", reply_header (self, nil, 0), Pod.Struct {})
  self.rejection = control.encode ("rejection", reply_header (self, nil, -11),
      Pod.Struct { "session", "No rejected request", Pod.Id (self.lifecycle) })
  self.manager = ObjectManager ({ Interest { type = "node",
    Constraint { "pipewireao.rtc-control.profile", "=", PROFILE, type = "pw" } } }, INFO)
  self.manager:connect ("object-added", function (_, node)
    if #self.controllers >= 32 then return end
    local ok, row = pcall (prove, node)
    if not ok then return end
    self.controllers [#self.controllers + 1] = row
    local function changed ()
      local valid, fresh = pcall (prove, node)
      if not valid or not identity_equal (row, fresh) or fresh.pid ~= row.pid or
          fresh.name ~= row.name then
        row.retired = true
        if self.pending and self.pending.journaled and not self.pending.dispatched and
            identity_equal (row, self.pending.header) then
          cancel_journal_ticket (self, self.pending, "Session controller changed before dispatch")
        end
        server.publish (self)
      end
    end
    node:connect ("notify::properties", changed)
    node:connect ("notify::n-input-ports", changed)
    node:connect ("notify::n-output-ports", changed)
    server.publish (self)
  end)
  self.manager:connect ("object-removed", function (_, node)
    local remaining = {}
    for _, row in ipairs (self.controllers) do
      if row.node ~= node then remaining [#remaining + 1] = row end
    end
    self.controllers = remaining
    if self.pending and self.pending.journaled and not self.pending.dispatched then
      local valid = pcall (server.check_ticket, self, self.pending)
      if not valid then
        cancel_journal_ticket (self, self.pending, "Session controller disappeared before dispatch")
      end
    end
    server.publish (self)
  end)
  endpoint:connect ("parameter", function (_, pod, received_us) request (self, pod, received_us) end)
  endpoint:connect ("transport-error", function (_, error)
    server.reject (self, nil, -22, error)
  end)
  endpoint:connect ("state-changed", function (_, state, error)
    if state < 1 then
      if self.pending and self.pending.journaled and not self.pending.dispatched then
        cancel_journal_ticket (self, self.pending, "Session transport lost before dispatch")
      end
      self.fault ("Session control transport lost: " .. error)
    end
  end)
  self.manager:activate ()
  server.publish (self)
  return self
end

return server
