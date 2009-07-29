Утилита MCPROG предназначена для записи программного обеспечения
в память микроконтроллеров Элвис Мультикор
(http://multicore.ru/index.php?id=27).

В качестве программатора используется адаптер JTAG-EPP
(http://multicore.ru/index.php?id=54), подключающийся
к параллельному порту компьютера. Поддерживаются чипы
Flash-памяти AMD/Alliance 29LV800 и SST 39VF800.

При вызове без параметров утилита MCPROG определяет тип процессора
и flash-памяти, установленных на плате. Например:

	Processor: MC12 (id 20777001)
	Flash at 1FC00000: SST 39VF800 (id 00BF 2781), 2 Mbytes, 32 bit wide
	Flash at 1FA00000: SST 39VF800 (id 00BF 2781), 2 Mbytes, 32 bit wide
	Flash at 02000000: SST 39VF800 (id 00BF 2781), 4 Mbytes, 64 bit wide

Запись в flash-память:
        mcprog [-v] file.sre
        mcprog [-v] file.bin [address]

Запись в статическую память:
        mcprog -w [-v] file.sre
        mcprog -w [-v] file.bin [address]

Чтение памяти в файл:
        mcprog -r file.bin address length

Параметры:
	file.srec  - файл с прошивкой в формате SREC
	file.bin   - бинарный файл с прошивкой
	address	   - адрес flash-памяти, по умолчанию 0xBFC00000
	-v	   - без записи, только проверка памяти на совпадение
        -w         - запись в статическую память
        -r         - чтение памяти

Входной файл должен иметь простой бинарный формат, или SREC.
Формат SREC предпочтительнее, так как в нём имеется информация
об адресах программы. Преобразовать формат ELF или COFF или A.OUT
в SREC можно командой objcopy, например:

	objcopy -O srec firmware.elf firmware.srec


Под Windows 2000/XP/Vista нужно установить драйвер GIVEIO,
позволяющий прямой доступ к параллельному порту.

1) Кладём giveio.sys в C:\WINDOWS\system32\drivers\

2) Запускаем install.reg

3) Перезагружаемся
___
С уважением,
Сергей Вакуленко,
ИТМиВТ 2008-2009
