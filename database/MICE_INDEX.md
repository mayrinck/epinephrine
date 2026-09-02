# Mice / Mouse Index

This file maps detected USB mice/mouse names to their official product pages and supported features.

To add a new entry, insert a row in the table below with the mouse name as it should appear in the application, its USB VID:PID (hex), the URL to its official product page, the number of buttons, and optionally whether it is a white-label product.

## Table

| Mice/Mouse Name | VID:PID | Official Product Page | Buttons | White-Label |
|---|---|---|---|---|
| Multilaser Gamer MO274 | 1bcf:0005 | https://suporte.grupomulti.com.br/produtos/combo-mouse-3200dpi-mousepad-laranja-mo274 | 6 | Sim (white-label de Sunplus Innovation Technology Inc. Optical Mouse) |


## Contributing

If your device is not listed, look up its USB VID:PID with `lsusb`, find its official product page, and submit a pull request adding the row to the table above.
