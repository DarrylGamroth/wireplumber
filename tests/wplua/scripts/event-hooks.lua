Script.async_activation = true

local tags = {}

local function checkpoint(tag)
  Log.info("at " .. tag)
  table.insert(tags, tag)
end

local function check_results()
  local i = 0
  local function inc()
    i = i+1
    return i
  end

  assert(tags[inc()] == "simple-first")
  assert(tags[inc()] == "simple-1")
  assert(tags[inc()] == "async-start")
  assert(tags[inc()] == "async-start-advance")
  assert(tags[inc()] == "async-step2")
  assert(tags[inc()] == "async-raise")
  assert(tags[inc()] == "simple-last")
end

local common_interests = {
  EventInterest {
    Constraint { "event.type", "=", "test-event" },
  },
}

local property_events_seen = 0

local function check_property_event (event, expected)
  local properties = event:get_properties ():parse ()
  assert (properties ["custom.test-property"] == expected)
  property_events_seen = property_events_seen + 1
  if property_events_seen == 3 then
    Script:finish_activation ()
  end
end

SimpleEventHook {
  name = "test-table-event-properties",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "test-table-properties" },
      Constraint { "custom.test-property", "=", "table-value" },
    },
  },
  execute = function (event)
    check_property_event (event, "table-value")
  end,
}:register ()

SimpleEventHook {
  name = "test-wp-properties-event-properties",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "test-wp-properties" },
      Constraint { "custom.test-property", "=", "properties-value" },
    },
  },
  execute = function (event)
    check_property_event (event, "properties-value")
  end,
}:register ()

SimpleEventHook {
  name = "test-omitted-event-properties",
  interests = {
    EventInterest {
      Constraint { "event.type", "=", "test-omitted-properties" },
    },
  },
  execute = function (event)
    assert (event:get_properties ():parse () ["custom.test-property"] == nil)
    property_events_seen = property_events_seen + 1
    if property_events_seen == 3 then
      Script:finish_activation ()
    end
  end,
}:register ()

AsyncEventHook {
  name = "test-async-hook",
  before = "test-last-hook" ,
  after = { "test-first-hook", "test-simple-hook" },
  interests = common_interests,
  steps = {
    start = {
      next = "step2",
      execute = function (event, transition)
        checkpoint("async-start")
        Core.idle_add(function ()
          checkpoint("async-start-advance")
          transition:advance()
          return false
        end)
      end,
    },
    step2 = {
      next = "none",
      execute = function (event, transition)
        checkpoint("async-step2")
        transition:advance()
      end,
    },
  },
}:register()

AsyncEventHook {
  name = "test-raising-hook",
  before = "test-last-hook",
  after = "test-async-hook",
  interests = common_interests,
  steps = {
    start = {
      next = "none",
      execute = function (event, transition)
        checkpoint("async-raise")
        error("intentional error from test-raising-hook")
      end,
    },
  },
}:register()

SimpleEventHook {
  name = "test-first-hook",
  before = { "test-simple-hook", "test-last-hook" },
  interests = common_interests,
  execute = function (event)
    checkpoint("simple-first")
  end
}:register()

SimpleEventHook {
  name = "test-simple-hook",
  after = { "test-first-hook" },
  before = {},
  interests = common_interests,
  execute = function (event)
    checkpoint("simple-1")
  end
}:register()

SimpleEventHook {
  name = "test-last-hook",
  interests = common_interests,
  execute = function (event)
    checkpoint("simple-last")
    check_results()
    EventDispatcher.push_event {
      type = "test-table-properties",
      priority = 1,
      properties = { ["custom.test-property"] = "table-value" },
    }
    EventDispatcher.push_event {
      type = "test-wp-properties",
      priority = 1,
      properties = Properties { ["custom.test-property"] = "properties-value" },
    }
    EventDispatcher.push_event {
      type = "test-omitted-properties",
      priority = 1,
    }
  end
}:register()

EventDispatcher.push_event { type = "test-event", priority = 1 }
