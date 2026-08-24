# Архитектурный анализ Mock Market-Making Engine с точки зрения Ultra-Low Latency (ULL) и план трансформации

**Автор:** Principal Ultra-Low Latency (ULL) Trading Systems Architect  
**Объект анализа:** `/Users/dima/cpp/cpp_assignment` (`cpp/`, `python/mmclient/`, `docs/`)  
**Дата:** 2026-08-25  

---

## 1. Executive Summary & Текущий профиль задержек

Текущий репозиторий представляет собой функционально выверенный, детерминированный прототип распределенной торговой системы:
- **C++20 Mock Engine:** Однопоточный асинхронный event loop на `Boost.Asio` / `Boost.Beast`, строгий sans-IO автомат состояний заявок (`OrderEngine`), асимметричная политика outbox (conflation рыночных данных до глубины 1, undroppable отчеты об исполнении), lock-free SPSC кольцевой буфер телеметрии (`SpscTelemetryRing`).
- **Python MM Client:** Sans-IO стратегия котирования на лучшем уровне книги (`Strategy`), кастомный стек на базе Cython-парсера `picows`, `uvloop` и бинарного C-сериализатора `msgspec`.
- **Протокол:** RFC 8259 JSON (`mm.v1`) поверх WebSocket TCP loopback.

### Текущий профиль задержек (Tuned Stack, бенчмарк `bench/results/20260730T220711Z`):
- **Tick-to-Order ($M3 = m0 \to m3$):** Median ($p50$) = **$149.2\,\mu\text{s}$**, Tail ($p99.9$) = **$667.4\,\mu\text{s}$**.
- **Client ACK Round-Trip ($M1 = t0 \to t3$):** Median ($p50$) = **$58.2\,\mu\text{s}$**, Tail ($p99.9$) = **$134.9\,\mu\text{s}$**.
- **Engine Service Time ($M2 = e1 \to e2$):** Median ($p50$) = **$\sim 0.6 - 1.0\,\mu\text{s}$** ($600-1000\,\text{ns}$).
- **Linux `perf` Flamegraph Breakdown:** Сетевой стек ядра Linux занимает **$46.8\%$** процессорного времени движка (`sock_def_readable` — $17.4\%$, `el0_svc` — $5.5\%$, `netif_rx_internal` — $3.7\%$).

В то время как текущий стек оптимизирован в пределах ограничений связки WebSocket + JSON ($\sim 50-150\,\mu\text{s}$), в промышленных HFT / ULL системах целевая задержка контура котирования составляет **sub-microsecond ($< 500\,\text{ns}$ wire-to-wire, $< 60\,\text{ns}$ internal matching core)**.

```
+---------------------------------------------------------------------------------------------------------+
|                                    ТЕКУЩАЯ АРХИТЕКТУРА (~50 - 150 мкс)                                  |
|                                                                                                         |
|  [ Feed/Timer ] --> [ TopOfBook ] --> [ Outbox/JSON Encode ] --> [ Kernel TCP / Loopback ]              |
|                                                                         |                               |
|  [ Order Engine ] <-- [ JSON Decode / Preflight ] <-- [ Boost.Beast ] <--+                              |
|       |                                                                                                 |
|   (std::map / std::set, std::string keys, vector heap returns)          | (WebSocket Frames)            |
|                                                                         v                               |
|  [ Python MM Client ] <-- [ uvloop / picows ] <-- [ msgspec ] <-- [ Preflight Scan (6.1 мкс) ]          |
|       |                                                                                                 |
|       +--> [ Strategy Core (Dynamic Python Dicts / Object Allocations / GIL) ]                          |
+---------------------------------------------------------------------------------------------------------+

                                                  vs

+---------------------------------------------------------------------------------------------------------+
|                               ПРОМЫШЛЕННАЯ ULL АРХИТЕКТУРА (< 500 нс)                                   |
|                                                                                                         |
|  [ NIC / Hardware ] ---> [ Kernel Bypass (Solarflare EF_VI / DPDK) ] (DMA в Ring Buffer памяти)         |
|                                            |                                                            |
|                                            v (Binary SBE / Raw Multicast Packet)                        |
|  [ Pinned Engine Core ] <==================+=================> [ Pinned Native Strategy Core ]          |
|   - Zero-allocation hot path (0 heap allocs)                    - Preallocated Flat Ring Buffer / IPC   |
|   - Contiguous Flat Array L3 Book                               - Zero-copy SBE decode (0 нс парсинг)   |
|   - Fixed-size structs (No std::string)                         - SIMD Price Check (AVX2/AVX-512)       |
|   - Busy-polling execution loop (0 syscalls/epoll)              - Direct NIC TX Descriptor push         |
|   - Isolated Cores (isolcpus, nohz_full, NUMA pinned)                                                   |
+---------------------------------------------------------------------------------------------------------+
```

