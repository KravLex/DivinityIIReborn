# Передача работы другой нейросети: Mage Skills Expansion (Divinity II: Developer's Cut)

Дата: 02.11.2026. Отвечать пользователю **по-русски**. Перед работой прочитать `CLAUDE.md` и `HANDOFF.md` в корне
проекта (раздел 2d — подробная хроника) и `MageExpansion/docs/RESEARCH.md` (§3 таблица адресов, §10 Frost Nova/свои навыки).

## 1. Правила пользователя (соблюдать строго)
- Геймплей менять можно (решение 03.11.2026: баланс, ошибки, описания входят в мод); не трогать оригинальные `.dv2` и `.exe`: только внешняя DLL (Div2Fix, `bin\dinput8.dll`)
  и свой архив-overlay `Data\Win32\Packed\Reborn\MageExpansion.dv2` (подхватывается раньше патчей игры).
- Не выдумывать адреса: каждый патч Div2Fix сверяет байты (`same(m, VA, "hex")`) и пишет причину пропуска в `Div2Fix.log`.
- Сначала разведка и отчёт, потом код; перед крупными шагами спрашивать.
- Не говорить «готово», пока пользователь не проверил в игре.
- Файлы пользователя не удалять. Пользователь сам запускает игру и присылает скриншоты (папка Steam:
  `C:\Program Files (x86)\Steam\userdata\245082663\760\remote\219780\screenshots`, их можно смотреть самому).
- Для Frost Nova пользователь потребовал: **свой навык с нуля**, без кода, вида и анимаций других навыков.

## 2. Пути, сборка, установка
- Игра: `C:\Program Files (x86)\Steam\steamapps\common\divinity2_dev_cut` (`bin\Div2Fix.ini`, `bin\Div2Fix.log`).
- Проект: `C:\Users\NEPTUN\Downloads\Divinity2-Engine-Modernization`.
- Div2Fix: исходники `native/div2fix/src/mageskills.c` (весь код навыков), сборка
  `PATH="/c/Program Files/LLVM/bin:$PATH" cmd //c "native\\div2fix\\build.cmd"` → `native/div2fix/build/dinput8.dll`,
  установка — копия в `bin\dinput8.dll` (игра должна быть закрыта). Файлы `.c` — CRLF.
- Архив: `py -3 -I MageExpansion/tools/build_mage.py "<игра>" MageExpansion/build`, проверка
  `py -3 -I MageExpansion/tests/check_archive.py "<игра>" MageExpansion/build/MageExpansion.dv2` (должно быть
  `all files OK`), установка `MageExpansion\install\install.cmd`. Конфиг навыков: `MageExpansion/config/skills.json`.
- Exe грузится с переносом базы (VA в логе = VA + 0x250000); в коде `VA(m, addr)`.

## 3. Что готово и проверено в игре
**Ice Bolt («Ледяной снаряд»)** — полностью: UUID `Chain Hit` (id 14), код Fireball, клетка дерева (6,2), своя иконка
(`tools/icon_art.py`), ледяные эффекты (перекраска Fireball), замедление и заморозка (Div2Fix `Slow14`, `Freeze14`),
15 рангов (уровни 3…41), учителя продают потолок 10/15 (Div2Fix дописывает записи в списки учителей после загрузки
сейва — списки учителей хранятся в сохранении, `0x8A0640`), имя строки у учителя по-русски.
**Дерево навыков расширено**: Div2Fix `TreeSheet=0` (каждая клетка показывает свою иконку), строка X=6.
**Frost Nova («Морозная вспышка»)** — UUID `Newt` (id 0x39), `CodePath="Newt"`, клетка (5,2), 15 рангов:
- Div2Fix `Own57=1`: перехват броска героя `0x923FC0` (у кода 0x39 нет ветки в `switch 0x924440`) → проверка
  `0x763AD0`, мана `0x764330`, перезарядка `0x89EC50` — **работает**.
- Поиск врагов вокруг (таблица `[0x14F13C4]+0x30`, позиция `0x755500`, отношение `0x886BF0` < 50) и магический урон
  (`0x7B9C00` формула + удар `0x981120`/`0x76C800`), урон = Param2, радиус = Param3 — **работает**.
