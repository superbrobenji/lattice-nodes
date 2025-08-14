# Planetopia - ESP-NOW Mesh Network System

A robust ESP-NOW mesh networking system for ESP32 devices with dynamic adapter capabilities, designed for IoT applications requiring reliable, low-power wireless communication.

## 🚀 Features

- **ESP-NOW Mesh Networking**: Low-latency, peer-to-peer communication between ESP32 devices
- **Dynamic Adapter System**: Runtime switching between different hardware adapters (PIR sensors, LEDs, serial communication, etc.)
- **Master-Node Architecture**: Centralized control through a master node with automatic peer discovery
- **EEPROM Persistence**: Configuration and peer information survives device restarts
- **Health Monitoring**: Real-time status reporting for all nodes in the mesh
- **Serial Communication**: Server integration via serial interface with Protobuf message encoding
- **Remote Configuration**: Change adapter types and settings across the mesh without physical access
- **Automatic Pin Management**: Pin assignments automatically inferred based on adapter type
- **Modular Architecture**: Clean separation of concerns with utility classes for common functionality
- **Enhanced Maintainability**: Refactored codebase following SOLID principles for better code organization

## 🏗️ Architecture

### Core Components

- **Mesh Network**: Handles peer discovery, routing, and message broadcasting
- **Adapter System**: Abstract interface for different hardware types with factory pattern
- **Serial Interface**: Server communication with structured message handling
- **Persistence Layer**: EEPROM-based configuration and peer storage
- **EEPROM Manager**: Centralized EEPROM operations following SOLID principles
- **Message Router**: Intelligent message routing and path calculation
- **Peer Manager**: Centralized peer discovery, monitoring, and management
- **Protobuf Codec**: Reusable protobuf encoding/decoding for all adapters
- **ErrorCore**: Centralized error signaling and seven-segment display integration
- **Configuration Manager**: Centralized configuration validation and management
- **Network Manager**: WiFi and MAC address management utilities

### Adapter Types

- **PIR_Adapter**: Motion detection sensor interface
- **Serial_Adapter**: Server communication and mesh control
- **LED_Adapter**: LED control and status indication
- **WIFI_Adapter**: WiFi connectivity management
- **Temp_Adapter**: Temperature sensor interface (example implementation)

### Design Principles