---

## 2. Глубокий аудит компонентов: Текущее состояние vs Tier-1 ULL

### 2.1 C++ Engine (`cpp/src/`, `cpp/include/mm/`)

#### 1. Модель потоков и Event Loop
* **Текущее решение:** Однопоточный reactor на базе `boost::asio::io_context` + отдельный фоновый поток телеметрии (`TelemetryWriter`), связанный через lock-free SPSC кольцо (`telemetry_ring.hpp`).
* **Критика с точки зрения ULL:**
  * *Плюсы:* Изоляция состояния книги в одном потоке исключает lock contention, мьютексы и cache-line bouncing. Lock-free SPSC кольцо защищает критический путь от блокирующего файлового I/O.
  * *Минусы:* Reactor-модель (`epoll`/`kqueue`) является interrupt-driven: ядро обрабатывает сетевые прерывания, пробуждает поток через `epoll_wait()`, Asio аллоцирует handler-объекты и выполняет каскадный dispatch.
  * *ULL Стандарт:* Промышленные matching engines и шлюзы работают в **busy-polling цикле** (`while (true) { poll_rx(); match(); drain_tx(); }`), зафиксированном на выделенном изолированном CPU-ядре (`isolcpus`). Переключение контекста и системные вызовы отсутствуют полностью ($0\,\text{ns}$ context switch overhead).

#### 2. Работа с памятью и аллокации на горячем пути
* **Текущее решение:**
  * `OrderEngine` (`engine.hpp`): Хранит заявки в `std::map<OrderKey, Order, KeyLess>` и `std::set<OrdersMap::iterator, LiveLess>`.
  * Ключи заявок: `using OrderKey = std::pair<std::uint64_t, std::string>;`.
  * Возврат результатов: `std::vector<OutMsg> on_new(...)`, `on_cancel(...)`, `on_book(...)`.
* **Критика с точки зрения ULL:**
  * **Аллокация узлов дерева (Node Allocation):** Каждая новая заявка выполняет `operator new` для вставки узла в `std::map`, узла в `std::set`, а при первом обращении сессии — в `entries_by_session_`. Это создает нагрузку на аллокатор кучи и непредсказуемый джиттер в хвостах распределения ($p99.9$).
  * **Pointer Chasing и Cache Misses:** Узлы красно-черного дерева фрагментированы в памяти. Итерация по `live_bids_` при каждом тике маркет-даты требует разыменования указателей узла `std::set`, затем указателей узла `std::map`, что приводит к промахам L1D/L2 кэша.
  * **Векторные аллокации:** Возврат `std::vector<OutMsg>` по значению даже с RVO требует динамической аллокации буфера вектора при превышении начальной емкости.
  * **Динамические строки:** Поля `cl_id`, `symbol`, `reason` используют `std::string`. При длине строки $>15$ байт (на libc++ SSO limit) происходит отдельная аллокация в куче.
  * *ULL Стандарт:* **Строгий 100% Zero-Allocation Invariant на горячем пути.** Память пулов заявок и стака вывода аллоцируется статически при старте приложения в виде непрерывных массивов (Flat Arrays) или кольцевых буферов. Строки заменяются на `char[16]` или 64-битные целочисленные ID.

