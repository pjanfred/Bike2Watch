#ifndef BIKE2WATCH_LOG_H
#define BIKE2WATCH_LOG_H

// Serial logging, compiled out unless the build defines DEBUG_LOG=1
// (see the `debug` environment in platformio.ini).
#if DEBUG_LOG
  #define LOG(fmt, ...) Serial.printf(fmt "\n", ##__VA_ARGS__)
#else
  #define LOG(fmt, ...)
#endif

#endif // BIKE2WATCH_LOG_H
