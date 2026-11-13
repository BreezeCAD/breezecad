"""Макеты окна BreezeCAD (светлая тема, токены из src/ui/Theme.cpp)."""
import sys

CSS = """
@font-face{font-family:Plex;src:url(IBMPlexSans-Regular.ttf);font-weight:400}
@font-face{font-family:Plex;src:url(IBMPlexSans-Medium.ttf);font-weight:500}
@font-face{font-family:Plex;src:url(IBMPlexSans-SemiBold.ttf);font-weight:600}
@font-face{font-family:PlexMono;src:url(IBMPlexMono-Regular.ttf)}
*{box-sizing:border-box;margin:0;padding:0}
body{width:1440px;height:900px;font-family:Plex;font-size:13px;color:#1B1F24;background:#F4F5F7;overflow:hidden;display:grid;
 grid-template-rows:40px 34px 104px 1fr 34px 26px;grid-template-columns:48px 296px 1fr}
.hdr{grid-column:1/4;display:flex;align-items:center;gap:6px;padding:0 10px;background:#F4F5F7}
.hdr .ib{width:28px;height:28px;display:grid;place-items:center;border-radius:6px}
.hdr .ib img{width:17px;opacity:.85}
.hdr .sep{width:1px;height:18px;background:#E1E4E8;margin:0 4px}
.hdr .app{width:200px;height:26px;border:1px solid #DADDE2;border-radius:6px;background:#fff;display:flex;align-items:center;gap:6px;padding:0 8px;color:#4A525E;font-size:12px;margin-left:10px}
.hdr .title{flex:1;text-align:center;font-weight:600;font-size:12.5px}.hdr .title span{color:#5A626E;font-weight:400}
.search{width:220px;height:28px;border:1px solid #DADDE2;border-radius:7px;background:#fff;display:flex;align-items:center;gap:7px;padding:0 8px;color:#A0A7B1;font-size:12px}
.search img{width:14px;opacity:.6}.kbd{margin-left:auto;font-family:PlexMono;font-size:10.5px;color:#5A626E;border:1px solid #DADDE2;border-radius:4px;padding:0 4px}
.stats{font-family:PlexMono;font-size:11px;color:#4A525E;display:flex;gap:12px;margin:0 6px 0 10px}.stats b{color:#2E7D4F;font-weight:400}
.modes{grid-column:1/4;display:flex;gap:4px;padding:0 12px;align-items:flex-end;background:#F4F5F7}
.modes div{padding:7px 14px 8px;font-size:12.5px;color:#4A525E;border-radius:7px 7px 0 0}
.modes .on{background:#FBFBFC;color:#1B1F24;font-weight:600;box-shadow:inset 0 -2px #1D4ED8}
.ribbon{grid-column:1/4;background:#FBFBFC;border-bottom:1px solid #E1E4E8;display:flex;padding:6px 8px 0}
.grp{display:flex;flex-direction:column;padding:0 8px;border-right:1px solid #EEF0F2}
.grp .btns{display:flex;gap:2px;flex:1}
.grp .cap{font-size:10.5px;color:#5A626E;text-align:center;padding:3px 0 5px}
.big{width:62px;display:flex;flex-direction:column;align-items:center;gap:4px;padding-top:6px;font-size:11.5px;text-align:center;line-height:1.2;border-radius:6px}
.big img{width:32px;height:32px}
.small{display:flex;flex-direction:column;gap:3px;padding-top:4px}.small div{display:flex;gap:6px;align-items:center;font-size:11.5px;height:20px;padding:0 4px}.small img{width:18px}
.rail{background:#F4F5F7;display:flex;flex-direction:column;align-items:center;gap:6px;padding-top:10px;border-right:1px solid #E1E4E8}
.rail div{width:34px;height:34px;border-radius:8px;display:grid;place-items:center}.rail img{width:18px;opacity:.75}
.rail .on{background:#E6EEFB;border:1px solid #BFD3F5}
.panel{background:#fff;border-right:1px solid #E1E4E8;overflow:hidden;display:flex;flex-direction:column}
.ph{height:40px;display:flex;align-items:center;justify-content:space-between;padding:0 12px;font-weight:600;font-size:12.5px;border-bottom:1px solid #EEF0F2}
.ph img{width:15px;opacity:.6}
.row{height:30px;display:flex;align-items:center;gap:7px;padding:0 12px 0 8px;font-size:12.5px;white-space:nowrap;position:relative}
.row .eye{width:15px;opacity:.4}.row .chev{width:10px;opacity:.5}.row .ic{width:16px;height:16px;position:relative;flex:none}
.row .ic img.m{width:16px}.row .ic img.b{position:absolute;right:-4px;bottom:-3px;width:10px}
.row .nm{flex:1;overflow:hidden;text-overflow:ellipsis}.row .meta{font-family:PlexMono;font-size:11px;color:#5A626E}
.row.sel{background:#E8F0FC}.row.hov{background:#F1F4F8}.row.mut{color:#A0A7B1}
.row .st{width:14px}
.vp{position:relative;overflow:hidden;background:linear-gradient(#FAFBFC,#E4E8ED)}
.vp>img.model{position:absolute;inset:0;width:100%;height:100%;object-fit:cover}
.cube{position:absolute;right:24px;top:22px;width:92px;opacity:.95}
.tabs{grid-column:1/4;background:#F4F5F7;border-top:1px solid #E1E4E8;display:flex;align-items:stretch;padding-left:48px}
.tabs div{padding:0 16px;display:flex;align-items:center;font-size:12px}.tabs .on{background:#fff;box-shadow:inset 0 2px #1D4ED8;font-weight:500}
.status{grid-column:1/4;background:#ECEEF1;display:flex;align-items:center;gap:16px;padding:0 12px;font-size:11.5px;color:#4A525E}
.status .r{margin-left:auto;display:flex;gap:14px}
.callout{position:absolute;background:#1B1F24;color:#F4F5F7;font-size:12px;line-height:1.35;padding:7px 10px;border-radius:8px;max-width:260px;box-shadow:0 6px 18px rgba(0,0,0,.18)}
.callout b{color:#9FD8F7;font-weight:600}
.pin{position:absolute;width:22px;height:22px;border-radius:50%;background:#EA7A1E;color:#fff;font-weight:600;font-size:12px;display:grid;place-items:center;box-shadow:0 0 0 3px rgba(234,122,30,.25)}
/* tool panels */
.sec{padding:10px 12px 4px;font-size:11px;font-weight:600;color:#5A626E;text-transform:uppercase;letter-spacing:.04em}
.seg{margin:4px 12px;display:flex;background:#F1F3F6;border-radius:7px;padding:3px;gap:3px}
.seg div{flex:1;height:34px;border-radius:5px;display:flex;flex-direction:column;align-items:center;justify-content:center;font-size:10.5px;color:#4A525E;gap:1px}
.seg div img{width:18px}.seg .on{background:#fff;box-shadow:0 1px 2px rgba(0,0,0,.08);color:#1B1F24;font-weight:500}
.fld{margin:6px 12px;height:36px;border:1px solid #DADDE2;border-radius:8px;display:flex;align-items:center;padding:0 10px;gap:8px}
.fld .l{color:#5A626E;font-size:12px;width:96px}.fld .v{font-family:PlexMono;font-size:12.5px;flex:1}.fld .u{color:#5A626E;font-size:11.5px}
.sl{margin:10px 12px;display:flex;align-items:center;gap:10px;font-size:12px;color:#4A525E}.sl .t{flex:1;height:4px;border-radius:2px;background:#E1E4E8;position:relative}
.sl .t i{position:absolute;left:0;top:0;bottom:0;background:#1D4ED8;border-radius:2px}.sl .t b{position:absolute;top:-6px;width:16px;height:16px;border-radius:50%;background:#fff;border:2px solid #1D4ED8}
.sw{margin:6px 12px;display:flex;gap:8px}.sw div{flex:1;height:52px;border-radius:8px;border:1px solid #DADDE2;display:flex;align-items:flex-end;padding:4px 6px;font-size:10.5px;color:#4A525E}
.sw .on{outline:2px solid #1D4ED8;outline-offset:1px}
.btnrow{margin:14px 12px 8px;display:flex;gap:8px}.btnp{flex:1;height:36px;border-radius:8px;background:#1D4ED8;color:#fff;font-weight:600;display:grid;place-items:center;font-size:12.5px}
.btns2{height:36px;padding:0 14px;border-radius:8px;border:1px solid #DADDE2;display:grid;place-items:center;font-size:12.5px}
.chk{margin:6px 12px;display:flex;align-items:center;gap:8px;font-size:12.5px}.chk i{width:16px;height:16px;border-radius:4px;background:#1D4ED8;display:grid;place-items:center}.chk i::after{content:"";width:7px;height:4px;border:2px solid #fff;border-top:0;border-right:0;transform:rotate(-45deg) translate(1px,-1px)}
.chk.off i{background:#fff;border:1.5px solid #A0A7B1}.chk.off i::after{display:none}
.hint{margin:4px 12px;font-size:11.5px;color:#5A626E;line-height:1.4}
.chips{display:flex;gap:5px;white-space:nowrap;padding:8px 12px 6px;border-bottom:1px solid #EEF0F2}.chips div{font-size:11.5px;padding:3px 9px;border-radius:12px;background:#F1F3F6;color:#4A525E}.chips .on{background:#E6EEFB;color:#1A3FB0;border:1px solid #BFD3F5}
.grprow{height:28px;display:flex;align-items:center;gap:7px;padding:0 12px 0 8px;font-size:12px;color:#5A626E;background:#FAFBFC}
.endm{height:0;border-top:2px solid #1D4ED8;margin:4px 12px;position:relative}.endm::before{content:"Конец детали";position:absolute;left:0;top:-9px;background:#fff;color:#1D4ED8;font-size:10.5px;padding:0 6px 0 0;font-weight:600}
"""