- Свой эффект `FrostNova_Burst` с нуля (`tools/fx_frostnova.py` + библиотека `tools/nif_write.py`: кольцо-полоса,
  трещины-ленты, дымка, 30 ледяных пирамид, вспышка, 5 своих текстур) — **вид принят пользователем**, но проверялся
  только через вспышку броска Ice Bolt (`Fx14=…,FrostNova_Burst` в INI). Сейчас INI вернули к `IceBolt_Cast`.

## 4. Открытая задача: своя анимация броска Frost Nova (на ней остановились)
Цель: при применении Frost Nova герой играет **свою** анимацию и на нём показывается эффект `FrostNova_Burst`.
Эффект на герое через компонент `0x14F2FAC` (`0x741140` + запуск `0x74FD10`) **без анимации не показывается**
(проверено даже на игровой вспышке). Вспышка Fireball (`0x95E0A0`) делает: `0x739AF0(comp, &std::string имя анимации)`,
`0x739B00(comp, 0)`, `0x741140(...)`, `0x74FD10(...)` — это реализовано в Div2Fix (`own_effect`, INI `OwnFx57`, `OwnAnim57`).

Сделано для анимации (инструменты рабочие):
- `tools/kfm.py` — KFM 2.2.0.0b, все 121 KFM игры пересобираются байт в байт; `add_sequence(None, file)` даёт id = max+1.
- `tools/skeleton.py` — скелет `Skeleton.nif`, FK (мир: +X левая сторона героя, −Y вперёд, +Z вверх), картинки.
- `tools/anim_frostnova.py` — анимация `Skill_FrostNova` 1,4 с, ключ `Cast` в 0,8 с (позы проверены на картинках).
- Сборщик `animations()` в `build_mage.py`: `.kf` для Ж/М в `Win32\Characters\Human{Female,Male}\Skill_FrostNova_{F,M}.kf`,
  запись в 14 KFM игрока `Player_{F,M}_Base[_1H|_1HSH|_2HSW|_BOW|_DW|_UA].kfm`, плюс строки в
  `Win32\Characters\CharacterAnimationData.xml` (анимация в каждом наборе, счётчик `Animations` +1) и
  `Win32\Characters\Sizes.xml` (размеры .kf). Включается полем `own_animations` в `skills.json` (сейчас переименовано в
  `own_animations_off` — выключено).

Результаты в игре:
1. Номер последовательности 900 → **модель героя пропала**.
2. Номер max+1 → пропала.
3. + строки в `CharacterAnimationData.xml` и `Sizes.xml` → модель есть, но **застыла** (анимации героя не играют).
4. Тот же `.kf` вообще без ключей (все кости в покое) → **так же застыла** ⇒ виновата регистрация, не ключи.
Проверено, что анимаций героя нет в `MdlManBinary.nif` (CKFMDescriptor = имя KFM + группа свойств), `TemplateStats.xml`,
`ItemDescriptors.xml`, `rpgstats_animation_states.xml`. Загрузчик `CharacterAnimationData` (`0xFE3BE0` Entry, `0xFE56F0`
Animation) только читает поля. Порядок анимаций в `CharacterAnimationData` ≠ порядку в KF и KFM (сопоставление, видимо,
по имени или хешу). Сейчас откат: архив `715a312e…` без анимации, `OwnAnim57` закомментирован — игра в норме.

Следующий шаг (предложен пользователю, он согласен): в Div2Fix перехватить загрузку набора анимаций героя
(NiActorManager / загрузка KFM и последовательностей, `0x739B10`… — найти по строкам `MdlManDataManagerProcessAnimation`,
`Skeleton.nif`, вызовам чтения KFM) и залогировать, на каком шаге отвергается новая последовательность. Гипотезы:
движок проверяет число/порядок последовательностей против своих таблиц; новая последовательность в отдельном .kf (у
игрока все 57 — в одном `Player_F_Base.kf`) — возможно, нужно класть её **в тот же .kf** набора (дописать блок
последовательности в `Player_*_Base*.kf`, индекс = число последовательностей); или переходы `type 5` нужно заменить на
игровые (`type 2` с длительностью). Самый дешёвый следующий опыт: дописать последовательность в сам
`Player_F_Base.kf` (нужна поддержка B-spline-блоков как raw — `nif_write` их хранит как raw, пересборка байт в байт работает).

