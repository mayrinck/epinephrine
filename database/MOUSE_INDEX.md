# Mice / Mouse Index

This file maps detected USB mice/mouse names to their official product pages and supported features.

To add a new entry, insert a row in the table below with the mouse name as it should appear in the application, its USB VID:PID (hex), the URL to its official product page, the number of buttons, and optionally whether it is a white-label product.

## Table

| Mice/Mouse Name | VID:PID | Official Product Page | Buttons | White-Label | Manufacturer | PO | Release Year |
|---|---|---|---|---|---|
| Logitech Gaming G300 | 046d:c24a | https://www.logitech.com/assets/47559/4/gaming-mouse-g300-quick-start-guide.pdf | 9 | | Logitech | 2011 |
| Logitech Gaming G300s | 046d:c246 | http://www.logitech.com/assets/53416/3/g300s621-000381.pdf | 9 | | Logitech | 2015 |
| AJAZZ AJ series / AJ179 (shared XCTECH receiver) | 249a:5c2f | | | | | | |
| AJAZZ AJ series (2.4 GHz receiver; shared models) | 248a:5c2f | | | | AJAZZ | | |
| AJAZZ AJ series (USB; shared models) | 248a:5c2e | | | | AJAZZ | | |
| AJAZZ AJ series (USB mode 5d2e; shared models) | 248a:5d2e | | | | AJAZZ | | |
| AJAZZ AJ series (USB mode 5e2e; shared models) | 248a:5e2e | | | | AJAZZ | | |
| AJAZZ AJ159 NL (USB) | 7768:6d26 | | | | AJAZZ | | |
| AJAZZ NearLink receiver (AJ159 NL and others) | 7768:6426 | | | | AJAZZ | | |
| Warrior Gamer IVOR [MO261] | 18f8:0f99 | https://suporte.grupomulti.com.br/produtos/warrior-ivor-mouse-gamer-3200dpi-preto-mo261 | 7 | Sim (white-label de Maxxter Optical gaming mouse) | Multilaser | PAD39 | 2018 |
| Multilaser Gamer Black/Orange [MO274] | 1bcf:0005 | https://suporte.grupomulti.com.br/produtos/combo-mouse-3200dpi-mousepad-laranja-mo274 | 6 | Sim (white-label de Sunplus Innovation Technology Inc. Optical Mouse) | Multilaser | NWT10 | 2020 |


IDs may be shared by different brands or models. Receiver entries identify the family, not a specific mouse. Sources for AJ-series IDs: https://github.com/Aiacos/ajazz-control-center/blob/develop/docs/_data/devices.yaml and https://github.com/Rockeyxx/aj179-linux. Identification does not imply hardware-configuration support.

## Contributing

If your device is not listed, look up its USB VID:PID with `lsusb`, find its official product page, and submit a pull request adding the row to the table above.