def icon(n): return f'icons/{n}.svg'

def header(stats):
    ib = ''.join(f'<div class="ib"><img src="{icon(n)}"></div>' for n in ['ui-new','ui-open','ui-save'])
    ib2 = ''.join(f'<div class="ib"><img src="{icon(n)}"></div>' for n in ['ui-undo','ui-redo'])
    ib3 = ''.join(f'<div class="ib"><img src="{icon(n)}"></div>' for n in ['ui-home','ui-cube','ui-palette','ui-fx','ui-ruler'])
    return f'''<div class="hdr"><img src="breezecad-mark.svg" style="width:22px;margin:0 6px 0 4px">{ib}<div class="sep"></div>{ib2}<div class="sep"></div>{ib3}
<div class="app"><span style="width:12px;height:12px;border-radius:3px;background:#C9CFD7;border:1px solid #9aa"></span>По умолчанию</div>
<div class="title">Плита — угол <span>— BreezeCAD</span></div>
<div class="search"><img src="{icon('ui-search')}">Поиск команд<span class="kbd">Ctrl K</span></div>
<div class="stats">{stats}</div></div>'''

def modes(active='3D-модель', extra=()):
    tabs = ['3D-модель','Эскиз','Вид', *extra]
    return '<div class="modes">' + ''.join(f'<div class="{"on" if t==active else ""}">{t}</div>' for t in tabs) + '</div>'