## 5. Важные ошибки, которые уже совершались (не повторять)
- Текстуры для `icon_art.mip_chain`/`dxt5` должны быть **uint8 0–255**, float 0–1 дают чёрную текстуру.
- `CStreamableAssetData` в `.item`: поле длины = размер блока − 17 (иначе вылет при загрузке).
- Для нового эффекта нужны строки в 5 списках (`rpgstats_effectprototypes`, `effectassetdatadescriptors`,
  `Effects\Sizes.xml`, `Effects\ItemDescriptors.xml` (границы), `effectanimationdata.xml` (имя последовательности, длина)).
- Плоские квады эффекта нужно рисовать с двух сторон: `NiStencilProperty` flags 0x4D80.
- В bash-heredoc'ах Python обратные слэши ломаются — писать скрипты файлами (Write) или через `chr(92)`.
- Полный перебор всех архивов игры очень медленный и жрёт память — открывать только нужный `.dv2`.

## 6. Итог 03.11.2026: анимация и эффект Frost Nova работают (проверено пользователем в игре)
- Причина «застывшего» героя: последовательность дописывается в `Player_{F,M}_Base.kf`, а размер файла в
  `Win32\Characters\Sizes.xml` оставался старым. `build_mage.py` теперь обновляет `m_Size`/`m_TotalSize`
  (и сумму родителя), `check_archive.py` считает `m_Size` счётчиком.
- Эффект шёл из руки: 3-й параметр `0x741140` — режим привязки (switch `0x9824DC`, таблица `0x982750`):
  1 = `Dummy_Cast_Primary` (рука), 0 = сам персонаж (позиция и поворот), 7 = переданный transform.
  В `own_effect` теперь 0 — эффект на земле вокруг героя.
- 03.11 (не проверено в игре): новая анимация 1,6 с (присед с IK ног, корпус, голова, кисти, пальцы, Cast 0,86 с);
  эффект Burst 4,66 с — сгустки у ладоней по траектории анимации, удар с CAST_AT, летящие осколки, снежная пыль, тает
  3,5 с; эффект на врагах `FrostNova_Chill` (`OwnHitFx57`); урон в момент удара (`OwnDelay57=860`, очередь в `hk_pump`);
  свой звук `tools/sfx_frostnova.py` → `bin\MageExpansion\FrostNova.wav` (`OwnSound57`, PlaySound, не 3D).
  Компонент эффекта держит один эффект (`0x741140` удаляет прежний `[esi+38h]`). Бэкап INI: `bin\Div2Fix.ini.bak_20261103`.
- 03.11 (проверено в игре): яркий круг на земле вокруг героя давал не эффект, а способ запуска. Если первый вектор
  `0x741140` (→ `[obj+50h]`) не равен глобальному `[0x14EF5CC]`, привязка `0x9827B0` вызывает у экземпляра
  `0x8100E0(7, …)` — подсветка земли. Div2Fix передаёт глобальный вектор (как вспышка Fireball), круг пропал.
  Найдено перебором (Div2Fix умеет `OwnFx57=A,B,C` — по касту на вариант; `fx_frostnova.build(dark=[...])`).
- Ещё 03.11: эффект пропадает целиком, если в файле есть блоки без родителя или у детали масштаб 0 (теперь MIN_SCALE);
  метка `HasEmittance` убрана со всех деталей; плоский слой инея убран (не ложится на рельеф); облака пыли и дымка у
  ладоней убраны (висели в небе); клинки — волна из 5 колец (4 внутренних — одна деталь на кольцо), непрозрачные
  (`NiAlphaProperty 0x12ED`, z-write); ключ `Cast` из анимации убран (не нужен); строки описания эффекта — с `FireBall_Cast`.
  Вспышка удара возвращена: до 1,4 м, 0,3 с, у ладоней.
- 03.11 (проверено в игре): каст на бегу. Перед заменой действия игра спрашивает `0x8FD5C0(current, newType)`: при
  `[comp+0Ch] != 0` прервать нельзя. Div2Fix ставит этот байт компоненту каста на `OwnLock57=2150` мс и снимает в
  `own_tick` — герой останавливается и доигрывает каст. Типы текущего действия (`[[stats+C4h]+0Ch]+8`): стоя 19, бег 26.
- Кольцо и трещины подняты на 14 см (`LIFT`), цвет `GROUND_TINT` как у клинков.
- Звук: волна клинков (залп треска на кольцо в `t = CAST_AT + 0.55*far`, свист, хруст при уходе в землю); тоны
  убраны/укорочены (колокол и долгие «пинги» звучали металлически).
