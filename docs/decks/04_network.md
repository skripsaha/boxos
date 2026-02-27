ID: 0x04

Название: Network Deck (Сетевой стек)

Роль: Упаковка, маршрутизация и передача данных по протоколам BoxNet/TCP/IP.

Таблица Opcodes:
Opcode Команда Описание Аргументы (в Data)
0x01 NET_POST Отправить сырой буфер в сеть. [dest_addr, port, proto]
0x02 NET_RECV Получить данные из сокета. [socket_id, max_len]
0x03 NET_BIND Открыть порт на прослушивание. [port, proto]
0x04 NET_RESOLVE DNS/Node-name разрешение. [name_string]
0x05 NET_STAT Состояние сетевого интерфейса. [iface_id]
