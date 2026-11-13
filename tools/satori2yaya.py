#!/usr/bin/env python3
"""satori2yaya.py - convert a Satori (里々) dictionary set to a YAYA dictionary.

Supports the common Satori subset used by typical solo ghosts:

  ＊name / ＊        sections (same name = random choice, empty name = random talk)
  ＠name             word groups (used here for SHIORI resources such as username)
  ：                 speaker line (alternates \\0 / \\1, inserts the scope-switch text)
  (continuation)     same speaker, new line
  （数字）           surface change  -> \\s[n]
  （Ｒn）            event reference -> reference[n]
  （現在月）（現在日）（現在曜日）（ユーザ名） and other unknown （変数） -> globals
  ＞target<TAB>cond  conditional / unconditional jump to another section
  ＃                 comment (a commented-out ＃＊header also mutes the lines below it)
  ＄name<TAB>value   initial variable values (satori_conf.txt ＊初期化)

The generated dictionary needs a SHIORI front-end that sets the global array
`reference` and calls functions by event ID (e.g. shiori3_mini.dic).
Event sections (＊OnXxx) become YAYA functions with the event name, except
those listed with --reserve, which the caller wires up itself through
SatoriCall("OnXxx").

Usage:
  python satori2yaya.py <satori ghost/master dir> <output.dic> [--reserve OnBoot,OnClose,...]
                        [--skip homeurl,sakura.portalsites] [--report report.txt]

Input files are read as CP932 (Shift_JIS); output is UTF-8.
"""
import argparse
import glob
import os
import re
import sys

FW_DIGITS = str.maketrans('０１２３４５６７８９', '0123456789')
WEEKDAYS = '日月火水木金土'
ALIASES = {'OnBoot': '起動'}  # Satori calls ＊起動 for OnBoot


def fw_to_ascii(text):
    return text.translate(FW_DIGITS)


def yaya_name(name):
    """Function name for a section."""
    safe = re.sub(r'[\s\t（）()「」、。，,：:；;"\'\\/]', '_', name)
    return 'Satori_' + safe


def quote(text):
    """YAYA single-quoted literal (no %-expansion); single quotes via CHR(39)."""
    if text == '':
        return "''"
    parts = text.split("'")
    return " + CHR(39) + ".join("'" + p + "'" for p in parts) if len(parts) > 1 else "'" + text + "'"