#### 3. Представление биржевого стакана (Order Book)
* **Текущее решение:** Движок хранит только лучший уровень (`Book` struct в `engine.hpp`) и сканирует активные заявки сессии при обновлении цен.
* **Критика с точки зрения ULL:** Для поддержки полноценной Level-2/Level-3 книги требуются безызбыточные структуры: прямой массив ценовых уровней (Direct Indexed Array) с адресацией `price / tick_size` и двусвязные интрузивные списки заявок (`Intrusive Doubly-Linked Lists`) внутри статических пулов, обеспечивающие операции вставки, отмены и исполнения за константное время $O(1)$ без аллокаций.

#### 4. Векторизация и SIMD
* **Текущее решение:** Скалярный побайтовый preflight-скан (`frame_preflight.cpp`) и скалярная проверка условий исполнения.
* **ULL Стандарт:** Использование векторных инструкций AVX2 / AVX-512 для параллельного сопоставления цен, парсинга разделителей и валидации диапазонов (до 32–64 байт за такт процессора).

---

### 2.2 Python MM Client (`python/mmclient/`)

#### 1. Накладные расходы среды исполнения (CPython Runtime)
* **Текущее решение:** Клиент на базе `uvloop` + `picows` с архитектурой sans-IO (`strategy.py`).
* **Критика с точки зрения ULL:**
  * *Плюсы:* `picows` на Cython вызывает callbacks `SessionDriver` напрямую из C-парсера без создания `asyncio.Task` на каждый фрейм.
  * *Минусы:* CPython ограничен динамической диспетчеризацией методов, подсчетом ссылок (`Py_INCREF`/`Py_DECREF`), накладными расходами на boxing примитивов в объекты Python и паузами сборщика мусора (GC).
  * Реакция стратегии на тик ($m0' \to m3$) занимает **$148.1\,\mu\text{s}$ ($p50$)**. В ULL торговле время принятия решения стратегией должно укладываться в **$< 200\,\text{ns}$**.

#### 2. Накладные расходы парсинга и Preflight
* **Текущее решение:** Валидация кастомным сканером `_preflight.py` + десериализация через `msgspec.decode`.
* **Критика с точки зрения ULL:**
  * Сканирование JSON в Python (`_preflight.py`) занимает **$6.1\,\mu\text{s}$** — это в 6 раз дольше, чем исполнение всего матчинг-движка C++ ($1.0\,\mu\text{s}$).

---

### 2.3 Сетевой уровень и формат передачи данных

#### 1. JSON Text vs Simple Binary Encoding (SBE)
* **Текущее решение:** Текстовый JSON по RFC 8259.
* **Критика с точки зрения ULL:**
  * Сообщение `NewOrder` в JSON весит $\sim 150\,\text{байт}$ с избыточными строковыми ключами. Эквивалентное бинарное сообщение SBE занимает **$48-58\,\text{байт}$**.
  * Преобразование чисел в текст (`std::to_chars`) и обратно (`std::from_chars`) сжигает сотни тактов CPU.
  * *ULL Стандарт (SBE / CME iLink3 / Binance ULL):*
    ```cpp
    #pragma pack(push, 1)
    struct SbeNewOrder {
        uint16_t msg_type;
        uint64_t seq;
        uint64_t cl_ord_id;
        int64_t  price;
        int64_t  qty;
        uint8_t  side;
        uint8_t  post_only;
    };
    #pragma pack(pop)
    // Декодирование за 0 наносекунд (Zero-Copy):
    const auto& order = *reinterpret_cast<const SbeNewOrder*>(rx_buffer);
    ```

#### 2. Транспорт: WebSocket vs Shared Memory vs Kernel Bypass
* **Текущее решение:** WebSocket по TCP Loopback (`127.0.0.1`).
* **Критика с точки зрения ULL:**
  * Маскирование фреймов WebSocket (XOR по каждому байту от клиента), парсинг заголовков, фрагментация.
  * Накладные расходы сетевого стека ядра Linux: RTT TCP Loopback составляет **$37.5\,\mu\text{s}$**, тогда как передача через Shared Memory кольцо занимает **$0.375\,\mu\text{s}$ ($100\times$ быстрее)**.

---

## 3. Анализ компромиссов тестового задания (Take-Home Scope Trade-offs)

Решения, принятые при разработке прототипа, являются осознанными инженерными компромиссами:

| Архитектурное измерение | Выбор в прототипе | Стандарт ULL Production | Обоснование компромисса в прототипе |
|---|---|---|---|
| **Формат сообщений** | **JSON text (`mm.v1`)** | **SBE / Fixed POD Binary** | **Инспектируемость:** Возможность отладки через Wireshark/снапшоты, читаемость логов, кросс-языковые golden fixtures (`tests/golden/`). |
| **Транспорт** | **WebSocket over TCP** | **Kernel Bypass UDP / Direct TCP / Shm** | **Стандартизация:** Совместимость с любыми веб- и Python-инструментами без сборки платформозависимых бинарных драйверов. |
| **Язык стратегии** | **Python (uvloop/picows/msgspec)** | **Pure C++20 / Rust Native** | **Скорость квант-разработки:** Демонстрация sans-IO архитектуры и изолированное A/B тестирование сетевого стека. |
| **Модель потоков** | **Asio `io_context` (2 потока)** | **Busy-Polling на изолированном ядре** | **Бережливость к ресурсам:** Движок не утилизирует CPU на 100% в idle-режиме на ноутбуке разработчика или в Docker. |
| **Структуры данных** | **`std::map` / `std::set` / `std::string`** | **Flat Array / Memory Pool / Zero-Alloc** | **Строгая безопасность исключений:** Стабильность итераторов и простота отката состояния при сбоях. |
| **IPC / Межпроцессная связь** | **SPSC Ring (внутри процесса)** | **Shared Memory Seqlock Ring** | **Верификация через TSan:** Shared Memory между процессами невидим для ThreadSanitizer; in-process кольцо позволило добиться 100% чистых проверок TSan/ASan. |

---

## 4. Пошаговый план трансформации в Sub-Microsecond Production Engine

```
+---------------------------------------------------------------------------------------------------------+
|                                    5-ФАЗНЫЙ ПЛАН ТРАНСФОРМАЦИИ                                          |
|                                                                                                         |
|  Фаза 1: Zero-Alloc C++ Core           --> Ликвидация std::map/vector, Flat Contiguous L3 Book          |
|                                            (Цель M2: 1.0 µs -> 60 ns)                                   |
|                                                                                                         |
|  Фаза 2: Binary SBE Wire Protocol      --> Замена JSON/WS на фиксированный бинарный SBE протокол       |
|                                            (Цель Encode/Decode: 7.8 µs -> 0 ns)                         |
|                                                                                                         |
|  Фаза 3: Shared Memory (IPC) Transport --> SPSC Hugepage Ring Buffers c Seqlock и Doorbell              |
|                                            (Цель Transport RTT: 37.5 µs -> 250 ns)                      |
|                                                                                                         |
|  Фаза 4: Native Strategy & Kernel Bypass-> Solarflare EF_VI / DPDK, C++20 Strategy Hot Path            |
|                                            (Цель Tick-to-Order: 149 µs -> 350 ns)                       |
|                                                                                                         |
|  Фаза 5: OS Tuning & CPU Pinning       --> isolcpus, nohz_full, NUMA Node Pinning, AVX-512 SIMD        |
|                                            (Цель Tail Jitter p99.9: 667 µs -> < 1.2 µs)                 |
+---------------------------------------------------------------------------------------------------------+
```

---

### Фаза 1: Zero-Allocation & Cache-Friendly C++ Engine Core
**Цель:** Снизить Engine Service Time $M2$ с $1.0\,\mu\text{s} \to \mathbf{< 60\,\text{ns}}$.

1. **Ликвидация динамической памяти на горячем пути:**
   - Замена `std::string cl_id` на фиксированную структуру `struct ClOrdId { char data[16]; };` или 64-битный целочисленный `uint64_t`.
   - Замена возвращаемого `std::vector<OutMsg>` на передачу предварительно аллоцированного caller-буфера (`std::span<OutMsg>`):
     ```cpp
     // Было: std::vector<OutMsg> on_new(uint64_t session, const NewOrder& order);
     // Стало:
     size_t on_new(uint64_t session, const NewOrder& order, std::span<OutMsg> out_buffer) noexcept;
     ```
2. **Плоское хранилище заявок (Flat Array Slot-Map):**
   - Замена `std::map` и `std::set` на статический непрерывный массив с выравниванием по границе кэш-линии (64 байта):
     ```cpp
     struct alignas(64) OrderSlot {
         uint64_t eng_id;
         uint64_t session_id;
         uint64_t cl_id_num;
         int64_t  price_ticks;
         int64_t  leaves_qty;
         Side     side;
         OrdState state;
     };
     alignas(64) std::array<OrderSlot, MAX_ORDERS> order_pool_;
     ```
3. **Книга заявок на интрузивных списках:**
   - Представление ценовых уровней в виде плоского массива `PriceLevel levels_[MAX_PRICE_LEVELS]`.
   - Поддержание очереди заявок внутри уровня через интрузивные индексы `next_order_idx` / `prev_order_idx` ($O(1)$ вставка, отмена и исполнение с идеальной локальностью кэша).

---

### Фаза 2: Simple Binary Encoding (SBE) Wire Protocol
**Цель:** Ликвидация накладных расходов на парсинг (Декодирование: $7.8\,\mu\text{s} \to \mathbf{0\,\text{ns}}$; Кодирование: $1.2\,\mu\text{s} \to \mathbf{0\,\text{ns}}$).

1. **Определение бинарной схемы SBE:**
   ```cpp
   #pragma pack(push, 1)
   struct SbeHeader {
       uint16_t block_length;
       uint16_t template_id;
       uint16_t schema_id;
       uint16_t version;
   };

   struct SbeNewOrder {
       SbeHeader header;
       uint64_t  seq;
       uint64_t  epoch;
       uint64_t  md_seq;
       uint64_t  cl_ord_id;
       int64_t   price;
       int64_t   qty;
       uint8_t   side;      // 1 = Bid, 2 = Ask
       uint8_t   post_only; // 1 = True, 0 = False
   };
   #pragma pack(pop)
   static_assert(sizeof(SbeNewOrder) == 58);
   ```
2. **Zero-Copy Memory Mapping:**
   - Входящий кадр с сокета/кольца сразу мапится на структуру через прямой каст указателя.
   - Исходящие сообщения `OrderAck` и `Fill` формируются прямой записью в выходной кольцевой буфер без промежуточных буферов сериализации.

---

### Фаза 3: Межпроцессная связь через Shared Memory (IPC)
**Цель:** Сократить задержку транспорта с $37.5\,\mu\text{s} \to \mathbf{250\,\text{ns}}$.

1. **Lock-Free SPSC Ring Buffer в Shared Memory:**
   - Выделение сегмента разделяемой памяти POSIX (`shm_open`, `mmap`) с поддержкой HugePages 2MB (`MAP_HUGETLB`).
   - Использование atomic-индексов с выравниванием по кэш-линиям и барьерами `acquire`/`release`:
     ```cpp
     struct alignas(64) ShmRingHeader {
         alignas(64) std::atomic<uint64_t> write_index{0};
         alignas(64) std::atomic<uint64_t> read_index{0};
         uint32_t capacity_mask;
     };
     ```
2. **Асимметричная политика колец:**
   - **Market Data Ring:** Кольцо с перезаписью старых тиков (Overwrite Oldest) — стратегия всегда видит самый свежий TOB с нулевым лагом.
   - **Order Entry Ring:** Кольцо с гарантированной доставкой без потерь (Never Drop) с сигналом backpressure при переполнении.
3. **Механизм ожидания:** Короткий spin-wait (100–1000 итераций `_mm_pause()`) с переходом на `futex` или Linux `io_uring` / `eventfd` при длительном простое.

---

### Фаза 4: Нативная C++ стратегия и Kernel Bypass
**Цель:** Сократить цикл реакции Tick-to-Order ($M3$) с $149\,\mu\text{s} \to \mathbf{< 350\,\text{ns}}$.

```
+----------------------------------------------------------------------------------------------------+
|                                 NATIVE C++ STRATEGY HOT-PATH LOOP                                  |
|                                                                                                    |
|  while (running) {                                                                                 |
|      // 1. Поллинг Shm кольца маркет-даты (0 ns context switch)                                    |
|      if (md_ring.poll(tob)) {                                                                      |
|          // 2. SIMD-оценка спреда и расчет котировок (< 50 ns)                                     |
|          if (strategy.on_tob(tob, new_bid, new_ask)) {                                             |
|              // 3. Прямая запись SBE заявки в Shm / TX дескриптор сетевой карты (< 30 ns)          |
|              oe_ring.push_order(new_bid);                                                          |
|          }                                                                                         |
|      }                                                                                             |
|      // 4. Поллинг отчетов об исполнениях (Fills / ACKs)                                           |
|      if (oe_ring.poll_report(report)) {                                                            |
|          strategy.on_report(report);                                                               |
|      }                                                                                             |
|  }                                                                                                 |
+----------------------------------------------------------------------------------------------------+
```

1. **Перенос ядра стратегии на C++20:**
   - Полный перенос логики `Strategy` на C++20. Все состояние котирования хранится в единой плоской структуре размером $<128$ байт (помещается в 2 кэш-линии L1D).
2. **Гибридный Control Plane на Python (опционально):**
   - Если Python сохраняется для расчета квант-моделей и волатильности, Python выносится в Cold Path:
   - Python-процесс пишет параметры риск-контроля и ширины спреда в служебный Shared Memory сегмент с низкой частотой (10–50 Hz).
   - Горячий C++ поток читает параметры через атомарный `seqlock` без блокировки торгового цикла.
3. **Kernel Bypass для внешних подключений:**
   - Интеграция драйверов **Solarflare OpenOnload / EF_VI** или **DPDK**.
   - Пакеты с сетевой карты поступают напрямую в память пользовательского процесса через DMA минуя сетевой стек ядра Linux.

---

### Фаза 5: Настройка операционной системы, CPU Pinning и изоляция оборудования
**Цель:** Ликвидация джиттера в хвостах распределений (снижение $p99.9$ с $667\,\mu\text{s} \to \mathbf{< 1.2\,\mu\text{s}}$).

1. **Параметры загрузки ядра Linux (`/etc/default/grub`):**
   - `isolcpus=2-7`: Полная изоляция ядер 2–7 от стандартного планировщика задач OS.
   - `nohz_full=2-7`: Перевод изолированных ядер в режим tickless (отключение таймерных прерываний ядра).
   - `rcu_nocbs=2-7`: Вынос RCU-коллбэков на ядра общего назначения.
   - `intel_idle.max_cstate=0 processor.max_cstate=0 idle=poll`: Отключение переходов CPU в энергосберегающие C-состояния (устранение задержки пробуждения процессора).
   - `mitigations=off`: Отключение защиты от атак класса Spectre/Meltdown (KPTI) в закрытом торговом контуре.
2. **Привязка потоков к ядрам (Core & NUMA Affinity):**
   - Поток движка жестко фиксируется на изолированном ядре 2, поток стратегии — на ядре 3:
     ```cpp
     cpu_set_t cpuset;
     CPU_ZERO(&cpuset);
     CPU_SET(core_id, &cpuset);
     pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
     ```
   - Память аллоцируется строго на локальном NUMA-узле сетевой карты: `numactl --membind=0`.
3. **Блокировка страниц памяти и HugePages:**
   - Блокировка виртуальной памяти в RAM для предотвращения page faults: `mlockall(MCL_CURRENT | MCL_FUTURE);`.
   - Использование HugePages 2MB / 1GB (`/dev/hugepages`) для устранения промахов TLB (Translation Lookaside Buffer).
4. **Флаги компилятора и кодогенерация:**
   - Сборка с флагами: `-O3 -march=native -mtune=native -flto -fno-rtti -fno-exceptions`.
   - Применение Profile-Guided Optimization (PGO) для оптимальной раскладки ветвлений и инлайнинга.

---

## 5. Итоговая матрица трансформации задержек

| Компонент / Метрика | Текущий прототип (Tuned) | Целевая ULL архитектура (Shm / Native) | Ожидаемый выигрыш |
|---|---|---|---|
| **Ядро матчинга ($M2$)** | $1.0\,\mu\text{s}$ | $\mathbf{0.04 - 0.08\,\mu\text{s}}$ ($40-80\,\text{ns}$) | **$\sim 15\times$** |
| **Декодирование / Кодирование** | $7.8\,\mu\text{s}$ (JSON / Preflight) | $\mathbf{0.00\,\mu\text{s}}$ (Zero-Copy SBE) | **$> 100\times$** |
| **Сетевой транспорт** | $37.5\,\mu\text{s}$ (TCP Loopback) | $\mathbf{0.25 - 0.35\,\mu\text{s}}$ (Shm Ring / HugePages) | **$\sim 100\times$** |
| **Логика стратегии** | $148.1\,\mu\text{s}$ (Python / uvloop) | $\mathbf{0.05 - 0.15\,\mu\text{s}}$ (C++ Native Core) | **$\sim 1000\times$** |
| **Полный цикл Tick-to-Order ($M3$)** | **$149.2\,\mu\text{s}$** ($p50$) | $\mathbf{0.35 - 0.55\,\mu\text{s}}$ ($350-550\,\text{ns}$) | **$\mathbf{\sim 300\times}$** |
| **Джиттер в хвосте ($p99.9$)** | **$667.4\,\mu\text{s}$** | $\mathbf{< 1.5\,\mu\text{s}}$ | **$\mathbf{\sim 450\times}$** |

## 6. Стратегия покрытия тестами и контроль регрессий (Test Coverage & QA)

В процессе ULL-трансформации любые структурные изменения должны валидироваться через строгий двухуровневый контур покрытия кода и исторический реестр регрессий:

```
+----------------------------------------------------------------------------------------------------+
|                         КОМПЛЕКСНЫЙ КОНТУР ТЕСТИРОВАНИЯ И ПОКРЫТИЯ КОДА                            |
+------------------------------+----------------------------------+----------------------------------+
| Уровень верификации          | C++ Подсистема (Engine / Codec)  | Python Подсистема (Client / MM)  |
+------------------------------+----------------------------------+----------------------------------+
| Фреймворк тестирования       | Catch2 v3 (179 тестов)           | pytest (580 тестов)              |
| Инструмент покрытия          | `llvm-cov` (Source-Based)        | `coverage.py` (Branch Coverage)  |
| Текущий уровень покрытия     | > 93% Lines / > 83% Branches     | 100.0% Branches (Ratchet Gate)   |
| Проверка утечек памяти       | ASan + LSan (LeakSanitizer)      | tracemalloc / memray             |
| Проверка многопоточности     | TSan (ThreadSanitizer)           | asyncio concurrency battery      |
| Команда запуска              | `make coverage-cxx`              | `make coverage`                  |
+------------------------------+----------------------------------+----------------------------------+
```

### 1. Нормативы покрытия для ULL-компонентов:
1. **Core Matching Engine (`engine.cpp`):**
   - Минимум **$>95\%$ Line Coverage** и **$>90\%$ Branch Coverage**.
   - Обязательное тестирование всех граничных условий валидации (`validation_reason`, проверка tick/lot, ценовые диапазоны, post-only cross).
   - Аллокационные зонды (`test_engine_alloc.cpp`): 0 динамических аллокаций в steady-state на `on_book` без исполнения.
2. **Codec & Protocol Preflight (`codec_glaze.cpp`, `_preflight.py`):**
   - Полное тестирование эквивалентности (`test_codec_equivalence.cpp`): 100% совпадение текстов ошибок и кодов возврата между Naive и Tuned кодеками.
   - Дифференциальный фаззинг граничных значений (BOM, surrogates, trailing garbage, oversize payload).
3. **Единая команда запуска полного контура:**
   ```bash
   make coverage-all  # Запуск C++ llvm-cov (>93%) + Python coverage.py (100% ratchet)
   ```
4. **Контроль регрессий перформанса:**
   - После любого изменения в горячем пути запускается `make perf-track` или автономный субагент `perf_regression_tracker`.
   - Регрессией считается рост $M1/M2/M3$ более чем на $5\%$ от базовой линии в [`bench/history/ledger.json`](file:///Users/dima/cpp/cpp_assignment/bench/history/ledger.json).

---
*Документ подготовлен и зафиксирован в репозитории: [`docs/ULL_ARCHITECTURAL_ANALYSIS.ru.md`](file:///Users/dima/cpp/cpp_assignment/docs/ULL_ARCHITECTURAL_ANALYSIS.ru.md).*
