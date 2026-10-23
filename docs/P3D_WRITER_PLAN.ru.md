# План: завершение `.p3d`-писателя UI-контента (референс форматов)

> **Цель:** закрыть TODO из `SPEC.ru.md` §15 («свои экраны / UI-слой после
> генератора .p3d») доведением реализации writer'а до эталонных спецификаций
> формата `.p3d`:
>
> **Источники (внешние, не наш код):**
> - спецификации чанков Pure3D (структуры, `p3d.json`-схема) и ориентиры по
>   кодогенерации;
> - порт исходной платформы под PSP, дающий точные структуры
>   IMAGE/TEXTURE/IMAGE_DATA, чанк-кодек с round-trip и идею VBO-кеширования
>   геометрии.
>
> Используем ТОЛЬКО как референс формата/идей. Код **не копируем целиком** —
> у нас своя архитектура (Scrooby runtime + собственный движок), берём факты о
> формате.

---

## 1. Текущее состояние (что уже есть у нас)

### Читатель (полный, есть)
`src/libs/scrooby/src/FeChunkHandler.cpp` — читает все фронтенд-чанки:
- `LoadScreenChunk` (0x18001), `LoadPageChunk` (0x18002),
  `LoadLayerChunk` (0x18003), `LoadGroupChunk` (0x18004),
  `LoadTextChunk` (0x18007 + sub-chunks 0x1800B/0x1800C),
  `LoadSpriteChunk` (0x18006), `LoadPolygonChunk` (0x18009),
  `LoadMovieChunk`, `LoadPure3dObjectChunk`, `LoadObject`.

### Писатель (минимальный, есть, но ограничен)
`src/libs/pure3d/p3d/p3dwriter.{h,cpp}` — асинхроннодендущйся writer:
- Корень: `DATA_FILE` (0xFF443350); заголовок чанка `[id u32][dataLength u32]
  [chunkLength u32]`.
- Умеет: `BeginProject`, `BeginScreen` + `AddScreenPageName`, `BeginPage`,
  `BeginLayer`, `BeginGroup`, `WriteText` (+ `AddTextHardCodedString`/
  `AddTextBibleString`, `EndText`), `WriteSprite` (+ `EndSprite`), `EndProject`.
- **Чего не хватает:** текстур/шрифтов (`TextureFont` 0x22000, `Image` 0x19001,
  `Sprite` с настоящими картинками), `FrontendPolygon` персистентно,
  ресурсов/TextStyle/TextBible, валидации против реального загрузчика.

### Тест-хук (выключен, был с AV)
`LobbyRuntimeTest` подменял Scrooby-проект фронтенда на этапе старта и давал AV в
реальном движке. Подмена проекта — рискованна (несовместимость слова/слоя).

---

## 2. Что берём из `plowteam/donut`

1. **`dev/codegen/p3d.json`** — авторитетная машинная схема ВСЕХ чанков Pure3D
   с точными типами. Для фронтенда ключевые:
   - `FrontendProject` (0x18000), `FrontendScreen` (0x18001),
     `FrontendPage` (0x18002), `FrontendLayer` (0x18003),
     `FrontendGroup` (0x18004),
     `FrontendMultiSprite` (0x18006), `FrontendMultiText` (0x18007),
     `FrontendStringTextBible` (0x1800B), `FrontendPolygon` (0x18009),
     `FrontendImageResource` (0x18100),
     `TextureFont` (0x22000) + `FontGlyphs`, `Texture` (0x19000),
     `Image` (0x19001) + `ImageData` (0x19002), `Sprite` (0x19005).
2. **`dev/Chunks.md`** — человекочитаемое описание тех же полей (удобно
   сверяться по типам: u32/s32/string/vec3/mat4/buffer/children).
3. **`dev/codegen/donut-codegen.exe` + `cmd.json`** — идея кодогенерации:
   из одной JSON-спецификации генерить и reader, и writer. Нам это полезно
   как подход, не как код.

---

## 3. Проверка нашей схемы против donut (контрольный список «дьявол в деталях»)

Сверка нашей `P3DWriter`/`FeChunkHandler` с `p3d.json` по точным типам:

