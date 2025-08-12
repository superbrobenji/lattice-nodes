# Error Code Reference (Seven-Segment Display)

The display always shows a 4-digit number **T M SS** where:

| Position | Name       | Description                                     |
|----------|------------|-------------------------------------------------|
| `T`      | Type       | 0=OK, 1=Hardware, 2=Comm, 3=Memory, 4=Config, 5=Logic |
| `M`      | Module     | 0=Core, 1=Adapter, 2=Mesh, 3=Persistence, 4=Network   |
| `SS`     | Subsystem  | 00-99 specific to the module                      |

---
## 1 × ××  Hardware errors
| Code | Location                | Meaning                                   |
|------|-------------------------|-------------------------------------------|
| 1101 | Core                    | Brown-out or unexpected reset             |
| 1201 | Adapter – PIR           | PIR sensor failed to init                |
| 1202 | Adapter – Serial        | Serial adapter buffer overflow           |
| 1301 | Persistence             | EEPROM failed to begin                   |

## 2 × ××  Communication errors
… *(extend as you add codes)* …

## How to add a new code
```cpp
// Example: mesh peer list overflow
constexpr uint8_t MESH_PEER_OVERFLOW = 1;   // SS part
ErrorHandler::getInstance().signalError(
        core::ErrorTypeDigit::MEMORY,
        core::ModuleDigit::MESH,
        MESH_PEER_OVERFLOW,
        "Peer vector overflow");
```
The display will show **3201**.

| Code | Meaning |
|------|---------|
| 1201 | Hardware error in adapter – TM1637 ACK timeout (SevenSegDisplay) |
