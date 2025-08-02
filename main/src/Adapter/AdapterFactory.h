#ifndef ADAPTER_FACTORY_H
#define ADAPTER_FACTORY_H

#include "Adapter.h"
#include "src/Adapter/PIR_Adapter/PIR_Adapter.h"
// #include "WiFi_Adapter.h"
// #include "LED_Adapter.h"

namespace planetopia {
namespace adapter {

class AdapterFactory {
public:
  static Adapter* createAdapter(adapter_types type, int pin);
};

}
}

#endif  // ADAPTER_FACTORY_H
