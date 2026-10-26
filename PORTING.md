# План переноса ALSA → IOAudioFamily

## Данные устройства и референсы

- Карта пользователя: Yamaha YMF754 / DS-1E.
- PCI vendor ID: `0x1073`; device ID: `0x0012`.
- Совмещённый IOKit match: `0x00121073`.
- В `Reference/ALSA/` сохранены предоставленные `ymfpci_main.c`, `ymfpci.c` и
  `ymfpci.h`. В `Reference/Firmware/` сохранены бинарные firmware-файлы из
  архива.
- ALSA-файлы содержат SPDX `GPL-2.0-or-later`. При переносе производного кода
  необходимо сохранять соответствующие заголовки авторства и лицензионные
  условия. Не включать файлы референса в Xcode target механически.