- 03.11 (проверено в игре): учителя. Ice Bolt и Frost Nova: потолки 10/13/15 (1500/5000/10000 золота, уровень
  17/27/32, как Magic Missile). Учитель предлагает покупки навыка в порядке записей: Div2Fix после загрузки дописывает
  недостающие (проверка по паре UUID+потолок) и сортирует записи навыка по потолку (сырые 0x30 байт).
- Баланс 03.11: Ice Bolt Param3 = max(старое, 0.6 × урон Fireball на том же требуемом уровне), Frost Nova Param2 =
  max(старое, 0.9 × Fireball); 15 ранг 1115 и 1670 (Fireball 1858).
- 03.11: баланс игровых навыков — `config/balance.json` (операции mul/add/linear/set по номеру параметра, мана,
  перезарядка, desc_en/desc_ru), применяет `build_mage.balance()` в обоих эпизодах + перевод; `check_archive`
  проверяет, что меняются только разрешённые поля навыков из balance.json. Источник — гайд Mandragorko (Steam 2914998758).
  Не сделано: ошибки в коде (неуязвимость после Подарка следопыта; мана прыжка после Тысячи ран с Div2Patch),
  описание Уловки следопыта (нужна проверка в игре). Frost Nova: радиус 6/9/12 м, эффекты FrostNova_Burst6/9/12,
  Div2Fix `OwnFx57=Имя@ранг,...`.
- 03.11 (не проверено в игре): призыв нескольких существ. `0x8CC210` добавляет призванного в список хозяина
  (`[ctrl+48h]`, 8 байт: handle, время) и при count > 1 обнуляет время самого старого (`0x8CC288 cmp ecx,1`).
  Param2 Мастерства призыва игра не читает нигде. Div2Fix `SummonLimit=1`: jmp на `summon_stub`, лимит = Param2
  ранга Мастерства хозяина (без навыка 1). Хозяин: `0x8C9330(0x8C92C0([ctrl+20h]))`, индекс `[obj+4]&0xFFF`,
  `0x818370`. balance.json: Param2 1/2/3/4 (ранги 1-4/5-9/10-14/15). Разрушение: уровни 10..40, покупки 10/13/15.
- 03.11 (проверено в игре): несколько призванных существ ломают игру. Два живых существа хозяина (даже разных видов)
  -> переполнение стека (глубокая рекурсия; вершина стека — распределитель 0x5FE206). Два от одного навыка — то же
  (одна ячейка [comp+18h]). Div2Fix `SummonLimit=0`, `SummonRecast=0` (код оставлен). Мастерство призыва — только сила.
  TrainerTrace=0. ИСПРАВЛЕНО позже: причина — рекурсия BucketID, BucketGuard; SummonLimit=1 снова включён. Учителя: новые покупки только в конец списка (скрипты открывают по номеру).
- 03.11: вылет при телепорте из башни (и, вероятно, с двумя существами): бесконечная рекурсия 0xB929A0 (тип 0x11:
  нет extra data "BucketID" -> 0xB92750 раздаёт её только мешам -> повтор через vtable+14h). Div2Fix crashguard.c
  `BucketGuard` (ini [General], по умолчанию 1): повтор только если BucketID появилась. CrashLog: отчёт о переполнении
  стека теперь пишется из отдельного потока (самые частые адреса возврата = петля).
- 03.11: задержка первого призыва. Замер (перехват 0x960950/0x960A30, потом убран): 1672 мс от старта действия до
  0x960A30 (анимация), создание <1 мс, существо сразу в списке хозяина (exists, active), но видно через 5-10 с.
  Значит, задержка в загрузке ресурсов (модели/текстуры), вероятно, той же природы, что и прозрачная земля. Не от
  времени жизни (×1.5 и ×1 — одинаково) и не от памяти (свободно ~2,2 ГБ). Перехват 0x960950 вызвал вылет
  (0x95AC09, ecx мусор) — возможно, вызывающий код полагается на регистры; эту функцию не перехватывать detour'ом.
  SummonRecast=1 в игре работает (3 демона, 4-е нажатие отзывает всех).
