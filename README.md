# Distributed Modular Robotic Arm System 🦾

A self-organizing, hot-swappable robotic arm system based on **ESP32** and **ESP32-C3** microcontrollers. This project moves away from centralized control, implementing a **distributed tree-topology architecture** where each joint module is intelligent and aware of its position in the chain.

## 🚀 Key Features (Highlights)

* **Automatic Topology Discovery:** Modules automatically detect their downstream neighbors and assign IDs dynamically upon boot (Plug & Play).
* **Distributed Control Architecture:**
    * **Master (ESP32):** Handles WiFi/WebSocket communication with the App and coordinates the system.
    * **Slaves (ESP32-C3):** Each joint runs its own motion planning firmware.
* **Look-Ahead Motion Planning:** Implemented a Look-Ahead algorithm (60ms buffer) on ESP32-C3 to ensure smooth, linear motion and eliminate jitter during multi-joint synchronization.
* **Hot-Swap Safety:** Hardware-level detection (`DETECT_PIN`) ensures the system safely handles module attachment/detachment in real-time.
* **Dual-Channel Communication:** Uses UART for reliable daisy-chain control and **ESP-NOW** as a wireless failover/debug channel.

## 🛠️ Tech Stack

* **MCU:** ESP32 (Master), ESP32-C3 (Slave Joints)
* **Communication:** UART (Daisy-chain), ESP-NOW, WebSockets
* **Actuators:** Bus Servos (LX-224) / Coreless Motors
* **Design:** SolidWorks (Mechanical Parts), 3D Printing

## 📂 File Structure

* `Master_Controller_ESP32.ino`: Main firmware for the host controller. Handles topology scanning and App instructions.
* `Slave_Joint_C3.ino`: Firmware for joint modules. Handles motion interpolation and ID assignment.
