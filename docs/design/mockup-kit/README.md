# Набор для макетов окна BreezeCAD

HTML-макеты 1440×900 в светлой теме (цвета — из `src/ui/Theme.cpp`, иконки — `assets/icons`, шрифт IBM Plex).
`gen.py` — детали окна (шапка, лента, дерево, вид, строка состояния), `build.py` — сами макеты, `run.sh` — сборка в PNG.

```bash
bash docs/design/mockup-kit/run.sh            # HTML → PNG в build/mockups/html
RENDER=1 bash docs/design/mockup-kit/run.sh   # плюс заново посчитать картинки модели (~10 мин)
```

`badge-*.svg` — значки операций для дерева (макет ветки `design/tree-header`), в `assets/icons` попадут только после согласования.
