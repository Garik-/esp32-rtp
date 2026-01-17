# ESP32S3 UDP RTP Streaming

use Seeed Studio XIAO ESP32S3 Sense

goals:
- [ ] попробовать подключить esp-camera что просто в лог будет выводиться что камера работает и fps
- [ ] подключить wifi с настройкой через Kconfig - можно наверное сразу с mDNS взять это дело можно из примера с httpd_poc
- [ ] простой UDP сервер сделать на esp32 и клиента тупого написать с hello world посмотреть что это работает
- [ ] замутить этот RTP JPEG протокол и позырить как работает эта хрень в VLC
- [ ] замерить сколько эта скатина жрет 
- [ ] попробовать оптимизировать код и выставить правильные заголовки RTP
- [ ] передавать звук https://wiki.seeedstudio.com/xiao_esp32s3_sense_mic/


stradm.dsp
```
v=0
o=- 0 0 IN IP4 127.0.0.1
s=RTP JPEG Stream
c=IN IP4 127.0.0.1
t=0 0
m=video 5004 RTP/AVP 26
a=rtpmap:26 JPEG/90000
```

## examples

https://wiki.seeedstudio.com/xiao_esp32s3_camera_usage/#project-ii-video-streaming


https://github.com/espressif/arduino-esp32/blob/master/libraries/ESP32/examples/Camera/CameraWebServer/app_httpd.cpp

run_face_recognition  
jpg_encode_stream  
capture_handler  
stream_handler 


