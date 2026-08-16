#ifndef ATANSTACK_AUTOMATION_SCHEDULE_H
#define ATANSTACK_AUTOMATION_SCHEDULE_H

#include <stdint.h>

// Builds a monotonically increasing key for a local civil minute. The year is
// included so the key cannot collide across years (or DST transitions).
inline uint32_t automationMinuteKey(int year, int yday, int hour, int minute) {
  return ((uint32_t)(year * 366 + yday) * 24u + (uint32_t)hour) * 60u +
         (uint32_t)minute;
}

// True when the rule time matches the given minute and it has not already
// fired for that minute. Matching minuteKey % 60 and (minuteKey / 60) % 24
// keeps the check free of any timezone dependence.
inline bool automationMinuteDue(uint32_t minuteKey,
                                uint8_t ruleHour,
                                uint8_t ruleMinute,
                                uint32_t lastFiredKey) {
  if (minuteKey == lastFiredKey) {
    return false;
  }
  return (uint8_t)(minuteKey % 60u) == ruleMinute &&
         (uint8_t)((minuteKey / 60u) % 24u) == ruleHour;
}

// True when a pending end time has been reached.
inline bool automationEndDue(uint32_t nowEpoch, uint32_t endEpoch) {
  return endEpoch != 0 && nowEpoch >= endEpoch;
}

#endif  // ATANSTACK_AUTOMATION_SCHEDULE_H