- 03.11 ИТОГ (проверено в игре): задержка первого призыва, прозрачная земля и вылеты 0x95AC09 (в т.ч. с перехватом
  0x960950 и при призраке после трёх демонов) — одна ошибка Div2Fix: toggle_stub писал 6 байт в 0x9609BA, а переход
  игры jne/je 0x9609BE попадал внутрь jmp. Теперь перехват 0x9609AF..0x9609BD (15 байт), возврат на 0x9609BE.
  Время жизни и память ни при чём. SummonRecast=1 работает. SummonCooldown=1 (по умолчанию): в summon_limit
  новая запись списка -> [существо+1C8h] = навык -> 0x753330 -> 0x89EC50 (игра сама перезарядку призыва не запускала).
  balance.json: время жизни существ исходное (решение пользователя), у призрака лечение ×1.5.
- 03.11: прозрачная земля вернулась при трёх существах разных видов (без повторного призыва, исключений нет).
  В bin\dxvk.conf выключен d3d9.asyncManagedUpload (патч DXVK 0003; копия dxvk.conf.bak_20261103) -> земля нормальная
  (один запуск). Подозрение: загрузка managed-текстур на CS-потоке в 0003 (пин копии + packImageData позже) и
  skipManagedTextureInit (0001: изображение не обнуляется до загрузки). Патч не разобран.
  Портреты призванных: summons.gfx в архиве, 3 байта (build_mage.SUMMONS_LEFT), ряд растёт влево — проверено.
- 03.11: прозрачная земля НЕ от призыва и НЕ от DXVK 0003/0004 (бывает и с asyncManagedUpload=False, ~1 из 7).
  Сценарий: загрузка -> башня (арена) -> камень дракона -> локация. Лог BucketGuard: каждый кадр одни и те же 6
  объектов 'Editable Poly' (это сами сетки земли) без BucketID, геометрий внутри 0. Причина в 0xB92750: у сетки
  [geom+64h] == NULL (b927c3 -> выход без выдачи BucketID), т.е. у сетки нет данных/свойства. В оригинале это
  бесконечная рекурсия = тот самый вылет при телепорте; BucketGuard превратил вылет в невидимую землю. Дальше:
  выяснить, что такое [geom+64h], кто его ставит и почему после телепорта он пуст.
- 03.11 КОРЕНЬ прозрачной земли / вылета при телепорте (ПРОВЕРЕНО: 11 заходов, земля на месте): сетки 'Editable Poly' — NiGeometry
  со свойствами, BucketID у них ЕСТЬ, но 0x4DB330 не находит: extra data отсортированы по указателю имени, а 0x4DB110 /
  0x4DB330 / 0x4DB3D0 сравнивают знаково (sub/test/jle). Игра LAA: строки выше 2 ГБ ломают порядок. Div2Fix
  crashguard.c ExtraDataOrder=1 ([General]): jle -> jb в 0x4DB207/0x4DB399/0x4DB439. Версия с UpdateProperties
  (0x552A50) была неверной и убрана. vtable сеток 0x164FB80 вне exe (модуль пишется в лог).
- 03.11 разведка Div2Patch: он ЗАМЕНЯЕТ bin\Divinity2.exe целиком. Steam: 1.4.700.49 (ts 0x5069AB57, бэкап в
  Div2PatchBackup\bin), Div2Patch: 1.4.700.56 (ts 0x50A3A9B6, +секции .text2/.rdata2, LAA 0x122, импорт Div2Patch.dll).
  Код .49 и .56 различается по существу с ~0x797000 (650 КБ не объясняются сдвигом данных +0x50), т.е. все адреса
  Div2Fix (0x8CC210, 0x960950, 0xB92A00...) верны только для .56. Также меняет Data\globalswitches.xml. Лицензии/
  разрешения на распространение не найдено (exe Larian + код BlanknameES).
- 03.11 Chain Lightning (id 0x1C, Div2Fix Name28 -> 0x89D560; CodePath Newt; клетка (6,3)): механика работает (урон,
  прыжки, -20%). Визуал: «компонент эффекта» персонажа = его текущее действие (0x74FA10 / 0x8FD5C0): на герое мешает
  OwnLock, у врагов ИИ сразу заменяет -> эффекты на врагах (в т.ч. FrostNova_Chill) не видны. Луч теперь свободный
  эффект в мире: 0x810BA0(менеджер [0x14F170C], &name, 0, &id) + 0x810060 позиция + 0x810270 поворот + 0x80FFC0(1).
  Луч D_LTower_Beam не годится (нужен шейдер div2_FF_LaserBeam, его exe даёт только этому имени) -> свой fx_chain.py,
  луч вдоль -X. Виден, направление верное (скриншот 03.11). Не сделано: звук, иней Морозной вспышки на врагах тем же путём.
