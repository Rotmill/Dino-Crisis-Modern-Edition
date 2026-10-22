"""Позиционный патч: правки без единой строки чужого кода.

Обычный unified diff носит в себе куски исходного файла — строки, которые
удаляются, и по три строки окружения вокруг каждой правки, чтобы patch нашёл
место. Для нас это значит раздавать вместе с модом чужой код: 4 908 строк
каркаса рекомпилятора и 1 312 строк рекомпилированной игры.

Ничего этого не нужно, потому что версия исходника у нас закреплена намертво:
каркас берётся с конкретного коммита, генкод получается детерминированно из
образа пользователя. Значит место правки можно задать НОМЕРОМ СТРОКИ, а вместо
удаляемого текста хранить только их количество. В файле патча остаются лишь
наши собственные строки.

Формат 2 (17.09.2026, игрок: «там, где мы поправили часть строки, в патче лежит
и текст автора вокруг правки — удали тексты автора, а сам патч сохрани»):
строка, в которой есть текст исходника, хранится не целиком, а СБОРКОЙ — наши
байты плюс ссылки на место в исходнике (номер строки, смещение, длина). Текст
автора (у патча генкода — выражения рекомпилированной игры) при наложении
берётся из файла, который лежит у пользователя. Правило сборки: любой кусок
исходника длиной от GRAM байт (не из одних пробелов) в наших байтах не
хранится — на него ставится ссылка; сверка `audit` проверяет это по готовому
патчу.

Целостность исходника проверяется хешем: не совпал — патч не применяется, и
это честнее, чем «не нашёл окружение» у обычного patch.

Формат (UTF-8, служебные строки в ASCII):

    # dcme-posdiff 2
    F <путь> <sha256 исходника> <строк в исходнике>
    @ <номер строки, с 0> <сколько удалить>
    + <наша строка, base64>
    ~ <кусок> <кусок> ...     строка-сборка; куски по порядку:
                              =<строка>,<смещение>,<длина>  байты исходника
                              +<base64>                     наши байты
    @ ...

Формат 1 (только «+») по-прежнему накладывается.

Работа идёт в БАЙТАХ и с сохранением переводов строк: файлы каркаса лежат в
CRLF, и любая нормализация сломала бы побайтовое совпадение.

Использование:
    python posdiff.py make <файл-патча> <корень-оригинала> <корень-правленого> <путь> [<путь> ...]
    python posdiff.py apply <файл-патча> <корень>
    python posdiff.py check <файл-патча> <корень>
    python posdiff.py audit <файл-патча> <корень-оригинала>
"""

import base64
import hashlib
import os
import sys

MAGIC1 = "# dcme-posdiff 1"
MAGIC = "# dcme-posdiff 2"

GRAM = 12        # кусок исходника от стольких байт в наших байтах не хранится
MIN_COPY = 3     # выравнивание со строкой, которую наша заменяет: остаток её текста от 3 байт — ссылкой
MIN_COPY_ELSEWHERE = 8   # со строкой из другого места файла — только куски от 8 байт (без дробления на «, g»)
SHORTLIST = 6    # сколько строк-кандидатов сравнивать с нашей строкой

for _s in (sys.stdout, sys.stderr):
    try: _s.reconfigure(encoding="utf-8", errors="replace")
    except Exception: pass


def split_lines(data):
    """Режет на строки, оставляя перевод строки внутри строки."""
    out, start = [], 0
    for i, b in enumerate(data):
        if b == 0x0A:
            out.append(data[start:i + 1])
            start = i + 1
    if start < len(data):
        out.append(data[start:])
    return out


def sha(data):
    return hashlib.sha256(data).hexdigest()


