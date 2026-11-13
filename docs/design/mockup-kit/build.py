from gen import *
import os
os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)),'html'))
def W(name, html): open(name,'w').write(html)
base=[header(STATS_MS),modes(),ribbon_model(),rail(),tree_panel()]
W('00-now.html',page(base+[viewport('now.png'),tabs(),status(HINT,'<span>мм</span><span>Готово</span>')],'Сейчас'))
W('A1-live.html',page(base[:4]+[tree_panel(),viewport('live.png'),tabs(),status(HINT,'<span>мм</span><span>Готово</span>')],'Живой вид'))
# A2 operation preview
extr=f'''<div class="panel"><div class="ph">Выдавливание<img src="{icon('ui-close')}"></div>
<div class="sec">Профиль</div><div class="row sel" style="margin:0 12px;border-radius:6px"><span class="ic"><img class="m" src="{icon('tree-sketch')}"></span><span class="nm">Профиль 1 — прямоугольник</span><img class="st" src="{icon('ui-close')}"></div>
<div class="sec">Операция</div><div class="seg"><div><img src="{icon('op-join')}">Добавить</div><div class="on"><img src="{icon('op-cut')}">Вырезать</div><div><img src="{icon('op-intersect')}">Пересечь</div><div><img src="{icon('op-new')}">Новое тело</div></div>
<div class="sec">Размер</div><div class="fld"><span class="l">Расстояние</span><span class="v">6.4</span><span class="u">мм</span></div>
<div class="seg"><div class="on"><img src="{icon('dir-down')}">Вниз</div><div><img src="{icon('dir-up')}">Вверх</div><div><img src="{icon('dir-both')}">Симм.</div><div><img src="{icon('dir-asym')}">Несимм.</div></div>
<div class="chk"><i></i>Предпросмотр результата</div>
<div class="hint">Синий призрак — что добавится, красный — что уйдёт. Цвета — токены темы, в тёмной теме свои.</div>
<div class="btnrow"><div class="btns2">Отмена</div><div class="btnp">Применить</div></div></div>'''
call='''<div class="callout" style="left:330px;top:470px"><b>Вычитание.</b> Объём, который уйдёт, — красный, видно и спрятанную часть внутри тела.</div>
<div class="callout" style="left:720px;top:210px"><b>Добавление.</b> Призрак нового тела — синий, полупрозрачный, с контуром.</div>'''
W('A2-preview.html',page(base[:4]+[extr,viewport('preview.png',call),tabs(),status('Перетащите стрелку или введите расстояние · Enter — применить · Esc — отмена','<span>мм</span><span>Готово</span>')],'Предпросмотр операции'))
# B1 photo mode
photo=f'''<div class="panel"><div class="ph">Кадр<img src="{icon('ui-close')}"></div>
<div class="sec">Фон</div><div class="sw"><div class="on" style="background:linear-gradient(#F7F8FA,#D6DEE8)">Студия</div><div style="background:linear-gradient(#383C44,#1A1C21);color:#ddd">Тёмная</div><div style="background:repeating-conic-gradient(#E6E8EB 0 25%,#fff 0 50%) 0/12px 12px">Прозрачный</div></div>
<div class="chk"><i></i>Тень на полу</div><div class="chk off"><i></i>Сетка пола</div><div class="chk"><i></i>Рёбра</div>
<div class="sec">Свет</div><div class="sl">Мягкость теней<span class="t"><i style="width:65%"></i><b style="left:62%"></b></span></div>
<div class="sl">Поворот света<span class="t"><i style="width:30%"></i><b style="left:27%"></b></span></div>
<div class="sec">Размер</div><div class="seg"><div>1280×720</div><div class="on">1920×1080</div><div>3840×2160</div><div>Квадрат</div></div>
<div class="sec">Качество</div><div class="seg"><div>Черновик</div><div class="on">Хорошо</div><div>Максимум</div></div>
<div class="hint">«Хорошо» на слабом ноуте — секунды, «Максимум» — до минуты. Окно при этом не замирает: прогресс в строке состояния.</div>
<div class="btnrow"><div class="btns2">Копировать</div><div class="btnp">Сохранить PNG…</div></div></div>'''
frame='<div style="position:absolute;left:60px;top:28px;right:60px;bottom:28px;border:1.5px dashed rgba(29,78,216,.7);border-radius:4px;box-shadow:0 0 0 2000px rgba(244,245,247,.55)"></div><div class="callout" style="left:76px;top:44px">Рамка показывает, что попадёт в картинку 16:9. Камера — как в окне.</div>'
W('B1-photo.html',page([header(STATS_MS),modes('Вид'),ribbon_view(),rail(),photo,viewport('render.png',frame),tabs(),status('Кадр: 1920×1080, «Хорошо» · проход 48 из 64','<span>мм</span><span style="display:flex;align-items:center;gap:6px">Рендер <span style="width:90px;height:4px;background:#E1E4E8;border-radius:2px;display:inline-block;position:relative"><i style="position:absolute;left:0;top:0;bottom:0;width:75%;background:#1D4ED8;border-radius:2px"></i></span></span>')],'Режим «Кадр»'))
# C1 tree polish
pins='''<div class="pin" style="left:140px;top:-2px;position:absolute">1</div>'''
c1=[header(STATS_FPS),modes(),ribbon_model(),rail(),tree_panel(badges=True,chips=True,endmarker=False,hover=6),viewport('live.png','''<div class="callout" style="left:24px;bottom:24px;max-width:420px;position:absolute"><b>1</b> Значок операции: красный «−» — вырез, фиолетовый — пересечение, зелёный «+» — новое тело. Без приписок «(вырез)» в имени.<br><b>2</b> Фильтры дерева: всё / тела / эскизы / рабочие элементы / проблемы (со счётчиком).<br><b>3</b> В шапке — кадры в секунду вместо мс.</div>'''),tabs(),status(HINT,'<span>мм</span><span>Готово</span>')]
html=page(c1,'Дерево и шапка')
html=html.replace('<div class="chips">','<div class="chips" style="position:relative"><div class="pin" style="right:8px;top:-12px">2</div>',1)
html=html.replace('<div class="stats">','<div class="stats" style="position:relative;margin-left:34px"><div class="pin" style="left:-30px;top:-1px">3</div>',1)
html=html.replace('<span class="nm">Паз под винт (вырез)</span>','<span class="nm">Паз под винт</span><div class="pin" style="left:20px;top:4px;width:18px;height:18px;font-size:10.5px">1</div>')
html=html.replace('Ступенька (вырез)','Ступенька').replace('Бобышка (выд.)','Бобышка')
W('C1-tree.html',html)