def big(ic, label): return f'<div class="big"><img src="{icon(ic)}"><div>{label}</div></div>'
def grp(cap, inner): return f'<div class="grp"><div class="btns">{inner}</div><div class="cap">{cap}</div></div>'
def small(items): return '<div class="small">' + ''.join(f'<div><img src="{icon(i)}">{l}</div>' for i, l in items) + '</div>'

def ribbon_model():
    return '<div class="ribbon">' + grp('Эскиз', big('sketch','Начать<br>эскиз')) + \
        grp('Создать', big('extrude','Выдав-<br>ливание')+big('revolve','Вращение')+small([('sweep',''),('loft',''),('coil','')])) + \
        grp('Изменить', big('hole','Отверстие')+big('fillet','Сопря-<br>жение')+big('thread','Резьба')+small([('chamfer',''),('shell','')])) + \
        grp('Листовой металл', big('sheet','Лист')+big('flange','Отбор-<br>товка')+big('sheet-cut','Вырез')+big('flat-pattern','Развёртка')) + \
        grp('Массив', small([('pattern-rect',''),('pattern-circular',''),('mirror','')])) + \
        grp('Раб. элементы', small([('work-plane',''),('work-axis',''),('work-point','')])) + \
        grp('Ст. изделия', big('library','Библиотека')) + \
        grp('Экспорт', small([('export','STEP'),('export','STL'),('export','3MF')])) + '</div>'

def ribbon_view():
    return '<div class="ribbon">' + \
        grp('Стиль', big('ds-shaded-edges','С рёбрами')+big('ds-shaded','Тонировка')+big('ds-transparent','Прозрачно')+big('ds-wireframe','Каркас')) + \
        grp('Сцена', big('view-background','Фон')+big('ui-grid','Пол<br>и сетка')+big('ui-sun','Освеще-<br>ние')) + \
        grp('Кадр', big('ui-frame','Сделать<br>кадр')) + \
        grp('Разрез', big('section','Разрез<br>вида')) + \
        grp('Качество', small([('ui-sliders','Живой вид'),('ui-sliders','Обычный'),('ui-sliders','Экономный')])) + '</div>'