class Source:
    """Строки исходника (или их диапазон) и указатели по ним: где впервые
    встречается каждый кусок в GRAM байт и какие строки похожи на данную."""

    def __init__(self, lines, lo=0, hi=None):
        self.lines = lines
        hi = len(lines) if hi is None else hi
        self.grams = {}                  # кусок GRAM байт -> (строка, смещение)
        self.votes = {}                  # кусок 8 байт -> строки, где он есть
        self.whole = {}                  # строка целиком -> номер
        for li in range(lo, hi):
            line = lines[li]
            if line.strip() and line not in self.whole:
                self.whole[line] = li
            for c in range(len(line) - GRAM + 1):
                g = line[c:c + GRAM]
                if g not in self.grams and g.strip():
                    self.grams[g] = (li, c)
            seen = set()
            for c in range(0, len(line) - 8 + 1, 2):
                g = line[c:c + 8]
                if g in seen or not g.strip():
                    continue
                seen.add(g)
                lst = self.votes.get(g)
                if lst is None:
                    self.votes[g] = [li]
                elif lst is not False:
                    if len(lst) >= 48:
                        self.votes[g] = False        # слишком частый кусок — не голосует
                    else:
                        lst.append(li)

    def shortlist(self, new):
        score = {}
        for c in range(0, len(new) - 8 + 1):
            lst = self.votes.get(new[c:c + 8])
            if lst:
                for li in lst:
                    score[li] = score.get(li, 0) + 1
        best = sorted(score.items(), key=lambda t: -t[1])[:SHORTLIST]
        return [li for li, s in best if s >= 3]


def _align(new, old, li, beat=0, min_copy=MIN_COPY):
    """Куски строки new по выравниванию со строкой исходника old (номер li).
    beat — лучший результат среди прежних кандидатов: заведомо не лучший
    кандидат отсекается дешёвой верхней оценкой."""
    import difflib
    sm = difflib.SequenceMatcher(None, old, new, autojunk=False)
    if sm.real_quick_ratio() * (len(old) + len(new)) / 2 <= beat or sm.quick_ratio() * (len(old) + len(new)) / 2 <= beat:
        return None, 0
    pieces, pos, copied = [], 0, 0
    for i, j, size in sm.get_matching_blocks():
        if size == 0:
            continue
        if j > pos:
            pieces.append(['L', new[pos:j]])
        if size >= min_copy and new[j:j + size].strip():
            pieces.append(['C', li, i, size])
            copied += size
        else:
            pieces.append(['L', new[j:j + size]])
        pos = j + size
    if pos < len(new):
        pieces.append(['L', new[pos:]])
    return pieces, copied


def _lz(seg, src):
    """Наши байты seg: каждый кусок исходника от GRAM байт — ссылкой."""
    out, k, lit = [], 0, bytearray()
    while k < len(seg):
        hit = src.grams.get(seg[k:k + GRAM]) if k + GRAM <= len(seg) else None
        if hit is None:
            lit.append(seg[k]); k += 1
            continue
        li, c = hit
        line = src.lines[li]
        n = GRAM
        while k + n < len(seg) and c + n < len(line) and seg[k + n] == line[c + n]:
            n += 1
        if lit:
            out.append(['L', bytes(lit)]); lit = bytearray()
        out.append(['C', li, c, n])
        k += n
    if lit:
        out.append(['L', bytes(lit)])
    return out


def _merge(pieces):
    """Соседние наши байты — в один кусок, соседние ссылки подряд по строке — в одну."""
    merged = []
    for p in pieces:
        if merged and p[0] == 'L' and merged[-1][0] == 'L':
            merged[-1][1] += p[1]
        elif (merged and p[0] == 'C' and merged[-1][0] == 'C' and merged[-1][1] == p[1]
              and merged[-1][2] + merged[-1][3] == p[2]):
            merged[-1][3] += p[3]
        else:
            merged.append(list(p))
    return merged


def encode_line(new, src, block_old=()):
    """Наша строка -> '+ base64' (если в ней нет текста исходника) или '~ куски'."""
    li = src.whole.get(new)
    if li is not None:
        pieces = [['C', li, 0, len(new)]]
    else:
        best, best_copied = [['L', new]], 0
        cands = [(c, MIN_COPY) for c in block_old]
        for c in src.shortlist(new):
            if c not in block_old:
                cands.append((c, MIN_COPY_ELSEWHERE))
        for c, mc in cands:
            p, copied = _align(new, src.lines[c], c, best_copied, mc)
            if p is not None and copied > best_copied:
                best, best_copied = p, copied
        # наши байты сначала склеиваются (короткие совпадения выравнивания лежат
        # между ними и вместе с соседями могут дать длинный кусок исходника), и
        # только потом по ним ищутся ссылки
        pieces = []
        for p in _merge(best):
            pieces.extend(_lz(p[1], src) if p[0] == 'L' else [p])
    merged = _merge(pieces)
    if len(merged) == 1 and merged[0][0] == 'L':
        return "+ " + base64.b64encode(new).decode('ascii')
    out = []
    for p in merged:
        if p[0] == 'L':
            out.append("+" + base64.b64encode(p[1]).decode('ascii'))
        else:
            out.append("=%d,%d,%d" % (p[1], p[2], p[3]))
    return "~ " + " ".join(out)


