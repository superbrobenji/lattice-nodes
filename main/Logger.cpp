#define DEBUG 
#include "Logger.h"

void Logger::log(const String& tag, const String& message) {
#ifdef DEBUG
  Serial.print("[");
  Serial.print(tag);
  Serial.print("] ");
  Serial.print(message);
#endif
}

void Logger::logln(const String& tag, const String& message) {
  log(tag, message);
#ifdef DEBUG
  Serial.println();
#endif
}
