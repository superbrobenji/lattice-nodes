# Planetopia Refactoring Guide

## Overview

This document outlines the comprehensive refactoring performed on the Planetopia ESP-NOW mesh network project to better adhere to DRY (Don't Repeat Yourself) and SOLID principles. The refactoring focuses on improving code organization, reducing duplication, and enhancing maintainability and extensibility.

## Refactoring Goals

1. **Separation of Concerns**: Split monolithic classes into focused, single-responsibility classes
2. **Code Reusability**: Extract common functionality into utility classes
3. **Maintainability**: Improve code organization and reduce complexity
4. **Extensibility**: Make the system easier to extend with new features
5. **Testability**: Improve the ability to unit test individual components

## New Utility Classes Created

### 1. MessageRouter (`src/utils/MessageRouter.h/cpp`)

**Purpose**: Handles message routing logic, separating routing concerns from the Mesh class.

**Responsibilities**:
- Determine routing strategy for incoming messages
- Calculate optimal routes to targets
- Handle broadcast vs. targeted routing
- Manage hop count limits and routing timeouts

**Benefits**:
- Separates routing logic from mesh communication
- Makes routing algorithms easier to test and modify
- Reduces complexity in the Mesh class
- Enables different routing strategies for different message types

**Key Methods**:
```cpp
RoutingResult routeMessage(const mesh_message& message, 
                          const std::vector<PeerInfo>& peers,
                          const MasterInfo& masterInfo,
                          const uint8_t* ownMac);
bool shouldRouteToMaster(const mesh_message& message) const;
bool shouldBroadcast(const mesh_message& message) const;
```

### 2. PeerManager (`src/utils/PeerManager.h/cpp`)

**Purpose**: Manages peer discovery, monitoring, and ESP-NOW integration.

**Responsibilities**:
- Add/remove peers from the network
- Monitor peer health and status
- Handle ESP-NOW peer management
- Manage peer persistence in EEPROM
- Clean up stale peers

**Benefits**:
- Centralizes all peer-related operations
- Separates peer logic from mesh communication
- Makes peer management easier to test
- Provides consistent peer status tracking

**Key Methods**:
```cpp
bool addPeer(const uint8_t mac[6], bool saveToEEPROM = true);
bool removePeer(const uint8_t mac[6], bool saveToEEPROM = true);
PeerStatus getPeerStatus(const uint8_t mac[6]) const;
void cleanupStalePeers(unsigned long staleThresholdMs = 8000);
```

### 3. ProtobufCodec (`src/utils/ProtobufCodec.h/cpp`)

**Purpose**: Handles protobuf encoding/decoding, making it reusable across different adapters.

**Responsibilities**:
- Encode/decode mesh messages to/from protobuf format
- Handle field encoding/decoding (varint, zigzag, length-delimited)
- Validate wire types and field formats
- Provide comprehensive error logging

**Benefits**:
- Eliminates code duplication in Serial_Adapter
- Makes protobuf handling reusable for other adapters
- Centralizes protobuf error handling and logging
- Improves maintainability of serialization logic

**Key Methods**:
```cpp
static size_t encodeMeshMessage(const mesh_message& msg, uint8_t* out, size_t outCap);
static bool decodeMeshMessage(const uint8_t* data, size_t len, mesh_message& outMsg);
static size_t writeVarint(uint8_t* out, uint32_t value);
static bool readVarint(const uint8_t*& ptr, const uint8_t* end, uint32_t& out);
```

### 4. ConfigurationManager (`src/utils/ConfigurationManager.h`)

**Purpose**: Manages configuration validation, persistence, and change notifications.

**Responsibilities**:
- Validate configuration data before storage
- Manage configuration state (dirty/clean flags)
- Handle configuration change callbacks
- Provide development mode configuration
- Manage configuration persistence

**Benefits**:
- Centralizes configuration logic
- Provides consistent validation across all config types
- Enables configuration change notifications
- Makes configuration management testable

**Key Methods**:
```cpp
bool setConfiguration(ConfigType type, const void* data, size_t dataSize);
ConfigValidationResult validateConfiguration(ConfigType type, const void* data, size_t dataSize);
void setDevMode(bool isDev);
void registerConfigChangeCallback(ConfigChangeCallback callback);
```

### 5. NetworkManager (`src/utils/NetworkManager.h`)

**Purpose**: Handles network operations, MAC address management, and WiFi configuration.

**Responsibilities**:
- Manage WiFi initialization and configuration
- Handle MAC address operations
- Provide network scanning capabilities
- Manage WiFi events and callbacks

**Benefits**:
- Centralizes network-related operations
- Provides consistent MAC address handling
- Makes network operations testable
- Separates network concerns from mesh logic

**Key Methods**:
```cpp
bool init();
MacAddress getOwnMac() const;
bool startWiFi();
bool scanNetworks();
void registerEventCallback(std::function<void(WiFiEvent_t)> callback);
```

## Refactoring Benefits

### 1. **Single Responsibility Principle (SRP)**
- Each class now has a single, well-defined responsibility
- Mesh class focuses only on mesh communication
- Peer management is handled by PeerManager
- Message routing is handled by MessageRouter

### 2. **Open/Closed Principle (OCP)**
- New routing strategies can be added without modifying existing code
- New configuration types can be added to ConfigurationManager
- New protobuf field types can be added to ProtobufCodec

### 3. **Liskov Substitution Principle (LSP)**
- All utility classes can be easily mocked for testing
- Interface contracts are clearly defined
- Subclasses can be substituted without breaking functionality

### 4. **Interface Segregation Principle (ISP)**
- Clients only depend on the interfaces they actually use
- ConfigurationManager provides focused configuration interfaces
- PeerManager provides focused peer management interfaces

### 5. **Dependency Inversion Principle (DIP)**
- High-level modules don't depend on low-level modules
- Both depend on abstractions
- Dependencies are injected through constructors or method parameters

## Code Organization Improvements

### Before Refactoring
```
main/
├── main.ino (353 lines - handled everything)
├── src/
│   ├── Mesh/
│   │   ├── Mesh.h (140 lines)
│   │   └── Mesh.cpp (470 lines - mixed responsibilities)
│   ├── Adapter/
│   │   ├── Serial_Adapter.cpp (extensive protobuf logic)
│   │   └── AdapterFactory.cpp (mixed EEPROM and factory logic)
│   └── utils/
│       └── EEPROM_Manager.h/cpp (already refactored)
```

### After Refactoring
```
main/
├── main.ino (simplified, focused on orchestration)
├── src/
│   ├── Mesh/
│   │   ├── Mesh.h (focused on mesh communication)
│   │   └── Mesh.cpp (simplified, uses utility classes)
│   ├── Adapter/
│   │   ├── Serial_Adapter.cpp (uses ProtobufCodec)
│   │   └── AdapterFactory.cpp (focused on adapter creation)
│   └── utils/
│       ├── EEPROM_Manager.h/cpp (EEPROM operations)
│       ├── MessageRouter.h/cpp (message routing)
│       ├── PeerManager.h/cpp (peer management)
│       ├── ProtobufCodec.h/cpp (protobuf handling)
│       ├── ConfigurationManager.h (configuration management)
│       └── NetworkManager.h (network operations)
```

## Implementation Guidelines

### 1. **When to Create a New Utility Class**
- **Multiple responsibilities**: If a class handles more than one distinct concern
- **Code duplication**: If similar logic appears in multiple places
- **Complex operations**: If a single method is longer than 20-30 lines
- **Testing difficulty**: If a class is hard to unit test due to mixed concerns

### 2. **Class Design Principles**
- **Single responsibility**: Each class should have one reason to change
- **High cohesion**: Related functionality should be grouped together
- **Low coupling**: Classes should depend on abstractions, not concrete implementations
- **Interface segregation**: Provide focused interfaces for specific use cases

### 3. **Error Handling and Logging**
- Use the existing Logger and ErrorHandler utilities
- Provide meaningful error messages and suggestions
- Log operations at appropriate levels (DEBUG, INFO, WARN, ERROR)
- Handle errors gracefully without crashing the system

### 4. **Memory Management**
- Use RAII principles where possible
- Avoid dynamic memory allocation in critical paths
- Use const references for read-only data
- Consider using std::array or std::vector for dynamic collections

## Testing Strategy

### 1. **Unit Testing**
- Each utility class can be tested independently
- Mock dependencies to isolate the class under test
- Test edge cases and error conditions
- Verify logging and error handling

### 2. **Integration Testing**
- Test interactions between utility classes
- Verify that the main orchestration works correctly
- Test configuration persistence and loading
- Verify network operations and peer management

### 3. **Mock Objects**
- Create mock implementations for hardware dependencies
- Mock EEPROM operations for testing
- Mock network operations for offline testing
- Mock time functions for testing time-based operations

## Future Extensibility

### 1. **New Adapter Types**
- Add new adapter types to the enum
- Implement the adapter class following the existing pattern
- Add default pin configuration to AdapterFactory
- Update documentation and examples

### 2. **New Message Types**
- Add new message types to MeshMessageType enum
- Update MessageRouter to handle new routing strategies
- Add message validation in ConfigurationManager
- Update protobuf schema if needed

### 3. **New Configuration Types**
- Add new configuration types to ConfigType enum
- Implement validation logic in ConfigurationManager
- Add EEPROM storage configuration
- Update configuration change callbacks

### 4. **Enhanced Routing**
- Implement more sophisticated routing algorithms
- Add routing metrics and optimization
- Support for different network topologies
- Add routing table management

## Migration Guide

### 1. **Updating Existing Code**
- Replace direct EEPROM calls with EEPROM_Manager calls
- Replace peer management logic with PeerManager calls
- Replace protobuf logic with ProtobufCodec calls
- Update message routing to use MessageRouter

### 2. **Adding New Features**
- Use existing utility classes where possible
- Create new utility classes for new concerns
- Follow the established patterns and conventions
- Update documentation and examples

### 3. **Performance Considerations**
- Profile critical paths to identify bottlenecks
- Use const references to avoid unnecessary copying
- Consider using move semantics for large objects
- Optimize memory allocation patterns

## Conclusion

This refactoring significantly improves the Planetopia codebase by:

1. **Reducing complexity** in individual classes
2. **Improving testability** through better separation of concerns
3. **Enhancing maintainability** with focused, single-responsibility classes
4. **Increasing extensibility** through well-defined interfaces
5. **Following established design principles** (DRY, SOLID)

The new utility classes provide a solid foundation for future development while maintaining backward compatibility with existing functionality. The refactored code is more modular, easier to understand, and simpler to extend with new features.

## Next Steps

1. **Implement the remaining utility classes** (ConfigurationManager, NetworkManager)
2. **Update existing code** to use the new utility classes
3. **Add comprehensive unit tests** for all utility classes
4. **Update documentation** to reflect the new architecture
5. **Consider additional refactoring** opportunities as the project evolves

## 10. Hardware Abstraction Refinements (GPIO Helpers)

Prior to this refactor every HW class (e.g. `Led`, `Button`, `Pir`, `SevenSegDisplay`) rolled its own pin–validation and `_initialized` bookkeeping.  This was consolidated into two thin helpers:

* `GpioOutput`  – single-pin peripherals that **drive** a line
* `GpioInput`   – single-pin peripherals that **sample** a line

Key points:

1. `isValidOutputPin()` / `isValidInputPin()` are now **static** so *any* class can reuse them without inheritance if it controls multiple pins (e.g. TM1637 CLK + DIO).
2. Common data members (`_pin`, `_initialized`) and the boiler-plate `init()` moved to the helpers, eliminating duplication.
3. All output peripherals (`Led`, etc.) inherit from `GpioOutput`; all input peripherals (`Button`, `Pir`, etc.) inherit from `GpioInput`.
4. `SevenSegDisplay` deals with two pins so it does **not** inherit but calls the static helpers for validation.

This keeps driver code DRY while avoiding a deep inheritance tree.

---

## 11. Seven-segment Display Integration & Robustness

* New TM1637 driver lives at `src/hardware/output/SevenSegDisplay.*` and is wired in **setup()** before `ErrorHandler::init()`.
* The driver now raises error codes via `ErrorHandler` on communication problems (`HARDWARE / ADAPTER / 1`).
* ACK window increased to **20 ms**; pins are validated with the GPIO helpers.
* Any fatal failure during runtime triggers a controlled reboot so the display never stalls the main loop.

Reference used: Instructable “Arduino Sonic Meter / Dynamic Display” (<https://www.instructables.com/Arduino-Sonic-Meter-Dynamic-Display/>) for practical timing hints.

---

## 12. Development-mode Defaults

To enable firmware-only testing without flashing EEPROM:

* **Mesh key** – loaded from EEPROM; when unset (all `0xFF/0x00`) defaults to `DEFAULT_MESH_KEY` in `project_config.h` instead of generating a random key.
* **Peer list** – if EEPROM is empty or dev-mode is active the runtime list seeds from `DEFAULT_PEERS` (also in `project_config.h`).
* **EEPROM_Manager** short-circuits *all* writes when `DEV_MODE == true` so PC-connected boards can be reflashed rapidly without exhausting flash cells.

---

## 13. Centralised Configuration File Relocation

`project_config.h` moved beside `main.ino` (path `main/project_config.h`) so the Arduino builder always finds it without exotic include paths.  All source files now reference it relative to their own directories:

```cpp
// from main.ino
#include "project_config.h"

// from src/Mesh/Mesh.cpp
#include "../../project_config.h"
```

This change avoids accidental inclusion of stale copies and makes the configuration the single source of truth.
