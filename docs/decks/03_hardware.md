ID: 0x03

Название: Hardware Deck (Аппаратный шлюз)

Роль: Прямое взаимодействие с оборудованием.

Таблица Opcodes

Opcode	Команда	Описание	Аргументы (в Data)
0x01	HW_PORT_IN	Чтение из порта (INB/INW/INL).	[port, size]
0x02	HW_PORT_OUT	Запись в порт (OUTB/OUTW/OUTL).	[port, size, val]
0x03	HW_MMIO_RD	Чтение из памяти устройства.	[phys_addr, size]
0x04	HW_MMIO_WR	Запись в память устройства.	[phys_addr, size, val]
0x05	HW_DMA_CFG	Настройка DMA канала.	[ch, addr, len, dir]
0x06	HW_IRQ_LNK	Привязка IRQ к прерыванию процесса.	[irq_vec, callback_ptr]