def encode_opcodes(a, b, opcodes, src, a0=0, b0=0):
    """Строки патча для правок одного файла (или его куска). a, b — строки
    исходника и правленого; opcodes — от difflib по этим спискам (номера
    относительно a0/b0, куска); src — Source по исходнику."""
    out, ours, theirs, composed = [], 0, 0, 0
    for tag, i1, i2, j1, j2 in opcodes:
        if tag == 'equal':
            continue
        out.append("@ %d %d" % (a0 + i1, i2 - i1))
        theirs += i2 - i1
        k, m = i2 - i1, j2 - j1
        for j, line in enumerate(b[b0 + j1:b0 + j2]):
            # строки исходника, которые эта строка заменяет, — по месту в блоке, окно в 7 строк
            block_old = []
            if k:
                at = (j * k) // m
                block_old = list(range(a0 + i1 + max(0, at - 3), a0 + i1 + min(k, at + 4)))
            ln = encode_line(line, src, block_old)
            if ln[0] == '~':
                composed += 1
            out.append(ln)
            ours += 1
    return out, ours, theirs, composed


def make(patch_path, old_root, new_root, rels):
    import difflib
    out = [MAGIC]
    ours = theirs = composed = 0
    for rel in rels:
        old = open(os.path.join(old_root, rel), 'rb').read()
        new = open(os.path.join(new_root, rel), 'rb').read()
        a, b = split_lines(old), split_lines(new)
        out.append("F %s %s %d" % (rel.replace('\\', '/'), sha(old), len(a)))
        sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
        ops = sm.get_opcodes()
        if any(t != 'equal' for t, *_ in ops):
            src = Source(a)
            lines, o, t, c = encode_opcodes(a, b, ops, src)
            out.extend(lines); ours += o; theirs += t; composed += c
    data = ("\n".join(out) + "\n").encode('utf-8')
    open(patch_path, 'wb').write(data)
    print("posdiff: %d files, %d our lines inserted (%d of them assembled from our bytes and references), "
          "%d original lines dropped by position" % (len(rels), ours, composed, theirs))
    print("posdiff: %s (%d bytes) - no text of the original: runs of %d+ bytes are references" % (patch_path, len(data), GRAM))


def parse(patch_path):
    return parse_bytes(open(patch_path, 'rb').read(), patch_path)


def parse_bytes(raw, name="patch"):
    lines = raw.decode('utf-8').split("\n")
    if not lines or lines[0].strip() not in (MAGIC, MAGIC1):
        sys.exit("posdiff: not a dcme positional patch: %s" % name)
    files, cur = [], None
    for ln in lines[1:]:
        if not ln:
            continue
        k = ln[0]
        if k == 'F':
            _, rel, digest, count = ln.split(' ', 3)
            cur = {'rel': rel, 'sha': digest, 'lines': int(count), 'ops': []}
            files.append(cur)
        elif k == '@':
            _, at, drop = ln.split(' ', 2)
            cur['ops'].append({'at': int(at), 'drop': int(drop), 'add': []})
        elif k == '+':
            cur['ops'][-1]['add'].append(('L', base64.b64decode(ln[2:])))
        elif k == '~':
            pieces = []
            for tok in ln[2:].split(' '):
                if tok[:1] == '+':
                    pieces.append(('L', base64.b64decode(tok[1:])))
                elif tok[:1] == '=':
                    li, st, n = (int(x) for x in tok[1:].split(','))
                    pieces.append(('C', li, st, n))
                else:
                    sys.exit("posdiff: bad piece: %r" % tok[:40])
            cur['ops'][-1]['add'].append(('S', pieces))
        else:
            sys.exit("posdiff: bad line: %r" % ln[:40])
    return files


