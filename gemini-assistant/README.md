# Gemini Live voice assistant (standalone)

Firmware for the bread-compact-wifi wiring (ESP32-S3 N16R8, INMP441, MAX98357A,
SH1106 OLED) that talks directly to the Gemini Live API. No xiaozhi server and
no self-hosted server.

- Build: GitHub Actions workflow `Gemini firmware` (push to the `gemini` branch).
  Download the `gemini-firmware` artifact and flash `gemini-merged-binary.bin` at `0x0`.
- First boot opens a Wi-Fi hotspot `Gemini-Setup`; join it, open 192.168.4.1,
  pick your Wi-Fi and paste the Gemini API key. The key is stored only on the device.
- To change settings later: press the button during the first 3 seconds after power-on.
- Hold the talk button (GPIO47) or BOOT to speak; release to hear the answer.

`lib/WebSockets` is arduinoWebSockets 2.6.1 (LGPL-2.1) with two small local patches:
`WEBSOCKETS_MAX_DATA_SIZE` can be overridden, and a weak `websocketsOnClose` hook
reports the server's close reason.
