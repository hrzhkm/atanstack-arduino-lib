#include <cassert>
#include <cstdio>

#include "../src/automationSchedule.h"

int main() {
  assert(automationMinuteKey(2026, 100, 7, 30) !=
         automationMinuteKey(2027, 100, 7, 30));
  assert(automationMinuteKey(2026, 100, 7, 30) !=
         automationMinuteKey(2026, 101, 7, 30));

  const uint32_t key = automationMinuteKey(2026, 100, 7, 30);

  assert(automationMinuteDue(key, 7, 30, 0));
  assert(automationMinuteDue(key, 7, 30, key - 1));
  assert(!automationMinuteDue(key, 7, 30, key));
  assert(!automationMinuteDue(key, 7, 29, 0));
  assert(!automationMinuteDue(key, 6, 30, 0));

  assert(automationEndDue(1000, 1000));
  assert(automationEndDue(1001, 1000));
  assert(!automationEndDue(999, 1000));
  assert(!automationEndDue(1000, 0));

  std::printf("automation schedule host spec passed\n");
  return 0;
}