| Чанк | Наше поле | donut `p3d.json` | Что проверить |
|------|-----------|------------------|---------------|
| `FrontendMultiText` | `x,y` (u32) | `positionX,positionY` : `s32` | знак/порядок |
| `FrontendMultiText` | `dimensionX, dimensionY` | `dimensionX, dimensionY` : `u32` | порядок полей |
| `FrontendMultiText` | `justificationX, justificationY` | `alignX` → `alignY` (порядок) | НЕ перепутать X/Y и порядок |
| `FrontendMultiText` | `index` (версия>16) | `current` : `u32` | после shadow-блока |
| `FrontendMultiText` | shadow: `shadowEnabled u8, shadowColor u32, shadowOffsetX/Y s32` | `shadowEnabled:u8, shadowColor:u32, shadowOffsetX:s32, shadowOffsetY:s32` | точный порядок |
| `FrontendMultiSprite` | `x,y, dimensionX,Y, colour, translucency, rotation, imageNames` | `positionX/Y:s32, dimensionX/Y:u32, alignX/Y:u32, color:u32, translucent:u32, rotation:float, numImages, imageNames:string[]` | соответствие полей |
| `FrontendProject` | `name,version,resX,resY,platform,pagePath,resourcePath,screenPath` | те же | порядок |
| `FrontendLayer` | `name,version,visible,editable,alpha` | `name,version,visible:u32,editable:u32,alpha:u32` | visible/editable перечислены как u32 |
| `FrontendPolygon` | `numPoints, points:vec3[], colors:u32[]` | те же | buffer типы |
| Chunks контейнеры | `buffer<...>` | `children`/`buffer` | как кодировать количество |

> Вывод: по большинству полей наша схема совпадает с donut, но **нужно
> построчно сверить порядок и знаки типов** по `p3d.json`, прежде чем
> расширять writer. Это и есть главный риск.

---

## 4. Пошаговый план работ

### Фаза A. Сверка и чистка текущего writer'а
1. Принести `p3d.json` (из donut) как эталонный источник в `docs/` или отдельный
   файл схемы — НЕ в git-код, а как документ (`docs/p3d_format.md`, с ссылкой
   на upstream и датой).
2. Написать юнит-тест «round-trip»: собрать минимальный проект `P3DWriter`'ом
   → прочитать его нашим `FeChunkHandler`/`chunkfile` → убедиться, что reader
   проходит без `rAssert` и поля совпадают (позиция, размер бокса, выравнивание,
   строка, shadow, индекс).
3. Исправить расхождения между нашим читателем/писателем и donut-схемой
   (типы, порядок полей). Каждое исправление — с тестом.

### Фаза B. Расширение writer'а до полного фронтенда
4. Добавить в `P3DWriter` недостающие элементы по `p3d.json`/`Chunks.md`:
   - `FrontendPolygon` (0x18009) — запись и чтение.
   - `FrontendObject` (0x18008) — имя (placeholder).
   - `FrontendImageResource` (0x18100) — filepath.
5. Добавить поддержку текстур/шрифтов (сложнее всего):
   - `Texture` (0x19000) + `Image` (0x19001) + `ImageData` (0x19002).
   - `TextureFont` (0x22000) + `FontGlyphs` (0x22001) — нужен реальный шрифт,
     см. §6.
   - `Sprite` (0x19005) с внутренними `Image`.
   Это отдельная под-задача; без шрифтов UI не сможет выводить текст.

### Фаза C. Встраивание в игру (осторожно)
6. НЕ подменять существующий `frontend.p3d` вслепую (AV в `LobbyRuntimeTest`).
   Вместо этого:
   - добавить **диагностический экран/оверлей**, который загружает наш
     сгенерированный `.p3d` через штатный Scrooby-загрузчик в изолированном
     контексте и проверяет, что нет AV и элементы отрисовались;
   - либо добавить наш `.p3d` как **новый** Screen в существующий проект (если
     возможно по структуре), не трогая рабочие экраны.
7. Подключить к «своему UI-слою» (третий пункт TODO §15): наш оверлей IP уже
   работает кодом — навести полноценный диалог поверх, используя
   сгенерированный проект (поле ввода, кнопки Host/Join).

### Фаза D. Тесты и интеграция
8. Тесты (`src/tests/network_tests.cpp` или новый файл `src/tests/p3d_tests.cpp`):
   - round-trip writer→reader (см. п.2);
   - сериализация каждого нового чанка (по вх. данным в `p3d.json`);
   - загрузка сгенерированного проекта через Scrooby без крашей/ассертов.
9. Прогон: `MSBuild src/tests/Tests.vcxproj /p:Configuration=Debug /p:Platform=x64
   /p:WindowsTargetPlatformVersion=10.0.22621.0` → `vstest.console.exe`.
10. Сборка игры, ручная проверка оверлея/диалога.

---

## 5. Риски и миттигация

| Риск | Миттигация |
|------|------------|
| Порядок/знаки полей расходятся reader↔writer | Round-trip юнит-тест + сверка по `p3d.json` и `p3dopt/p3d.py` (round-trip byte-exact) |
| Подмена `frontend.p3d` → AV | Не подменять вслепую; грузить наш проект изолированно / как новый Screen |
| Шрифты/текстуры не воспроизводятся | Отдельная под-задача Фазы B; структуры IMAGE/TEXTURE/IMAGE_DATA см. §7.1; начать с hard-coded строк (уже работает) |
| GPL-3.0 код donut / SHAR-PSP | Не копировать код; использовать только факты о формате (идея/спека — не копия) |
| VBO-перенос в `primgroup.cpp` | Низкий приоритет, отдельным шагом; вынести из `RAD_PSP`, убрать `ms0:`-логи, гейт `mIndexCount>0 && mVertexList`, прогнать перф-тест |
| Большой объём | Делить на фазы; каждая фаза — коммит с тестами |

