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

## Почему нельзя просто собрать исходники ALSA

Эти файлы используют Linux PCI, MMIO, IRQ, DMA, таймеры, firmware loader,
AC'97, PCM, mixer и MIDI API. В macOS им соответствуют другие интерфейсы IOKit
и IOAudioFamily; Linux-заголовки, `snd_pcm_*`, `request_firmware`, `readl/writel`,
Linux locks и callbacks не являются переносимыми API.

Рекомендуемая граница портирования:

1. Сначала определить структуру PCI-функции и resource BAR на реальном Mac.
   Отобразить MMIO через IOKit; не обращаться к регистрам, пока длина BAR и
   его тип не проверены.
2. Реализовать endian-safe доступ к регистрам и таймауты на основе IOKit.
   Сохранить смещения из `ymfpci.h` в отдельном заголовке без Linux-зависимостей.
3. Воспроизвести последовательность аппаратного reset/инициализации из
   `ymfpci_main.c`; проверить ожидаемые статусы и PCI revision до запуска DSP.
4. Включать DSP firmware в бинарный kext и проверять размер каждого образа.
   Не считать наличие файла достаточным доказательством совместимости содержимого.
   Не вводить чтение файлов из ядра или зависимость от private OSKext API.
5. Реализовать прерывания и DMA с IOKit memory descriptors и DMA command
   abstractions, подходящими для PCI-карты и ограничения адресации YMF754.
   Проверить адреса, выравнивание, кольцевые буферы и остановку DMA.
6. Опубликовать `IOAudioEngine`/потоки для воспроизведения и захвата и связать
   callbacks формата/запуска/остановки с аппаратными голосами YMF754.
7. Перенести управление AC'97 и mixer controls; физический микрофон, чувствительность,
   master volume и mute подтверждены пользователем.
8. Отдельный S/PDIF PCM endpoint следует ALSA IEC958 playback: `SPDIFOUTCTRL` bit 1,
   `MODE` bit 30, включение второго codec через `SECCONFIG`, swapped EFF2/EFF3 gains
   и consumer PCM status `0x8200`. Аналоговый и цифровой endpoints используют общий
   DSP и поэтому запускаются по одному; физический S/PDIF-сигнал ещё нужно проверить.
9. Проверить sleep/wake, выгрузку, остановку engine и обработку ошибок.

`IOAudioFamily` использует подклассы `IOAudioDevice` и `IOAudioEngine`.
Движок публикуется только после подготовки DMA, IRQ и аналогового AC'97.
Собственный `start()` проверяет готовность: в legacy IOAudioFamily встречаются
версии, где возврат `initHardware()` теряется внутри базового `start()`.
Факт регистрации всё равно не заменяет аппаратную проверку звука.

## Как читать исходники-референсы

- `ymfpci.c`: PCI IDs, обнаружение, создание/регистрация карты, связь подсистем.
- `ymfpci_main.c`: регистры, кодек, аппаратные голоса, PCM, прерывания,
  mixer/SPDIF controls и lifecycle.
- `ymfpci.h`: карта регистров, структуры состояния и объявления ALSA.

При переносе держать Linux-совместимую логику отдельными небольшими функциями,
а IOKit lifecycle и IOAudioFamily callbacks — в собственных классах. Не переносить
Linux-структуры целиком как замену macOS audio engine.

## Реализованная граница

`YMFPCIHardware.cpp` переносит последовательности `snd_ymfpci_aclink_reset`,
`snd_ymfpci_codec_ready`, `snd_ymfpci_disable_dsp` и запись образов из
`snd_ymfpci_download_image`. Вместо Linux API используются callbacks транспорта;
адаптер в `YMFPCIAudioDevice.cpp` предоставляет IOKit PCI config access и MMIO.
Все шесть macOS translation units проверены против Kernel SDK 10.6 через Clang
15.0.7: syntax-only и Mach-O x86_64 object compilation с предупреждениями как
ошибками. Повторяемая проверка: `Tools/check_sdk.sh`; нативная линковка и
проверка загрузки нового PCM-варианта всё ещё нужны.

`YMFPCIPCM` переносит ABI playback bank, dual-voice stereo и AC'97 команды.
Размеры банков определяет firmware, stride не подменяется `sizeof` структуры.
`YMFPCIDMA` выделяет две низкоадресные contiguous области и отображает их через
`IODMACommand`; проверяет один полный 32-битный bus segment, выравнивание и длину.
Неподходящее отображение — ошибка, не усечение адреса и не фиктивный fallback.

Два `IOAudioEngine` предоставляют аналоговый и цифровой S16LE stereo output /
48 kHz endpoints. Они имеют отдельные DMA-области, но используют общие регистры
DSP, поэтому новый endpoint останавливает уже активный перед настройкой своего
маршрута. Аналоговый engine дополнительно публикует микрофонный input; цифровой
engine входной поток не публикует, что соответствует требованию пользователя.
Последовательность:

1. При выключенном PCI bus mastering и MODE=0 кратко включить CONFIG для чтения
   размеров банков; снова остановить DSP.
2. Выделить таблицу 64 голосов, все двойные банки playback/capture/effect,
   work area и PCM ring; установить shared IRQ filter.
3. Настроить PCI I/O decode только при валидных назначенных BAR1/BAR2,
   сохраняя прежнее состояние для teardown; затем настроить AC'97 front DAC
   (48 kHz, начальный master −22.5 dB) и заглушить analog monitoring.
4. Записать проверенные DMA bases, отключить capture/effect/SPDIF/timer,
   включить DSP с MODE=0, опубликовать выходной поток.
5. При PCM start: заново подготовить нулевые банки/таблицу при остановленном DSP,
   заполнить два S16 stereo голоса, установить начальный timestamp, включить
   bus mastering, запустить MODE=3.
6. IRQ filter проверяет STATUS bit31, читает CTRLSELECT/start активного банка,
   обновляет timestamp на wrap, подтверждает STATUS и переактивирует MODE bit1.
   Spin lock синхронизирует регистры и состояние с stop; никаких аллокаций,
   ожиданий или IOLog в primary interrupt context. Диагностика — на workloop.
7. Обычный PCM stop выключает IRQ, глушит выход, очищает MODE и ждёт
   `STATUS.busy=0` до 20 ms при работающем CONFIG/DSP. Только затем выключает
   bus mastering и очищает указатели голосов; CONFIG остаётся включённым для
   повторного открытия. Full device teardown после того же quiesce останавливает
   CONFIG/DSP отдельно. Если DSP завис, bus mastering выключается, а движок
   запрещает повторный старт. Перед освобождением MMIO unregister IRQ; DMA
   descriptors живут до освобождения engine.
8. Sleep безопасно quiesce-ит PCM и выключает bus mastering. Wake заново загружает
   firmware, проверяет неизменность размеров банков и восстанавливает тракт;
   при ошибке остаётся выключенным. Аппаратно sleep/wake пока не проверен.

S/PDIF PCM не дублирует front output и не является raw AC-3 passthrough. Поток
подаётся через отдельные rear/effect gains согласно `snd_ymfpci_playback_spdif_open`;
при закрытии цифрового endpoint снимаются S/PDIF/secondary-codec enable bits.

Пользователь подтвердил аналоговый playback, микрофон, регуляторы громкости, mute
и отдельный PCM S/PDIF endpoint на физической карте. Микрофон нужен только при
аналоговом выходе. Для SigmaTel STAC9708 рабочий исходник пользователя записывает
`0x0f0f` в AC'97 `3D Control` (`0x22`) и включает бит `0x2000` в
`General Purpose` (`0x20`), сохраняя остальные биты `0x20`. Слышимый эффект в
каждом варианте драйвера следует подтвердить отдельно. Однако стандартная панель Sound в Mac OS X
10.6 не показывает произвольные IOAudioToggleControl, поэтому флажка в ней нет;
для переключения добавлена отдельная программа `ControlApp/`. Её сборка в Xcode
4.2 и проверка на карте ещё нужны. Эффект на слух и независимость от S/PDIF тоже
нужно проверить. Raw AC-3 passthrough, SPDIF capture, MIDI, другие форматы и
sleep/wake остаются вне текущей реализации.

Образы `ds1_dsp.fw` и `ds1e_ctrl.fw` встраиваются генератором Python 2/3
при сборке. Образы DS-1 и `yss225_registers.bin` не используются для YMF754
в перенесённой последовательности. Авторство/лицензии ALSA и firmware различать.

## Источники API и последовательности

- Вложенный пользователем ALSA-референс: initialization routines в `ymfpci_main.c`.
- Apple, Audio Device Driver Programming Guide:
  https://leopard-adc.pepas.com/documentation/DeviceDrivers/Conceptual/WritingAudioDrivers/ImplementDriver/ImplementDriver.html
- Публичный заголовок SDK 10.6 `IOPCIDevice.h` (зеркало):
  https://github.com/phracker/MacOSX-SDKs/blob/master/MacOSX10.6.sdk/System/Library/Frameworks/Kernel.framework/Versions/A/Headers/IOKit/pci/IOPCIDevice.h
- Исходник legacy IOAudioFamily (lifecycle, timestamp):
  https://github.com/apple-oss-distributions/IOAudioFamily/tree/IOAudioFamily-179.4.4
- XNU Snow Leopard KPI exports (`kernel_task` требует `com.apple.kpi.mach`):
  https://github.com/apple-oss-distributions/xnu/blob/xnu-1504.15.3/config/Mach.exports