The project follows **SOLID principles** and **DRY (Don't Repeat Yourself)**:

- **Single Responsibility**: Each class has one clear purpose
- **Open/Closed**: Easy to extend with new adapters without modifying existing code
- **Liskov Substitution**: All adapters can be used interchangeably
- **Interface Segregation**: Clean interfaces for different concerns
- **Dependency Inversion**: High-level modules don't depend on low-level modules
- **DRY**: Common functionality centralized in utility classes

### 🐅 Tiger Style Engineering
Planetopia embraces the [Tiger Style](https://tigerstyle.dev/) philosophy—prioritising safety, performance and developer experience.

* **Safety** – static memory allocation after `setup()`, exhaustive assertions and centralised error handling via `error/ErrorCore`.
* **Performance** – predictable control-flow, explicit bounds and zero dynamic allocation inside critical loops.
* **Developer experience** – strict style gates (`clang-format`, `cppcheck`, `-Werror`) and a zero-technical-debt policy enforced in CI.

### Refactored Architecture

The codebase has been extensively refactored to improve maintainability and extensibility:

- **MessageRouter**: Separates routing logic from mesh communication
- **PeerManager**: Centralizes all peer-related operations
- **ProtobufCodec**: Makes protobuf handling reusable across adapters
- **ErrorCore**: Consolidates error signaling and safe reboot logic
- **ConfigurationManager**: Provides consistent configuration validation
- **NetworkManager**: Handles network operations and MAC address management

This refactoring follows the **Single Responsibility Principle** by ensuring each class handles only one concern, making the code easier to test, maintain, and extend.

## 📋 Requirements

- **Hardware**: ESP32-WROOM-DA Module (or compatible ESP32 board)
- **Arduino IDE**: Version 1.8.x or later
- **ESP32 Board Package**: ESP32 Arduino core
- **Dependencies**: ESP-IDF components (WiFi, ESP-NOW, EEPROM)

## ⚙️ Project Configuration (project_config.h)

Key compile-time constants you may want to tweak:

| Constant | Purpose | Typical value |
|----------|---------|---------------|
| `DEV_MODE` | Skip EEPROM writes during development | `true` while testing |
| `DEFAULT_ADAPTER` | Adapter the firmware instantiates when EEPROM is blank or in DEV mode | `PIR_ADAPTER`, change to any enum in `Adapter::adapter_types` |
| `ENABLE_SEVSEG_DISPLAY` | Compile the TM1637 7-segment driver | `true` for production hardware, `false` if you do not have the display |
| `WIFI_CHANNEL` | Wi-Fi / ESP-NOW channel (1-13) – **must match on every node** | `1` |
| `DEFAULT_MESH_KEY` | 16-byte AES key – network-wide credential **always used** | edit for your install |
| `DEFAULT_PEERS` | Bootstrap MAC list written to EEPROM on first boot (or used directly in DEV mode) | include at least the master MAC |
| `MASTER_BEACON_INTERVAL_MS` | How often the master broadcasts its beacon | `2000` |

All nodes must be compiled with identical `WIFI_CHANNEL` and `DEFAULT_MESH_KEY`.

## 📁 Project Structure

```
main/
├── main.ino                 # Main application entry point
├── src/
│   ├── Mesh/               # Mesh networking implementation
│   │   ├── Mesh.h         # Mesh protocol and communication
│   │   └── Mesh.cpp       # Mesh implementation
│   ├── Adapter/           # Hardware adapter system
│   │   ├── Adapter.h      # Abstract adapter base class
│   │   ├── AdapterFactory.h/cpp  # Adapter creation and management
│   │   ├── PIR_Adapter/   # Motion sensor adapter
│   │   ├── Serial_Adapter/ # Serial communication adapter
│   │   ├── LED_Adapter/   # LED control adapter
│   │   └── Temp_Adapter/  # Temperature sensor adapter
│   ├── hardware/          # Hardware abstraction layer
│   │   ├── input/         # Input device abstractions
│   │   └── output/        # Output device abstractions
│   ├── core/              # Core utilities (logging, common helpers)
│   │   └── Logger.h/cpp    # Logging system
│   ├── error/             # Error handling subsystem
│   │   ├── ErrorCore.h/cpp # Centralized error signaling
│   │   └── ErrorCodes.h    # Numeric error registry
│   ├── persistence/       # EEPROM persistence utilities
│   │   └── EEPROM_Manager.h/cpp    # EEPROM operations
│   └── network/           # Network helpers (MacAddress)
```

## 🔧 Setup

### 1. Hardware Configuration

Connect your hardware according to the adapter type:

```cpp
// Default pin assignments (defined in AdapterFactory)
constexpr int PIR_ADAPTER_PIN = 27;
constexpr int LED_ADAPTER_PIN = 26;
constexpr int SERIAL_ADAPTER_PIN = -1;  // No pin needed
constexpr int TEMP_ADAPTER_PIN = 25;

// Button pins
constexpr int CONFIG_BUTTON_PIN = 32;   // Toggle master/node role
constexpr int RESET_BUTTON_PIN = 35;    // Clear EEPROM (5-second hold)
```

### 2. Software Setup

1. **Clone the repository**:
   ```bash
   git clone <repository-url>
   cd planetopia
   ```

2. **Open in Arduino IDE**:
   - Open `main/main.ino` in Arduino IDE
   - Select **“ESP32 WROOM-DA Module”** in the Arduino **Tools → Board** menu and choose the correct serial port
   - Install required libraries if prompted

3. **Configure MAC addresses**:
   - Update `defaultPeerList` in `main.ino` with your device MACs
   - Include the master node's MAC address

4. **Set master node**:
   - Configure one device as master by setting `isMaster` flag
   - Or use the config button (hold for 5 seconds)

5. **Configure development mode**:
   - Set `DEV_MODE = true` for development (no EEPROM storage)
   - Set `DEV_MODE = false` for production (EEPROM persistence)

### 3. Compilation

```bash
arduino-cli compile --fqbn esp32:esp32:esp32da main
```

## 🚀 Usage

### Basic Operation

1. **Power on devices**: All nodes automatically join the mesh
2. **Master beaconing**: Master node broadcasts presence every 2 seconds
3. **Peer discovery**: Nodes automatically discover and maintain peer list
4. **Adapter operation**: Each node operates according to its configured adapter

### Buttons and Development Mode

#### Config Button (Pin 32)
- **5-second hold**: Toggle between master and node roles
- **Production mode**: Saves role to EEPROM and restarts
- **Development mode**: Shows warning (no EEPROM storage)

#### Reset Button (Pin 35)
- **5-second hold**: Clears all EEPROM memory
- **Visual feedback**: Both LEDs blink rapidly
- **Automatic restart**: Device restarts after 3 seconds

#### Development Mode
- **Compile-time flag**: Set `DEV_MODE = true` in `main.ino`
- **No EEPROM storage**: Adapter types and configurations not persisted
- **Default behavior**: Always creates PIR adapter with default pin
- **Testing friendly**: Perfect for development and testing scenarios

#### Production Mode
- **EEPROM persistence**: All configurations saved to non-volatile memory
- **Runtime configuration**: Adapter types can be changed remotely
- **State recovery**: Device remembers settings after power cycles

### Serial Communication (Server Integration)

The master node communicates with your server via serial (115200 baud):

- **Message framing**: 2-byte length prefix + Protobuf payload
- **Configuration**: Set adapter types remotely via `OP_CONFIG_SET`
- **Health monitoring**: Request status reports via `OP_HEALTH_REQ`
- **Data forwarding**: Route messages to specific nodes or broadcast to all

### Message Types

- **Targeted messages**: Sent to specific MAC address
- **Broadcast messages**: Sent to all nodes in mesh
- **Control messages**: Configuration and health check operations

## 🔌 Server Integration

For detailed server implementation requirements, see [`server_requirements.md`](server_requirements.md).

### Key Server Responsibilities

- **Serial communication**: Read/write framed messages
- **Protobuf handling**: Parse and construct `MeshMessage` objects
- **Configuration management**: Send adapter type changes
- **Health monitoring**: Request and process status reports
- **Message routing**: Specify target MAC addresses for directed communication

### Go Implementation Example

```go
// Send configuration change
func sendConfigSet(serial *serial.Port, targetMAC [6]byte, adapterType uint8) error {
    msg := &MeshMessage{
        MessageType:        MESH_TYPE_ADAPTER_DATA,
        DataType:          SERIAL_ADAPTER,
        TargetMacAddress:  targetMAC[:],
        Data:              []byte{OP_CONFIG_SET, adapterType},
    }
    return sendFramedMessage(serial, msg)
}
```

## 🛠️ Development

### Adding New Adapters

For detailed development guide, see [`adapter_development_guide.md`](adapter_development_guide.md).

#### Quick Steps:

1. **Create adapter files**:
   - `main/src/Adapter/NewAdapter/NewAdapter.h`
   - `main/src/Adapter/NewAdapter/NewAdapter.cpp`

2. **Update enums**:
   - Add to `adapter_types` in `Adapter.h`
   - Add to `AdapterFactory::createAdapter()`

3. **Define default pin**:
   - Add constant in `AdapterFactory.h`
   - Implement pin logic in `getDefaultPinForAdapter()`

4. **Implement interface**:
   - Inherit from `Adapter` base class
   - Implement `init()`, `loop()`, and `onMeshDataImpl()`

### Changing Default Adapter

```cpp
// Method 1: Code change
void setup() {
    // Change default adapter type
    planetopia::adapter::AdapterFactory::initializeDefaultsIfUnset();
}

// Method 2: Serial command
// Send OP_CONFIG_SET via serial interface

// Method 3: EEPROM clear
// Clear EEPROM and restart device
```

## 🔧 EEPROM Management

### EEPROM_Manager Utility Class

The project uses a centralized `EEPROM_Manager` utility class that follows **SOLID principles** and eliminates code duplication:

#### **Benefits:**
- **Single Responsibility**: All EEPROM operations in one place
- **DRY Principle**: No repeated EEPROM.begin/end/commit calls
- **Dev Mode Support**: Automatic bypass of EEPROM operations in development
- **Error Handling**: Centralized error management for memory operations
- **Address Management**: All EEPROM addresses defined in one location
- **Type Safety**: Strong typing for different data types

#### **Usage Example:**
```cpp
// Initialize the manager
EEPROM_Manager::getInstance().init();

// Set dev mode (bypasses EEPROM operations)
EEPROM_Manager::getInstance().setDevMode(true);

// Save/load data
EEPROM_Manager::getInstance().saveMasterFlag(true);
bool isMaster = EEPROM_Manager::getInstance().loadMasterFlag();

// Clear operations
EEPROM_Manager::getInstance().clearAll();
```

#### **Centralized Constants:**
```cpp
namespace EEPROM_ADDRESSES {
  constexpr int MASTER_FLAG = 0;      // Master flag
  constexpr int DEV_FLAG = 1;         // Dev mode flag
  constexpr int MESH_KEY = 16;        // Mesh encryption key
  constexpr int PEER_LIST = 32;       // Peer MAC addresses
  constexpr int ADAPTER_TYPE = 8;     // Adapter type
}
```

## 📊 Monitoring and Debugging

### Health Reports

Nodes automatically report their status:
- Adapter type
- MAC address
- Uptime
- Connection status

### Logging

Comprehensive logging system with multiple levels:
- **DEBUG**: Detailed operation information
- **INFO**: General status updates
- **WARN**: Non-critical issues
- **ERROR**: Critical problems

### Error Handling

Centralized error management with specific error types:
- `COMMUNICATION_FAIL`: Network or serial issues
- `CONFIG_ERROR`: Configuration problems
- `HARDWARE_ERROR`: Hardware failures

## 🔒 Security

- **Mesh encryption**: 16-byte encryption key stored in EEPROM
- **Peer validation**: MAC address verification for all communications
- **Access control**: Master node controls configuration changes

## 📁 Project Structure

```
planetopia/
├── main/
│   ├── main.ino                 # Main application entry point
│   └── src/
│       ├── Adapter/             # Adapter system
│       │   ├── Adapter.h        # Base adapter class
│       │   ├── AdapterFactory.h # Adapter creation and management
│       │   ├── PIR_Adapter/     # Motion sensor adapter
│       │   ├── Serial_Adapter/  # Server communication adapter
│       │   └── ...
│       ├── Mesh/                # Mesh networking
│       │   ├── Mesh.h          # Mesh protocol and routing
│       │   └── Mesh.cpp        # Mesh implementation
│       ├── hardware/            # Hardware abstractions
│       │   ├── input/          # Input devices (buttons, sensors)
│       │   └── output/         # Output devices (LEDs, displays)
│       ├── core/               # Core utilities
│       │   └── Logger.h         # Logging system
│       ├── error/              # Error handling subsystem
│       │   └── ErrorCore.h      # Centralized error signaling
│       └── persistence/        # EEPROM_Manager utility
├── server_requirements.md       # Server implementation guide
├── adapter_development_guide.md # Adapter development guide
└── README.md                    # This file
```

## 🤝 Contributing

1. Fork the repository
2. Create a feature branch
3. Implement your changes
4. Add appropriate tests and documentation
5. Submit a pull request

## 📝 License

[Add your license information here]

## 🆘 Support

For issues and questions:
- Check the documentation files
- Review the code examples
- Open an issue on the repository

## 🔄 Version History

- **v1.0.0**: Initial release with ESP-NOW mesh networking
- **v1.1.0**: Added dynamic adapter system and serial communication
- **v1.2.0**: Implemented health monitoring and remote configuration
- **v1.3.0**: Added Protobuf support and simplified server protocol

---

**Note**: This project is designed for ESP32 devices and requires the ESP32 Arduino core. Make sure your development environment is properly configured before attempting to compile or upload the code.

## ⚙️ Configuration file

All user-tunable settings now live in `project_config.h` at repo root.  
Key fields:
* `DEV_MODE` – true = development build (no EEPROM writes)  
* `DEFAULT_DEV_MASTER` – startup role when in DEV_MODE  
* Pin mapping constants (`RED_LED_PIN`, `SEVSEG_DATA_PIN`, …)  
* `DEFAULT_LOG_LEVEL` – global verbosity  
Edit the header and rebuild – no source changes required.

## 🔢  Seven-segment error codes
The four-digit display shows numeric error codes generated by `ErrorCore`.

Code layout: `T M S` (hex digits)
* **T** – error type (1=Generic,2=Sensor,3=Comm,4=Memory,5=Hardware,6=Config)
* **M** – module (1=Core,2=Adapter,3=Mesh,4=EEPROM,5=Hardware)
* **S** – sub-code (0-F)

Example table:
| Code | Meaning |
|------|---------|
| 652 | Config error in Hardware module – TM1637 no ACK |
| 431 | Memory error in Mesh module – peer overflow |
| 212 | Sensor error in Adapter module – PIR hardware failed to initialise |

See `docs/error_codes.md` for the full registry.

## 📚 Documentation

* [Adapter Development Guide](docs/adapter_development_guide.md)
* [Server Requirements](docs/server_requirements.md)
* [Seven-segment Error Codes](docs/error_codes.md)

---