---

## 6. Про шрифты (быстрый взгляд)

Сейчас наш оверлей использует ассет-шрифт существующего текста (`MainMenu`).
Для собственного `.p3d` с текстом нужен `TextureFont` (0x22000): glyph-метрики +
`Texture`. Варианты:
- Переиспользовать существующий шрифт из ассета (скопировать чанк 0x22000 из
  оригинального .p3d через наш reader) — без внешнего файла.
- Либо внешний инструмент (Lucas' Pure3D Editor) сгенерит правильный шрифт.

---

## 7. Уточнение по итогам изучения PSP-порта исходной платформы

Доработано после детального сравнения с портом исходной кодовой базы под PSP
(порт на sceGU). Оттуда берём **только факты о формате и идеи**, не GPL-код.

### 7.1 Структуры IMAGE/TEXTURE/IMAGE_DATA (закрывает пробел текстур в writer'е)

Наш `P3DWriter::WriteSprite` (p3dwriter.cpp:263) пишет **только имена IMAGE**
(спрайт-фреймы), НО НЕ встраивает реальные пиксельные чанки. PSP-форк даёт точную
раскладку полей для дописывания:

```
IMAGE 0x19001:        // payload
  pstr name
  u32  version
  u32  width
  u32  height
  u32  bpp
  u32  palettized     // 1 если палитра
  u32  hasAlpha
  u32  format         // 1 == PNG (в PC-ассетах блобы PNG)
  child IMAGE_DATA

IMAGE_DATA 0x19002:   // payload
  u32  size           // размер blob, затем raw blob (PNG)

TEXTURE 0x19000:      // payload
  pstr name
  u32  version
  u32  width, u32 height, u32 bpp,
  u32  alphaDepth, u32 numMipMaps,
  u32  textureType, u32 usage, u32 priority
  children IMAGE(по одной на мип-уровень)
  // при смене IMAGE надо синхронно обновлять width/height/bpp/numMipMaps
```

### 7.2 Round-trip кодек (эталон для нашего writer'а)

PSP-форк `tools/p3dopt/p3d.py` — структурный чанк-кодек, который подтверждает
нашу схему заголовка (`[id u32][dataLength u32][chunkLength u32]`,
`dataLength=12+payload`, строки `[u8 len][bytes]` без NUL), и делает
round-trip **byte-exact** на frontend/camset/level. Полезен как **валидатор**:
если наш C++-writer+reader дают byte-exact на тех же файлах — схема верна.

### 7.3 VBO-кеширование в tPrimGroupStreamed::Display (применимо к PC, низкий приоритет)

Сравнение `src/libs/pure3d/p3d/primgroup.cpp`:
- Наша PC-версия `Display()` (строка 229) — **immediate-режим**:
  `BeginPrims(...)` → `Display(stream)` → `EndPrims(...)` на каждый пасс. Тот же
  код, что и вне-`RAD_PSP`-часть PSP-форка (наш совпадает с их эталонной частью).
- PSP-форк добавляет под `#if defined(RAD_PSP)` **статический VBO**: строится один
  раз `NewPrimBuffer` + `SetIndices`, далее каждый кадр `DrawPrimBuffer` вместо
  повторной отправки вершин (строки 335-363). Идея **платформонезависимая** —
  immediate-отправка дорога и на десктопе. Переносить вручную: вынести из-под
  `RAD_PSP`, убрать diag-`fopen("ms0:/...")`, гейт `mIndexCount > 0 && mVertexList`.
- `g_pspSkipReskin` (строки 458-467) к PC **не применим**: PC-skin идёт через
  `tPrimGroupSkinnedPC` (hardware skin на `mBuffer`), а флаг живёт только в
  `tPrimGroupSkinnedStreamed`, который на PC не задействуется.

> ⚠️ `tVertexList`-free правка, упомянутая в README PSP-форка, в `primgroup.cpp`
> вне `RAD_PSP` **не найдена** — вероятно, в `vertexlist.cpp`/другом файле.
> Проверять отдельно, только если критично.

### 7.4 Что НЕ берём
- Палитрование/децимацию текстур и геометрии (`p3dopt` levers) — нужно только
  для 22 МБ RAM PSP; на PC это потеря качества без выгоды.
- sceGU-рендерер, pspdev/`RAD_PSP`-обвязки — наша платформа другая.

---

## 8. Итоговая цель

На выходе: `P3DWriter` пишет полноценный `frontend`-проект (тексты, спрайты,
полигоны, опционально текстуры/шрифты), read-only round-trip гарантирован
тестами, и наш «свой UI-слой» (диалог ввода IP, кнопки) собирается чистым
кодом + сгенерированным `.p3d`, без подмены боевых экранов и без AV.