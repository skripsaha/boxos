docs/decks/01_operations.md
ID: 0x01

Название: Operations Deck (Вычислительный узел)

Роль: Трансформация данных «на лету» внутри структуры Event.

Описание

Дек не имеет доступа к внешним ресурсам (диск, сеть). Его область видимости ограничена буфером event->data. Используется для подготовки данных перед отправкой в другие Деки или для обработки результатов перед выдачей пользователю.

Таблица Opcodes

Opcode	Команда	Описание	Аргументы (в Data)
0x01	BUF_MOVE	Перемещение блока байтов.	[from_off, to_off, len]
0x02	BUF_FILL	Заполнение памяти байтом (memset).	[offset, len, byte]
0x03	BUF_XOR	Побитовый XOR (шифрование/маски).	[offset, len, mask]
0x04	BUF_HASH	Вычисление CRC32/Hash.	[offset, len, target_off]
0x05	BUF_CMP	Сравнение двух областей.	[off1, off2, len]
0x06	BUF_FIND	Поиск паттерна в данных.	[pattern_len, pattern...]
0x07	BUF_PACK	Сжатие данных (LZ4).	[offset, len]
0x08	BUF_UNPACK	Распаковка данных.	[offset, len]
0x09	BIT_SWAP	Изменение Endianness (LE <-> BE).	[offset, len, mode]
0x0A	VAL_MOD	Инкремент/декремент значения.	[offset, type_size, delta]