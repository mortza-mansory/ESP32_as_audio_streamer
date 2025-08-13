# ESP32_as_audio_streamer
My bluetooth  moudle is broke so i using my esp32 as bluetooth( A2DP Protocol ) streamer so at lasted i can hear music while coding!

Status: Working ( done )

The logic:
PC: Your computer sends the audio file to the ESP32 over Wi-Fi.
ESP32: Receives the audio data from Wi-Fi and streams it to the headphones using Bluetooth (A2DP protocol).
Headphones: Receive and play the audio signal from the ESP32.


Usage:
Use only esp32-idf for build and flash to the eps32, use menuconfig for turning on bluetooth and for blutoth options use classic mode for searching you headphones, after connect to your wifi modem( or hotspot mobile ) and connect your pc and headphones both in same network; After you should use the GUI release( https://github.com/mortza-mansory/ESP32_as_audio_streamer_gui/blob/main/README.md ) and choice a WAV format audio to stream and listen.


Issus:
Too many latency ( ITS LIKE YOUR NETWORK IS HAVING 500 MS PING! ).
Only WAV's audio format is supported.


Creator: morteza mansory.
Contact me at( Telegram ): @dashclss 
