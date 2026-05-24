$env:IDF_PATH = "D:\esp32\Espressif\frameworks\esp-idf-v5.4.4"
$env:PATH = "D:\esp32\Espressif\tools\cmake\3.30.2\bin;D:\esp32\Espressif\tools\ninja\1.12.1;D:\esp32\Espressif\tools\xtensa-esp-elf\esp-14.2.0_20260121\xtensa-esp-elf\bin;" + $env:PATH
Set-Location "D:\esp32\esp32-websocket-tool"
python "D:\esp32\Espressif\frameworks\esp-idf-v5.4.4\tools\idf.py" reconfigure 2>&1
