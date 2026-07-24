# Keyboard Index

This file maps detected USB keyboard names to their official product pages and supported features.

To add a new entry, insert a row in the table below with the keyboard official name as it should appear in the application, its USB VID:PID (hex), the type of keyset (QWERTY for standard keyboards, use the other types if needed), the URL to its official product page, a comma-separated list of supported features and the connection type.

## Table

| Keyboard Name | VID:PID | Keyset | Official Product Page | Features | Connection |
|---|---|---|---|---|---|
| Defender Gaming Keyboard | 0e6a:02c0 | QWERTY |  |  | USB |
| GEMBIRD Flexible keyboard KB-109F-B-DE | 0e6a:6001 | QWERTY |  |  | USB |
| Truly Ergonomic Computer Keyboard (DFU Mode) | 0e6a:030B | QWERTY |  |  | USB |
| Truly Ergonomic Computer Keyboard | 0e6a:030C | QWERTY |  |  | USB |

## Contributing

If your keyboard is not listed, look up its USB VID:PID with `lsusb` or `dmesg | grep -i "input:"`, find its official product page, and submit a pull request adding the row to the table above.

https://devicehunt.com