class Converter:
    def __init__(self, reserve, skip, wait_scale=1.0):
        self.sections = {}     # name -> list of bodies (list of lines)
        self.words = {}        # ＠name -> list of values
        self.order = []
        self.variables = {}    # ＄ initial values
        self.reserve = set(reserve)
        self.skip = set(skip)
        self.report = []
        self.wait_scale = wait_scale
        self.scope_switch = '\\n[half]'

    # ---------------------------------------------------------------- parsing
    def parse_file(self, path):
        text = open(path, 'rb').read().decode('cp932', errors='replace')
        current = None  # ('＊', name) / ('＠', name) / None (outside or muted)
        body = None
        for raw in text.splitlines():
            line = raw.rstrip('\r')
            if line.startswith('＃'):
                if line.startswith('＃＊') or line.startswith('＃＠'):
                    self._close(current, body)
                    current, body = None, None  # commented-out header mutes its body
                continue
            if line.startswith('＊') or line.startswith('＠'):
                self._close(current, body)
                name = line[1:].strip()
                current = (line[0], name)
                body = []
                continue
            if current is None:
                continue
            body.append(line)
        self._close(current, body)

    def _close(self, current, body):
        if current is None:
            return
        kind, name = current
        while body and body[-1].strip() == '':
            body.pop()
        if kind == '＊':
            if name == '初期化':
                for line in body:
                    if line.startswith('＄') and '\t' in line:
                        key, value = line[1:].split('\t', 1)
                        self.variables[key.strip()] = value.strip()
                return
            if name not in self.sections:
                self.sections[name] = []
                self.order.append(name)
            self.sections[name].append(body)
        else:
            self.words.setdefault(name, []).extend(l for l in body if l.strip())

    # ------------------------------------------------------------- expressions
    def inline(self, text):
        """Converts one line of Satori text to a list of YAYA expression parts."""
        parts = []
        pos = 0
        for m in re.finditer(r'（([^（）]*)）', text):
            if m.start() > pos:
                parts.append(quote(self.auto_wait(text[pos:m.start()])))
            inner = m.group(1)
            ascii_inner = fw_to_ascii(inner)
            if re.fullmatch(r'-?\d+', ascii_inner):
                parts.append(quote('\\s[' + ascii_inner + ']'))
            elif re.fullmatch(r'[ＲR](\d+)', ascii_inner):
                parts.append('reference[' + re.fullmatch(r'[ＲR](\d+)', ascii_inner).group(1) + ']')
            elif inner == '現在月':
                parts.append('Satori_Now(1)')
            elif inner == '現在日':
                parts.append('Satori_Now(2)')
            elif inner == '現在時':
                parts.append('Satori_Now(4)')
            elif inner == '現在分':
                parts.append('Satori_Now(5)')
            elif inner == '現在年':
                parts.append('Satori_Now(0)')
            elif inner == '現在曜日':
                parts.append('Satori_Weekday()')
            elif inner in self.words:
                parts.append('Satori_Word(' + quote(inner) + ')')
            else:
                parts.append('Satori_Var(' + quote(inner) + ')')
            pos = m.end()
        if pos < len(text):
            parts.append(quote(self.auto_wait(text[pos:])))
        return parts or ["''"]

    def auto_wait(self, text):
        if self.wait_scale <= 0:
            return text
        def w(n):
            return '\\w' + str(max(1, int(round(n * self.wait_scale))))
        # Satori-like automatic waits after punctuation (once per run of marks).
        text = re.sub(r'(、)(?![、。！？!?])', lambda m: m.group(1) + w(3), text)
        text = re.sub(r'([。！？!?]+)', lambda m: m.group(1) + w(6), text)
        return text

    def condition(self, cond):
        cond = cond.strip()
        out = []
        pos = 0
        for m in re.finditer(r'（([^（）]*)）', cond):
            out.append(fw_to_ascii(cond[pos:m.start()]))
            inner = m.group(1)
            expr = ' + '.join(self.inline('（' + inner + '）'))
            # Numbers from GETTIME compare numerically; keep variables as strings.
            out.append('(' + expr + ')')
            pos = m.end()
        out.append(fw_to_ascii(cond[pos:]))
        expr = ''.join(out)
        for a, b in (('＝＝', '=='), ('！＝', '!='), ('＞＝', '>='), ('＜＝', '<='), ('＆＆', '&&'), ('｜｜', '||'),
                     ('＞', '>'), ('＜', '<'), ('＝', '==')):
            expr = expr.replace(a, b)
        return expr

    # ------------------------------------------------------------------ bodies
    def body_ops(self, body):
        ops = []
        for line in body:
            if line.startswith('＞'):
                target, _, cond = line[1:].partition('\t')
                static = '（' not in target
                # "＞（２）おっと！" style lines: a surface tag or sentence punctuation means talk, not a jump.
                looks_like_talk = bool(re.search(r'（[0-9０-９]+）', target)) or bool(re.search(r'[、。！？…]', target))
                if (static or looks_like_talk) and ALIASES.get(target, target) not in self.sections:
                    # Jump to a section that does not exist: the author meant a line of talk.
                    ops.append(('text', target, True))
                    self.report.append('＞{} -> 存在しないジャンプ先のためセリフとして変換'.format(target))
                    continue
                ops.append(('jump', target, cond))
            elif line.startswith('：'):
                ops.append(('text', line[1:], True))
            else:
                ops.append(('text', line, False))
        return ops

    def talk_expression(self, texts):
        """texts: list of (text, speaker_switch) -> YAYA string expression (finished talk)."""
        parts = [quote('\\0\\s[0]')]  # 会話時サーフェス戻し
        scope = None
        spoken = set()
        first_line = True
        for text, switch in texts:
            if switch:
                scope = 0 if scope is None else 1 - scope
                tag = '\\' + str(scope)
                if scope in spoken:
                    tag += self.scope_switch
                parts.append(quote(tag) if scope != 0 or not first_line else "''")
                spoken.add(scope)
            elif not first_line:
                parts.append(quote('\\n'))
            if scope is None:
                scope = 0
                spoken.add(0)
            parts.extend(self.inline(text))
            first_line = False
        parts.append(quote('\\e'))
        parts = [p for p in parts if p != "''"]
        return ' + '.join(parts)

    def body_code(self, body, indent='    '):
        ops = self.body_ops(body)
        if not any(op[0] == 'jump' for op in ops):
            texts = [(op[1], op[2]) for op in ops if op[0] == 'text' and (op[1] != '' or not op[2])]
            if not texts:
                return None
            return indent + self.talk_expression(texts)
        lines = []
        pending = []
        for op in ops:
            if op[0] == 'text':
                pending.append((op[1], op[2]))
                continue
            _, target, cond = op
            target_expr = ' + '.join(self.inline(target)) if '（' in target else quote(ALIASES.get(target, target))
            call = 'return SatoriCall({})'.format(target_expr)
            if cond.strip():
                lines.append('{}if {} {{'.format(indent, self.condition(cond)))
                lines.append('{}    {}'.format(indent, call))
                lines.append('{}}}'.format(indent))
            else:
                lines.append(indent + call)
        if pending:
            lines.append(indent + 'return ' + self.talk_expression(pending))
        return '\n'.join(lines)

    # ------------------------------------------------------------------ output
    def generate(self, source):
        out = []
        out.append('// ---------------------------------------------------------------------------')
        out.append('// Generated by tools/satori2yaya.py from: ' + source)
        out.append('// Do not edit by hand: change the Satori dictionaries and convert again.')
        out.append('// ---------------------------------------------------------------------------')
        out.append('')
        out.append('// ---- runtime helpers ----')
        out.append('Satori_Now\n{\n    _t = GETTIME\n    _t[_argv[0]]\n}\n')
        out.append("Satori_Weekday\n{\n    _t = GETTIME\n    SUBSTR('" + WEEKDAYS + "', _t[3], 1)\n}\n")
        out.append('Satori_Var\n{\n    _name = \'satori.var.\' + _argv[0]\n    if ISVAR(_name) {\n        EVAL(_name)\n    }\n    else {\n        Satori_Default(_argv[0])\n    }\n}\n')
        defaults = []
        for key, value in self.variables.items():
            defaults.append('    if _argv[0] == {} {{\n        return {}\n    }}'.format(quote(key), quote(value)))
        out.append('Satori_Default\n{\n' + '\n'.join(defaults) + ('\n' if defaults else '') + "    ''\n}\n")
        out.append("Satori_Word\n{\n    _f = 'Satori_Word_' + _argv[0]\n    if ISFUNC(_f) == 1 {\n        EVAL(_f)\n    }\n}\n")
        out.append("// Calls a section by its Satori name (empty when it does not exist).\nSatoriCall\n{\n    _f = 'Satori_' + _argv[0]\n    if ISFUNC(_f) == 1 {\n        EVAL(_f)\n    }\n}\n")
        out.append('// Random talk interval (seconds) from ＄喋り間隔.')
        interval = fw_to_ascii(self.variables.get('喋り間隔', '30')).rstrip('秒')
        out.append('Satori_TalkInterval\n{\n    ' + (interval if interval.isdigit() else '30') + '\n}\n')

        for name, values in self.words.items():
            if name in self.skip:
                self.report.append('＠{} -> --skip により除外'.format(name))
                continue
            func = 'Satori_Word_' + re.sub(r'[^\w.]', '_', name)
            out.append(func)
            out.append('{')
            for v in values:
                out.append('    ' + ' + '.join(self.inline(v)))
            out.append('}\n')
            if re.fullmatch(r'[A-Za-z0-9_.]+', name):
                out.append('{}\n{{\n    {}\n}}\n'.format(name, func))  # SHIORI resource (e.g. username)

        out.append('// ---- sections ----')
        events = []
        for name in self.order:
            if name in self.skip:
                self.report.append('＊{} -> --skip により除外'.format(name))
                continue
            bodies = self.sections[name]
            label = '（ランダムトーク）' if name == '' else name
            func = 'Satori_RandomTalk' if name == '' else yaya_name(name)
            codes = [self.body_code(b) for b in bodies]
            codes = [c for c in codes if c]
            if not codes:
                self.report.append('＊{} -> 本文が空'.format(label))
                continue
            simple = all('\n' not in c and 'return' not in c for c in codes)
            out.append('// ＊' + label + ('（{} 件）'.format(len(codes)) if len(codes) > 1 else ''))
            if simple:
                mode = ' : nonoverlap' if len(codes) > 1 else ''
                out.append(func + mode)
                out.append('{')
                out.extend(codes)
                out.append('}\n')
            else:
                for i, code in enumerate(codes):
                    out.append('{}_{}\n{{\n{}\n}}\n'.format(func, i + 1, code))
                out.append(func + '\n{')
                for i in range(len(codes)):
                    out.append('    {}_{}()'.format(func, i + 1))
                out.append('}\n')
            if name.startswith('On') and re.fullmatch(r'[A-Za-z0-9_.]+', name):
                events.append(name)

        out.append('// ---- SHIORI events ----')
        for name in events:
            if name in self.reserve:
                self.report.append('＊{} -> 予約（呼び出し側で SatoriCall("{}")）'.format(name, name))
                continue
            out.append('{}\n{{\n    SatoriCall({})\n}}\n'.format(name, quote(name)))
        return '\n'.join(out) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('source', help='Satori ghost/master directory')
    parser.add_argument('output', help='output YAYA dictionary (UTF-8)')
    parser.add_argument('--reserve', default='', help='comma-separated events wired up by the caller')
    parser.add_argument('--skip', default='', help='comma-separated sections / word groups to drop')
    parser.add_argument('--wait-scale', type=float, default=1.0, help='automatic wait multiplier (0 = none)')
    parser.add_argument('--report', help='write a conversion report')
    args = parser.parse_args()

    conv = Converter([r for r in args.reserve.split(',') if r], [s for s in args.skip.split(',') if s],
                     args.wait_scale)
    files = sorted(glob.glob(os.path.join(args.source, 'dic*.txt')))
    conf = os.path.join(args.source, 'satori_conf.txt')
    for path in ([conf] if os.path.exists(conf) else []) + files:
        conv.parse_file(path)
    text = conv.generate(os.path.basename(os.path.normpath(os.path.join(args.source, '..', '..'))))
    with open(args.output, 'w', encoding='utf-8', newline='\r\n') as f:
        f.write(text)

    talks = len(conv.sections.get('', []))
    summary = 'sections={} randomTalks={} words={} variables={}'.format(
        len(conv.sections), talks, len(conv.words), len(conv.variables))
    if args.report:
        with open(args.report, 'w', encoding='utf-8') as f:
            f.write(summary + '\n' + '\n'.join(conv.report) + '\n')
    sys.stdout.write(summary + '\n')


if __name__ == '__main__':
    main()
