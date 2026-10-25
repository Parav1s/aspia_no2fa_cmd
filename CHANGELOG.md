# Aspia (форк без 2FA) — что изменено

Основа: оригинальная Aspia 3.0.37. Ниже — отличия форка от неё. Ссылки ведут на коммиты в этом
репозитории. Версия сборки — `3.0.37.<номер коммита>`.

## Аутентификация и хосты

- Полностью удалена двухфакторная аутентификация: вход выполняется по логину и паролю
  ([a5d9bd88c](https://github.com/Parav1s/aspia_no2fa_cmd/commit/a5d9bd88c)).
- Новые хосты подтверждаются автоматически, настройка и страница «Неподтверждённые хосты» удалены
  ([ed940af9a](https://github.com/Parav1s/aspia_no2fa_cmd/commit/ed940af9a),
  [867e22243](https://github.com/Parav1s/aspia_no2fa_cmd/commit/867e22243),
  [c72a97985](https://github.com/Parav1s/aspia_no2fa_cmd/commit/c72a97985)).
- Одноразовый пароль можно отключить; при отключённом пароле его поле скрыто в окне хоста
  ([de1232740](https://github.com/Parav1s/aspia_no2fa_cmd/commit/de1232740),
  [2db1894f2](https://github.com/Parav1s/aspia_no2fa_cmd/commit/2db1894f2)).
- Хост получает постоянный ID сразу; освободившиеся ID переиспользуются (берётся минимальный
  свободный). У хостов появилась дата создания
  ([9dbe1790e](https://github.com/Parav1s/aspia_no2fa_cmd/commit/9dbe1790e)).
- Legacy-слой принимает хосты версии 2.4
  ([350d2d591](https://github.com/Parav1s/aspia_no2fa_cmd/commit/350d2d591)).
- Windows 7: точки входа WinRT загружаются вручную, без статических импортов
  ([27fe17a76](https://github.com/Parav1s/aspia_no2fa_cmd/commit/27fe17a76)).

## Терминал

- Оболочка выбирается по версии системы: ConPTY (Windows 10 1809 и новее) — PowerShell, без ConPTY
  (Windows 7/8) — перенаправленные каналы. Агент терминала запускается с повышенным токеном без
  запроса UAC, ошибка запуска оболочки показывается в окне терминала
  ([dd9141cb3](https://github.com/Parav1s/aspia_no2fa_cmd/commit/dd9141cb3)).
- Добавлен диагностический режим `aspia_host --shell-test` (запуск оболочки без сессии)
  ([265fe0d46](https://github.com/Parav1s/aspia_no2fa_cmd/commit/265fe0d46)).

## Быстрая поддержка (Quick Support)

- Экспорт из настроек хоста: один exe с адресом и ключом роутера, пользователями хоста (логин и
  пароль) и ID хоста внутри; одноразовый пароль не экспортируется
  ([e36058957](https://github.com/Parav1s/aspia_no2fa_cmd/commit/e36058957)).
- Запуск без прав администратора: служба работает внутри процесса, окно — отдельный процесс того же
  файла. Данные хранятся в `%LOCALAPPDATA%\AspiaQS`, диалоги экспорта открываются на десктопе
  пользователя сессии, а не администратора
  ([9d4842078](https://github.com/Parav1s/aspia_no2fa_cmd/commit/9d4842078),
  [254a869c2](https://github.com/Parav1s/aspia_no2fa_cmd/commit/254a869c2),
  [fafd8e372](https://github.com/Parav1s/aspia_no2fa_cmd/commit/fafd8e372)).
- Пакет запускает только своё окно из собственного exe. Установленный `aspia_host.exe` из каталога
  пакета не запускается, поэтому установленная Aspia не появляется после закрытия QS.
- ID: если установленный хост запущен, пакет получает собственный новый ID только один раз — дальше
  он сохраняет этот уникальный ID между запусками и не конфликтует с установленным хостом. Если
  установленный хост остановлен, пакет наоборот использует ID установленного хоста — из его базы или
  из настроек, встроенных в пакет, — чтобы клиенты находили его по прежнему ID
  ([eeb604421](https://github.com/Parav1s/aspia_no2fa_cmd/commit/eeb604421),
  [9e03716c2](https://github.com/Parav1s/aspia_no2fa_cmd/commit/9e03716c2)).
- Кнопка со щитом рядом с ID поднимает с правами администратора только десктоп-агент. Служба и
  клиентский сеанс при этом не перезапускаются, поэтому клиент не отключается: на время замены
  агента он видит «Ожидание подтверждения прав администратора» и ждёт до 60 секунд. Если запрос UAC
  отменён, обычный агент возвращается
  ([1e6a9052f](https://github.com/Parav1s/aspia_no2fa_cmd/commit/1e6a9052f),
  [9dda36568](https://github.com/Parav1s/aspia_no2fa_cmd/commit/9dda36568)).
- Пакет не проверяет и не устанавливает обновления: в нём нет модуля обновлений. Иначе он скачивал
  бы официальную Aspia и запускал установщик с запросом UAC. Обновление пакета — повторный экспорт
  из новой версии хоста
  ([a8c571cf4](https://github.com/Parav1s/aspia_no2fa_cmd/commit/a8c571cf4)).
- Одноразовый пароль в пакете всегда выключен, и его блок в окне не показывается
  ([a8c571cf4](https://github.com/Parav1s/aspia_no2fa_cmd/commit/a8c571cf4)).
- Закрытие окна завершает работу пакета (в трей не сворачивается); параметр установленного хоста
  «запретить закрытие» на пакет не действует
  ([fce81a1e6](https://github.com/Parav1s/aspia_no2fa_cmd/commit/fce81a1e6)).
- Служебные ключи: `--export-quick-support=<файл>`, `--qs-info`, `--qs-dir=<путь>`.
- Без прав администратора ввод в окна, запущенные от администратора, блокируется Windows (UIPI):
  пока такое окно в фокусе, не работают ни клики, ни клавиши. После закрытия/сворачивания окна ввод
  возвращается сам. Для управления такими окнами и нужен щит.
- Прямой порт 8050 в portable-режиме не слушается: работа только через роутер.

## Клиент

- Чистка неактивных хостов: 1, 3 или 6 месяцев, по умолчанию выключена. Хосты без даты последнего
  подключения и хосты онлайн не удаляются; перед удалением — подтверждение с количеством и списком
  ([6ab09adcb](https://github.com/Parav1s/aspia_no2fa_cmd/commit/6ab09adcb)).
- Режим подключения из командной строки с опциями 2.x (`--address`, `--username`, `--password`,
  `--session-type`, `--router-address` и другие); опции сессии сохраняются
  ([af0e91eff](https://github.com/Parav1s/aspia_no2fa_cmd/commit/af0e91eff),
  [ece005ac5](https://github.com/Parav1s/aspia_no2fa_cmd/commit/ece005ac5)).
- Тулбар командной сессии действует на своё окно
  ([2b8a4118d](https://github.com/Parav1s/aspia_no2fa_cmd/commit/2b8a4118d)).
- Экспорт установщика с настройками хоста из окна настроек
  ([2c95aa382](https://github.com/Parav1s/aspia_no2fa_cmd/commit/2c95aa382)).
- Опция «Запомнить мастер-пароль»: ключ базы хранится в системном хранилище секретов, пароль
  спрашивается один раз
  ([6b0f16b49](https://github.com/Parav1s/aspia_no2fa_cmd/commit/6b0f16b49)).
- Запоминание роутера и его параметров
  ([315a94562](https://github.com/Parav1s/aspia_no2fa_cmd/commit/315a94562),
  [e1228d533](https://github.com/Parav1s/aspia_no2fa_cmd/commit/e1228d533),
  [331ed77f1](https://github.com/Parav1s/aspia_no2fa_cmd/commit/331ed77f1),
  [6b542151e](https://github.com/Parav1s/aspia_no2fa_cmd/commit/6b542151e)).
- Свой сервер обновлений: адрес и публичный ключ
  ([88b62a4f5](https://github.com/Parav1s/aspia_no2fa_cmd/commit/88b62a4f5)).
- В удалённой панели передачи файлов есть ярлык `AppData` пользователя, в сеансе которого работает
  хост. Локальная панель клиента этот ярлык не показывает
  ([6207c411e](https://github.com/Parav1s/aspia_no2fa_cmd/commit/6207c411e)).

## Экран и захват

- Список экранов хоста и просмотр всех мониторов сразу, режим «весь рабочий стол»
  ([cced0e975](https://github.com/Parav1s/aspia_no2fa_cmd/commit/cced0e975),
  [5344550e5](https://github.com/Parav1s/aspia_no2fa_cmd/commit/5344550e5),
  [52b635215](https://github.com/Parav1s/aspia_no2fa_cmd/commit/52b635215),
  [320116a12](https://github.com/Parav1s/aspia_no2fa_cmd/commit/320116a12),
  [90cb82c01](https://github.com/Parav1s/aspia_no2fa_cmd/commit/90cb82c01),
  [2e5d6ec3b](https://github.com/Parav1s/aspia_no2fa_cmd/commit/2e5d6ec3b)).
- Порт списка экранов из мода 3.1 с сохранением переводов и окна «О программе»
  ([84769ede5](https://github.com/Parav1s/aspia_no2fa_cmd/commit/84769ede5),
  [70c758f09](https://github.com/Parav1s/aspia_no2fa_cmd/commit/70c758f09),
  [77c730ac4](https://github.com/Parav1s/aspia_no2fa_cmd/commit/77c730ac4)).
- Захват через Windows Graphics Capture и следование активной сессии пользователя
  ([775f81c7d](https://github.com/Parav1s/aspia_no2fa_cmd/commit/775f81c7d)).

## Установщик и сборка

- Установка обновляется поверх уже установленной версии, в том числе той же `3.0.37`; издатель в
  установщике и автор форка в «О программе» — Paravis
  ([b6f6895c2](https://github.com/Parav1s/aspia_no2fa_cmd/commit/b6f6895c2)).
- Удалена поддержка Android
  ([2ce5b1e92](https://github.com/Parav1s/aspia_no2fa_cmd/commit/2ce5b1e92),
  [9784a9134](https://github.com/Parav1s/aspia_no2fa_cmd/commit/9784a9134),
  [1933d74e6](https://github.com/Parav1s/aspia_no2fa_cmd/commit/1933d74e6)).
- Сборки Windows x86/x64, macOS и Linux на hosted runners GitHub Actions.
- Новые строки переведены на все 24 языка интерфейса
  ([862a8d7ee](https://github.com/Parav1s/aspia_no2fa_cmd/commit/862a8d7ee)).
- Скрипт сборки умеет собирать только затронутые компоненты и обновляет `sha256-3.0.37.txt`.
- README: предупреждение о локальной сети, дисклеймер и упор на Windows
  ([1a46245d6](https://github.com/Parav1s/aspia_no2fa_cmd/commit/1a46245d6),
  [699e17658](https://github.com/Parav1s/aspia_no2fa_cmd/commit/699e17658),
  [66aab5cf4](https://github.com/Parav1s/aspia_no2fa_cmd/commit/66aab5cf4),
  [aa88d1645](https://github.com/Parav1s/aspia_no2fa_cmd/commit/aa88d1645),
  [7d15be724](https://github.com/Parav1s/aspia_no2fa_cmd/commit/7d15be724)).
