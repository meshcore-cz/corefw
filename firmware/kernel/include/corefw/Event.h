// The corefw event system.
//
// Components are event-driven wherever practical: an idle component should not
// need to run on every main-loop iteration. The kernel dispatches these events;
// modules react to the ones they care about in onEvent().
#pragma once

#include <cstdint>

namespace corefw {

enum class EventType : uint8_t {
  PacketReceived,
  PacketForLocalNode,
  PacketForwarded,
  BatteryChanged,
  PowerSourceChanged,
  GPSFixAvailable,
  CompanionConnected,
  CompanionDisconnected,
  ConfigurationChanged,
  BeforeSleep,
  AfterWake,
  TimerExpired,
};

// Event is a lightweight, tagged union-ish carrier. The payload pointer is
// interpreted according to type; the kernel owns the referenced memory for the
// duration of dispatch.
struct Event {
  EventType type;
  const void* data = nullptr;
  int32_t value = 0;  // convenience scalar (battery %, timer id, ...)
};

}  // namespace corefw