def rail(on=0):
    ics = ['ui-tree','ui-sliders','ui-fx']
    return '<div class="rail">' + ''.join(f'<div class="{"on" if i==on else ""}"><img src="{icon(n)}"></div>' for i, n in enumerate(ics)) + '</div>'

TREE = [
    # (icon, badge, name, meta, flags)
    ('tree-part', None, 'Плита — угол', '', ''),
    ('tree-origin', None, 'Начало координат', '', 'chev'),
    ('tree-extrude', None, 'Основание', '10 мм', 'chev'),
    ('tree-plane', None, 'Плоскость верха плиты', '10 мм', 'hid'),
    ('tree-hole', None, 'Отверстие Ø10', 'насквозь', 'chev'),
    ('tree-extrude', 'cut', 'Паз под винт (вырез)', '10 мм', 'chev'),
    ('tree-extrude', 'cut', 'Ступенька (вырез)', '5 мм', 'chev sel'),
    ('tree-chamfer', None, 'Скосы кромок 45°', '0.5×45°', ''),
    ('tree-fillet', None, 'Скругление паза', 'R1', 'warn'),
    ('tree-extrude', None, 'Бобышка (выд.)', '10 мм', 'chev'),
    ('tree-plane', None, 'Плоскость опоры', '−5.82 мм', 'hid'),
    ('tree-extrude', 'isect', 'Обрезка по контуру', '—', 'chev'),
    ('tree-pattern-rect', None, 'Массив бобышек 2×3', '6 шт', 'chev'),
    ('tree-sketch', None, 'Эскиз маркировки', '', 'mut'),
]

def tree_rows(badges=False, rows=TREE, hover=None):
    out = []
    for i, (ic, b, nm, meta, fl) in enumerate(rows):
        cls = 'row' + (' sel' if 'sel' in fl else '') + (' mut' if 'mut' in fl else '') + (' hov' if hover == i else '')
        eye = f'<img class="eye" src="{icon("ui-eye-off")}">' if 'hid' in fl else '<span style="width:15px"></span>'
        chev = f'<img class="chev" src="{icon("ui-chevron-right")}">' if 'chev' in fl else '<span style="width:10px"></span>'
        bd = f'<img class="b" src="{icon("badge-"+b)}">' if (badges and b) else ''
        st = f'<img class="st" src="{icon("status-warning")}">' if 'warn' in fl else ''
        if not badges and b and '(вырез)' not in nm and b == 'cut': pass
        out.append(f'<div class="{cls}">{eye}{chev}<span class="ic"><img class="m" src="{icon(ic)}">{bd}</span><span class="nm">{nm}</span>{st}<span class="meta">{meta}</span></div>')
    return ''.join(out)

def tree_panel(badges=False, chips=False, endmarker=False, hover=None):
    head = f'<div class="ph">Дерево построения<img src="{icon("ui-search")}"></div>'
    ch = '<div class="chips"><div class="on">Всё</div><div>Тела</div><div>Эскизы</div><div>Раб.</div><div>Проблемы 1</div></div>' if chips else ''
    rows = tree_rows(badges, hover=hover)
    if endmarker:
        parts = rows.split('</div><div class="row')
        rows = '</div><div class="row'.join(parts[:12]) + '</div><div class="endm"></div><div class="row' + '</div><div class="row'.join(parts[12:])
    return f'<div class="panel">{head}{ch}{rows}</div>'

def page(body_parts, title='макет'):
    return f'<!DOCTYPE html><html lang="ru"><head><meta charset="utf-8"><title>{title}</title><style>{CSS}</style></head><body>{"".join(body_parts)}</body></html>'

def viewport(img, extra=''):
    return f'<div class="vp"><img class="model" src="{img}"><img class="cube" src="cube.svg">{extra}</div>'

def tabs(): return '<div class="tabs"><div class="on">Плита — угол</div><div style="color:#5A626E">+</div></div>'
def status(hint, right): return f'<div class="status"><b style="font-weight:600;color:#1B1F24">Деталь</b><span>{hint}</span><div class="r">{right}</div></div>'

STATS_MS = '<span>1.8 мс</span><span>202 МБ</span><span>56 OBJ</span>'
STATS_FPS = '<span><b>●</b> 144 к/с</span><span>202 МБ</span><span>56 объектов</span>'
HINT = 'ПКМ — вращение, зажатое колесо — сдвиг, прокрутка — масштаб, двойной щелчок в дереве — редактировать'