- 03.11 Chain Lightning: родные звуки через Wwise-обёртку игры (менеджер [0x14F13F8]: 0x6EB2B0 рег. объекта,
  0x6EB2D0 позиция, 0x6EBE90 событие; как метеор 0xA606A5) -> ВЫЛЕТ (EIP 0x6EB200, read FFFFFFFF, после вызова
  0x6EB2D0; и при касте, и при ударе). Выключено (;OwnCastSound28/;OwnStrikeSound28), синтезированный WAV снова
  включён. Не разобрано: аргументы/поток. Также: бирюзовый MagicMissile_Impact убран (пользователь); цели без трупов
  (0x7563B0 IsDead) и без перепада >3 м.
- 03.11 Chain Lightning, итог (проверено в игре): первая цель = цель игрока [[stats+FCh]+44h] (как Fireball, 0x9244FF),
  иначе ближайший враг 15 м; прыжки к ближайшим. Правила цели own_enemy (как поиск игры 0x724E30): active, не bit21,
  не IsDead (0x7563B0), здоровье [stats+18Ch] > 0, объект vtable+6Ch байт +2 (на сцене), враг с битом 15 (0x7565C0);
  перепад высоты <= 3 м. Теми же правилами теперь бьёт Морозная вспышка (не проверено). Лучи: свободные эффекты
  0x810BA0/0x810060/0x810270 БЕЗ 0x80FFC0 (флаг -> 0x792050 -> вылет 0x7864BA). Звук: assets/sounds/electric-spark-zap.wav
  (от пользователя, вероятно BudgetPixel CC BY 4.0 - подтвердить), каст: родной Skill_MagicBlast_Cast через
  [stats+224h] + 0x8F5910 (OwnCastSound28). Свои объекты Wwise (0x6EB2B0/0x6EBE90) не работают - убрано.
- 03.11 Ice Bolt, свой вид и звук (проверено в игре): полёт/попадание остаются кодом Fireball (Fx14). tools/fx_icebolt.py:
  Travel/Impact = эффекты Fireball с их системами частиц (nif_fx.tint SAT 0.9 x LIGHT 0.45, все текстуры заменены на
  ледяные: fx_icebolt_flake/mist/ring + Frost Nova), HasEmittance=0, части 'Ring' и 'Flash' Fireball скрыты
  (APP_CULLED: ударная волна на весь радиус взрыва и плоский квадрат); + свои части (кристалл, искры, осколки).
  Cast = только свои части (частицы каста давали белый шар). Своя квадратная текстура кольца: полоса Frost Nova на
  квадрате заливала пол. Для отладки: env MAGE_NO_BASE_<ИМЯ ЭФФЕКТА> собирает без частиц. Звук: Div2Fix CastWav14
  (0x95E8C4 вместо Skill_Fireball_Cast, id = [ebp+4]), assets/sounds/frost-spell-impact.wav (Pixabay DRAGON-STUDIO),
  обрезка SKIP 0.85 s, KEEP 2.6 s.

## 03–04.11: particle systems (checked in game by the user)
- `tools/psys.py add(nif, name, tex, emitter=..., rate, start, stop, life, speed, radius, colours, gravity, ...)` builds a
  Gamebryo 20.3 particle system. The emitter is driven by the effect's NiControllerSequence, as in 619 of the game's
  738 systems (GenericCast_A):
  - controller flags 104;
  - NiBlendFloat/NiBlendBool interpolators;
  - sequence items BirthRate/EmitterActive in `nif.psys_items`, which the encoders add.
  Running them on their own clock made the burst come late when an effect was replayed. Drag stopped world-space
  particles at once, so it is not used.
- Particles in: Ice Bolt (Travel, Impact, IceBolt_Trail), Frost Nova (FrostWave/FrostCrystals/FrostFog, Chill),
  Chain Lightning (beam Crackle/Ions/StrikeSparks; ChainLightning_Cast, OwnFx28; OwnAnim plays only with an own effect).
- Div2Fix trail: `OwnTrail14=IceBolt_Trail,50` (cm spacing).
  - Hook 0x969D1A → 0x7D37C0 (projectile pool [0x14F1714], handle [obj+1BACh]).
  - Position [obj+B4h], confirmed in game.
  - TrailTrace=1 logs the live effects.
