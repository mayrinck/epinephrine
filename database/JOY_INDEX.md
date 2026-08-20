# Joystick / Controller Index

This file maps detected USB joystick/controller names (from evdev) to their official product pages and supported features.

To add a new entry, insert a row in the table below with the controller name as it should appear in the application, its USB VID:PID (hex), the category (Controller for standard gamepad layout, Arcade for arcade stick layout), the URL to its official product page, a comma-separated list of supported features, the connection type, and the number of buttons.

## Table

| Controller Name | VID:PID | Category | Official Product Page | Features | Connection | Buttons |
|---|---|---|---|---|---|---|
| KROM KUMITE | 0079:187c | Arcade | https://www.kromgaming.com/pt/comandos/kumite | 8 Action Buttons, D-Pad, Analog Stick, L3/R3, Share, Home, Options | USB | 14 |
| KNUP KP-CN700 | 045e:028e | Controller | https://knup.com.br/produto/controle-n-switch-ps3-android-pc-cn700/ | 8 Action Buttons, 2 Extra triggers, D-Pad, Dual Analog Stick, LS/RS, Share, Home, Options, Turbo | USB | 16 |
| GameSir T3 Lite | 3537:1093 | Controller | https://gamesir.com/products/gamesir-tegenaria-lite | 8 Action Buttons, 2 Extra Back Buttons (L4/R4), D-Pad, Dual Hall Effect Analog Stick, L3/R3, Share, Home, Options, Turbo | USB | 16 |

## Contributing

If your controller is not listed, look up its USB VID:PID with `lsusb` or `dmesg | grep -i "input:"`, find its official product page, and submit a pull request adding the row to the table above.