def build_line(item, a, rel):
    if item[0] == 'L':
        return item[1]
    out = []
    for p in item[1]:
        if p[0] == 'L':
            out.append(p[1])
        else:
            _, li, st, n = p
            if li < 0 or li >= len(a) or st < 0 or st + n > len(a[li]):
                sys.exit("posdiff: reference out of range in %s: line %d, %d+%d" % (rel, li, st, n))
            out.append(a[li][st:st + n])
    return b"".join(out)


def apply_file(f, data):
    """Одна запись F над байтами исходника -> правленые байты."""
    a = split_lines(data)
    if len(a) != f['lines']:
        sys.exit("posdiff: %s has %d lines, expected %d" % (f['rel'], len(a), f['lines']))
    # операции идут по возрастанию, собираем заново одним проходом
    out, pos = [], 0
    for op in f['ops']:
        if op['at'] < pos:
            sys.exit("posdiff: overlapping edits in %s" % f['rel'])
        out.extend(a[pos:op['at']])
        out.extend(build_line(item, a, f['rel']) for item in op['add'])
        pos = op['at'] + op['drop']
    out.extend(a[pos:])
    return b"".join(out)


def apply(patch_path, root, check_only=False):
    files = parse(patch_path)
    changed = 0
    for f in files:
        path = os.path.join(root, f['rel'].replace('/', os.sep))
        if not os.path.isfile(path):
            sys.exit("posdiff: missing %s" % f['rel'])
        data = open(path, 'rb').read()
        got = sha(data)
        if got != f['sha']:
            sys.exit("posdiff: %s does not match the expected original\n"
                     "  expected sha256 %s\n  got            %s\n"
                     "  (wrong upstream revision, or the file was already patched)"
                     % (f['rel'], f['sha'], got))
        if check_only:
            continue
        open(path, 'wb').write(apply_file(f, data))
        changed += 1
    if check_only:
        print("posdiff: %d files match the expected original" % len(files))
    else:
        print("posdiff: patched %d files" % changed)


def audit_file(f, data, chunk=None):
    """Наши байты записи F против исходника: (всего наших байт, кусков исходника
    от GRAM байт внутри них). chunk(at) -> (lo, hi) ограничивает поиск диапазоном
    строк (патч генкода: функция)."""
    a = split_lines(data)
    whole_src = None if chunk else Source(a)
    total = bad = 0
    for op in f['ops']:
        src = whole_src
        if chunk:
            lo, hi = chunk(op['at'])
            src = Source(a, lo, hi)
        for item in op['add']:
            lits = [item[1]] if item[0] == 'L' else [p[1] for p in item[1] if p[0] == 'L']
            for lit in lits:
                total += len(lit)
                for c in range(len(lit) - GRAM + 1):
                    g = lit[c:c + GRAM]
                    if g.strip() and g in src.grams:
                        bad += 1
    return total, bad


def audit(patch_path, old_root):
    total = bad = 0
    for f in parse(patch_path):
        data = open(os.path.join(old_root, f['rel'].replace('/', os.sep)), 'rb').read()
        t, b = audit_file(f, data)
        total += t; bad += b
        if b:
            print("posdiff: %s - %d runs of the original inside our bytes" % (f['rel'], b))
    print("posdiff audit: %d bytes of ours, %d runs of %d+ bytes of the original among them" % (total, bad, GRAM))
    return bad


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__.strip().splitlines()[-4].strip())
    mode = sys.argv[1]
    if mode == 'make':
        make(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:])
    elif mode == 'apply':
        apply(sys.argv[2], sys.argv[3])
    elif mode == 'check':
        apply(sys.argv[2], sys.argv[3], check_only=True)
    elif mode == 'audit':
        sys.exit(1 if audit(sys.argv[2], sys.argv[3]) else 0)
    else:
        sys.exit("posdiff: unknown mode %r (make / apply / check / audit)" % mode)


if __name__ == '__main__':
    main()
