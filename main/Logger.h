#ifndef LOGGER_H
#define LOGGER_H

#include <Arduino.h>

class Logger {
public:
  static void log(const String& tag, const String& message);
  static void logln(const String& tag, const String& message);
};

#endif
