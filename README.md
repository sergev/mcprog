Утилита MCPROG предназначена для записи программного обеспечения
в память микроконтроллеров [Элвис Мультикор](http://multicore.ru/index.php?id=27).

В качестве программатора используется адаптер [JTAG-EPP](http://multicore.ru/index.php?id=54),
подключающийся к параллельному порту компьютера. Поддерживаются чипы
Flash-памяти AMD/Alliance 29LV800 и SST 39VF800.

 * [Подробнее об утилите MCPROG](https://github.com/sergev/mcprog/wiki/mcprog_ru)
 * [Подробнее об утилите MCREMOTE](https://github.com/sergev/mcprog/wiki/mcremote_ru)
 * [Программаторы для процессоров Элвис Мультикор](https://github.com/sergev/mcprog/wiki/adapters_ru)


## Вызов

При вызове без параметров утилита MCPROG определяет тип процессора
и flash-памяти, установленных на плате. Например:

	Processor: MC12 (id 20777001)
	Board: pkbi
	Flash at 1FC00000: SST 39VF800 (id 00BF 2781), 2 Mbytes, 32 bit wide
	Flash at 1FA00000: SST 39VF800 (id 00BF 2781), 2 Mbytes, 32 bit wide
	Flash at 02000000: SST 39VF800 (id 00BF 2781), 4 Mbytes, 64 bit wide

Запись в flash-память:
        mcprog [-v] file.srec
        mcprog [-v] file.hex
        mcprog [-v] file.bin [address]

Запись в статическую память:
        mcprog -w [-v] file.srec
        mcprog -w [-v] file.hex
        mcprog -w [-v] file.bin [address]

Чтение памяти в файл:
        mcprog -r file.bin address length

Параметры:
	file.srec  - файл с прошивкой в формате SREC
	file.hex   - файл с прошивкой в формате Intel HEX
	file.bin   - бинарный файл с прошивкой
	address	   - адрес flash-памяти, по умолчанию 0xBFC00000
	-v	   - без записи, только проверка памяти на совпадение
        -w         - запись в статическую память
        -r         - чтение памяти
        -b name    - выбор типа платы

Входной файл должен иметь простой бинарный формат, или SREC, или Intel HEX.
Форматы SREC и HEX предпочтительнее, так как в них имеются контрольные суммы
и информация об адресах программы. Преобразовать формат ELF или COFF или A.OUT
в SREC или HEX можно командой objcopy, например:

	objcopy -O srec firmware.elf firmware.srec
	objcopy -O ihex firmware.elf firmware.hex


## Файл конфигурации

Утилита считывает параметры платы из файла конфигурации "mcprog.conf".
Файл должен находиться  в текущем каталоге, либо в каталоге /usr/local/etc
(для Linux), либо в каталоге утилиты "mcprog.exe" (для Windows).
В файле для каждого типа платы задаются значения регистров CSR и CSCONi,
а также диапазоны адресов секций flash-памяти.

Можно определять до 16 flash-секций вида "flash foobar = first-last".
Здесь foobar - произвольное имя, first и last - первый и последний адреса секции.

Тип платы задаётся при вызове флагом "-b", либо параметром "default"
в конфигурационном файле.


## Драйвер LPT-порта для Windows

Под Windows 2000/XP/Vista нужно установить драйвер GIVEIO,
позволяющий прямой доступ к параллельному порту.

1) Кладём giveio.sys в C:\WINDOWS\system32\drivers\

2) Запускаем install.reg

3) Перезагружаемся
___
С уважением,
Сергей Вакуленко,
ИТМиВТ 2008-2009
