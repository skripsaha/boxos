ID: 0x02

Название: Storage Deck (Менеджер Tagfs)

Роль: Семантическое управление данными. Превращает «теги» в «объекты».

Описание

Этот Дек отвечает за логику Tagfs. Он работает с базой метаданных тегов и связывает их с физическими ID объектов.

Таблица Opcodes

Opcode	Команда	Описание	Аргументы (в Data)
0x01	TAG_QUERY	Поиск файлов по тегам (intersection).	[tags_count, tags_string...]
0x02	TAG_SET	Добавление/изменение тега файла.	[file_id, tag_string]
0x03	TAG_UNSET	Удаление тега.	[file_id, tag_key]
0x04	OBJ_OPEN	Получение дескриптора и размера.	[file_id]
0x05	OBJ_READ	Чтение контента в event->data.	[file_id, offset, len]
0x06	OBJ_WRITE	Запись из event->data в объект.	[file_id, offset, len]
0x07	OBJ_CREATE	Создание пустого объекта.	[initial_tags...]
0x08	OBJ_DELETE	Пометка на удаление (trashed).	[file_id]