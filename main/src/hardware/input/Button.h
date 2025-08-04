#ifndef BUTTON_H
#define BUTTON_H

#include "GpioInput.h"

namespace planetopia {
namespace hardware {

class Button : public GpioInput {
public:
    explicit Button(uint8_t pin);
    // Returns true if the button is currently pressed (active LOW)
    bool isPressed();
    // Returns true if held for the given ms (blocking call)
    bool waitForHold(unsigned long ms);
};

}  // namespace hardware
}  // namespace planetopia

#endif
