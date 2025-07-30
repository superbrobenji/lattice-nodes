#ifndef ADAPTER_FACTORY_H
#define ADAPTER_FACTORY_H

#include "Adapter.h"
#include "PIR_Adapter.h"
// #include "WiFi_Adapter.h"
// #include "LED_Adapter.h"

class AdapterFactory {
public:
  static Adapter* createAdapter(adapter_types type, int pin);
};

#endif // ADAPTER_FACTORY_H
