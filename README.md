# ESP32S3 UDP RTP Streaming

use Seeed Studio XIAO ESP32S3 Sense

goals:
- [x] попробовать подключить esp-camera что просто в лог будет выводиться что камера работает и fps
- [x] подключить wifi с настройкой через Kconfig - можно наверное сразу с mDNS взять это дело можно из примера с httpd_poc
- [x] простой UDP сервер сделать на esp32 и клиента тупого написать с hello world посмотреть что это работает
- [x] замутить этот RTP JPEG протокол и позырить как работает эта хрень в VLC
- [ ] замерить сколько эта скатина жрет 
- [x] выставить правильные заголовки RTP
- [ ] посмотри про restart markers (RSTM)
- [x] передавать звук https://wiki.seeedstudio.com/xiao_esp32s3_sense_mic/
- [ ] оптимизировать код
- [ ] реализовать переподключение при обрыве wifi соединения с бекоф линейным
- [ ] через UART CLI сделать управление качеством видео


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


## оптимизации компилятора


```
IRAM_ATTR
DRAM_ATTR
__attribute__((packed))
ESP_LOGI(TAG, "value=%d", x);
const
inline
extern
```


### Когда нужен volatile
```c
volatile uint32_t *UART_STATUS = (uint32_t *)0x3FF40000;
```
Значение может измениться без участия кода
- регистры
- DMA
- ISR

Компилятор:
- не кэширует
- не оптимизирует чтения/записи

### указатели и const
```c
const int *p;   // нельзя менять *p
int * const p;  // нельзя менять p
const int * const p; // нельзя ни то ни другое
```

### Что такое likely / unlikely

Это подсказка компилятору:
какая ветка условия чаще всего выполняется.

Типичное определение (ESP-IDF, Linux):
```c
#define likely(x)   __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
```
- Используется GCC / Clang
- не меняет логику, только генерацию кода

#### Где СТОИТ использовать
Проверки ошибок
```c
if (unlikely(ptr == NULL)) return ESP_ERR_NO_MEM;
```

Guard-conditions
```c
if (unlikely(len == 0)) return;
```

В ISR
```c
if (unlikely(status & ERROR_BIT)) {
    handle_error();
}
```

Assert / sanity-check
```c
if (unlikely(i >= MAX)) abort();
```


__attribute__((hot / cold))

```c
__attribute__((hot))  // функция вызывается часто
__attribute__((cold)) // функция вызывается редко
```

Используйте hot, если:

- функция реально в hot path
- профилирование это подтверждает
- код небольшой

Используйте cold, если:

- ошибки, rare paths
- логирование
- аварийные выходы

Когда НЕ стоит

- “на всякий случай”
- без понимания профиля
- помечать большие функции как hot


__attribute__((packed))
__attribute__((aligned(N))) // DMA, cache, SIMD


Используйте restrict, если выполнены ВСЕ условия:
- Вы контролируете все вызовы функции
- Указатели логически не могут алиаситься
- Код находится на критическом пути
- Вы измерили или ожидаете выигрыш

Алиасинг (aliasing) — это ситуация, когда два или больше разных выражения в программе обращаются к одной и той же области памяти. Если не обращаются то можно сделать restrict


используется чаще всего в порядке убывания
- static	
- inline	
- const	
- unlikely	
- restrict	
- атрибуты GCC	
- branchless	
- LTO	


---

# **ESP32 Memory & Attributes Cheat Sheet**

```
            ┌───────────────────────────┐
            │           Flash           │
            │  - Большая (мегабайты)   │
            │  - Медленная              │
            │  - Код по умолчанию       │
            │  - const lookup tables    │
            └─────────────┬────────────┘
                          │
        ┌─────────────────┴─────────────────┐
        │                                   │
    ┌───▼───┐                           ┌───▼───┐
    │ IRAM  │                           │ DRAM  │
    │ Instr │                           │ Data  │
    │ 128-256KB                        │ ~320KB│
    │ Быстрое исполнение кода          │ Быстрый доступ к данным
    │ Для ISR / fast-path              │ Для DMA, аппаратных структур
    └─────┬────┘                       └─────┬─────┘
          │                                 │
  ┌───────▼────────┐               ┌────────▼─────────┐
  │ IRAM_ATTR      │               │ DRAM_ATTR        │
  │ Функции        │               │ Буферы DMA       │
  │ ISR / критический│             │ Структуры периферии │
  │ код / hot-path │               │ Быстрый доступ   │
  └────────────────┘               └─────────────────┘

```


# **Правила использования**

| Атрибут        | Использовать для                                | Примечания                                                    |
| -------------- | ----------------------------------------------- | ------------------------------------------------------------- |
| `IRAM_ATTR`    | ISR, fast-path функции                          | Код исполняется в IRAM, быстрый доступ                        |
| `DRAM_ATTR`    | DMA буферы, структуры для периферии             | Аппарат может напрямую читать/писать                          |
| `const`        | Lookup tables, неизменяемые данные              | Обычно Flash, можно сочетать с IRAM_ATTR для быстрого доступа |
| `static`       | Переменные и функции с областью видимости файла | Приватность, помогает линковщику                              |
| обычный код    | Всё остальное                                   | Лежит в Flash по умолчанию                                    |
| обычные данные | Глобальные и локальные переменные               | Лежат в DRAM                                                  |

# **Примеры**

### ISR и DMA буфер

```c
DRAM_ATTR uint8_t dma_buf[512]; // DMA-safe, читаем аппаратно

void IRAM_ATTR gpio_isr_handler(void* arg) {
    dma_buf[0] = 42; // быстрый доступ, ISR-safe
}
```

### Lookup table в IRAM

```c
IRAM_ATTR const uint16_t sin_table[256] = { ... };
// fast access для алгоритмов
```

### Приватная функция и переменная

```c
static int counter;

static void inc_counter(void) {
    counter++;
}
```

# **Ключевые советы**

1. **IRAM маленькая** → клади туда только горячий код/ISR
2. **DRAM для DMA и аппаратного доступа**
3. **Flash для больших констант** (таблицы, строки)
4. **const + IRAM_ATTR** → быстрый доступ к неизменяемым данным
5. **static** → ограничивает видимость в файле, помогает линковщику


# Проблемы с UDP
мне не удалось пока настроить multicast, нужно знать IP адрес конкретной машины
VLC принимает порт 4000 но не принимает порт 12345


![jpeg](jpeg.png)

```
Что НЕ передаётся
❌ Не передаются стандартные JPEG-маркеры, такие как:
SOI (FFD8)
APP0 / JFIF
DQT (таблицы квантования — обычно)
SOF
SOS
EOI (FFD9)
❌ Не передаётся цельный .jpg файл


[RTP header]
[RTP JPEG header]
[Restart Marker header]   ← если используется
[Quantization Table header]  ← ОДИН
[Quantization Table Data]    ← ВСЕ таблицы подряд
[Entropy-coded JPEG data]

---
Quantization Table header
MBZ        = 0x00
Precision  = 0x00
Length     = 0x0080

[64 bytes table 0]
[64 bytes table 1]
```