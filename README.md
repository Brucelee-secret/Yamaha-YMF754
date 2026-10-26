Yamaha YMF754 Audio Driver 

A native x86_64 KEXT audio driver for the Yamaha YMF754 sound card. Built from scratch for low latency and broad compatibility. 

Key Features
Compatible with OS X 10.6.8 Snow Leopard up to Tahoe 26.7
Exclusively compiled for x86_64 architecture
Supports native system volume control 

Installation 

1. Disable SIP on newer macOS versions by setting csr-active-config to 03080000 in your bootloader config.
2. Download the latest release.
3. Install the KEXT using Kext Droplet or your preferred utility.
4. Rebuild the kext cache and reboot.

Compilation
The project is configured for compilation using Xcode 4.2. Clone the repository and open the project file to build from source.

===============================================================================================================
Русский.
Драйвер легендарной звуковой карты Yamaha YMF754
Нативный аудиодрайвер (KEXT) под архитектуру x86_64 для звуковой карты Yamaha YMF754. Написан с нуля для минимальной задержки и стабильной работы.
Главные преимущества
Совместимость с системами от 10.6.8 до Tahoe 26.7
Скомпилирован строго под x86_64
Поддерживает родное системное регулирование громкости
Установка
1. Отключите SIP в современных версиях macOS, прописав значение csr-active-config = 03080000 в конфигурации загрузчика.
2. Скачайте последнюю версию из раздела Releases.
3. Установите KEXT с помощью утилиты Kext Droplet.
4. Обновите кэш кекстов и перезагрузите компьютер.
Компиляция
Проект подготовлен для сборки через Xcode 4.2. Склонируйте репозиторий и откройте файл проекта для компиляции из исходного кода.
Описания получились емкими, профессиональными и полностью закрывают все вопросы хакинтошников по установке и сборке.
Если у вас есть ссылка на репозиторий или вы хотите добавить раздел благодарностей (Credits) другим разработчикам, чьи исходники или документация вам помогли, дайте знать. Я помогу красиво интегрировать их в текст.