- Div2Fix free effects are released by Div2Fix itself: manager vtable+34h(id), as 0x810C16 does; slot checked in
  [mgr+30h]. The game never removed them (564 alive, fps fell). Times: puffs 950 ms (the game replays a free effect
  after ~1 s), beams 1000 ms, strike fx 4000 ms.

## 04.11: Arcane Push (id 0x36) — on hold by the user, in the game as is
Works in game: damage, push 6-12 m, own animation, sound (licence TODO), `ArcanePush_Burst` wave. Knockdown: did not work on every enemy; the last fix (the name by weapon via 0x752230, stopping the action via vtable+20h) is installed but NOT checked in game. Next free skill id: none (0x1C, 0x36, 0x0E, 0x39 used); new ids above 0x54 not researched. Arcane Blink: `config/skill_arcane_blink_shelved.json`, needs its own id now.

## 04.11: Arcane Shield (new skill id 0x60 = 96)
Works in game: tree, cast, absorb and mana by `hk_takeHit` (0x76C800). Skin: `OwnShieldSkin96=2` = 0x8FCC70 picked by the user (third application of the test); 0 = 0x8FCB70, the dragon's, sweeps from the feet up. `OwnShieldPulse96=0`. Not checked: a save with id 96 loaded again; poison and burning through the shield.

## 04.11: loading a save of the other campaign in one run (open bug)
Profile `Registry.xml` GameType selects the campaign at start. Loading a save of the other campaign re-registers the archives (Overlay round 2) and rebuilds the prototype table, and that table has the game's skills only (logged ids 81, 84 of Episode_2, no 96), though `MageExpansion.dv2` is registered first in round 2 too; `Order=last` did not help. A save without our skills: they are just missing (`no prototype`, table 81). A save with them: crash at 0x89F9B0 (null prototype, [ecx+24h]; caller 0x8A010A). GameType is saved anyway, so the next start works. Why round 2 does not read our archive: unknown.

## 05.11: old game bug, on hold: jump attack after Thousand Strikes (id 0x1D; Jump Attack 0x1F)
Mana Leech gives mana, the end of the animation takes it, the camera spins. 0x7432C0 (ranged) not involved. 0x959C10, reached from action start 0x95C210 (type 71, every character), pays 0x1D at 0x959D45 and sets 0x945Exx values; called often, for several actions, so "it runs twice" is NOT proven. Trace: `MeleeTrace=1`. Jump Attack `EnduranceCost` is 0 now (balance `cost_set`).

## 05.11: necrosaur torso has no shadow (census 20261105_005027/005056, open)
Worn: DragonElf head, legs, torso, Froblin arms. World shadow map R11 (8192 VSM) has MED head 704, legs 766, arms 752 (skinned), NOT torso MED 518; main pass has torso HI 942 with its own VS `?76c896320468` (others `?a2e89a16f439`). R15 (4096 R32F, `_VSM_Char`) holds a separate set (982 rigid, 6 skinned 14222, alpha 2012), apparently the hero only: this is why others' shadows are weaker. No difference found in the part files (flags, extra data, properties). Next: the code that chooses shapes for the shadow pass; why the torso's main VS differs.
Filter 0xB8D2C0 (`ShadowTrace=1`, logs): its list holds top level objects, mostly `DynamicAsset Dummy` with category masks ([+ACh] 0x400, 0x800, 0x200000, 0x20) and lights, not body parts; the parts are chosen further in, by those objects' vtable+0BCh. On hold by the user's decision if not continued.
Third trace pass: the 40-line limit was spent on masks 0x20000 and 0x40000; 0x40000 objects (vt live 0x16C2AC0, likely terrain) have another layout (junk at [+11Ch]), and the character's container was not reached. Not established which class holds characters; the [+11Ch]/[+130h] layout is known only from 0xB4C9C0's classes (masks 0x200000, 0x20, 0x800).

## 05.11: confirmed in game by the user
- New skill ids above 0x54 work: id 0x60 (Arcane Shield) keeps its rank through save and load. Other save/load of the same campaign only; see the campaign switch bug.
- Arcane Push knockdown works (name by weapon via 0x7521E0).
- Necrosaur torso shadow fixed: `SkinnedAlphaShadow` (crashguard.c): the VSM cascades leave the renderer's skinned alpha slot +18h empty, such parts were skipped (0xBD57DA).
