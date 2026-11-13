"""Tests for tools/satori2yaya.py using a small synthetic Satori dictionary.

  python satori2yaya_test.py <path to tools/satori2yaya.py>
"""
import os
import subprocess
import sys
import tempfile

DIC = '''ヘッダーのコメント（出力されない）

＊
：（１）一行目だ。
二行目、続き。

＊
：普通のトーク

＊起動
＞誕生日\t（ユーザ誕生月）＝＝（現在月）＆＆（ユーザ誕生日）＝＝（現在日）
＞起動一般

＊起動一般
：（６）おはよう、（ユーザ名）。

＊誕生日
：おめでとう

＊OnGhostChanged
＞（２）おっと！交代か！

＊OnOtherGhostClosed
：（Ｒ０）がどっかいったな

＃＊OnMouseMove
：コメントアウトされた見出しの本文は無視される

＠username
（ユーザ名）
'''

CONF = '''＊初期化
＄喋り間隔\t４５秒
＄ユーザ名\tマスター
'''


def main():
    tool = sys.argv[1]
    work = tempfile.mkdtemp()
    with open(os.path.join(work, 'dic_test.txt'), 'wb') as f:
        f.write(DIC.encode('cp932'))
    with open(os.path.join(work, 'satori_conf.txt'), 'wb') as f:
        f.write(CONF.encode('cp932'))
    out = os.path.join(work, 'out.dic')
    report = os.path.join(work, 'report.txt')
    subprocess.check_call([sys.executable, tool, work, out, '--reserve', 'OnBoot', '--report', report])
    text = open(out, encoding='utf-8').read()
    rep = open(report, encoding='utf-8').read()

    failures = []

    def check(cond, message):
        if not cond:
            failures.append(message)

    check('Satori_RandomTalk : nonoverlap' in text, 'random talk pool with nonoverlap')
    check("'\\s[1]'" in text, 'surface tag converted')
    check("'\\n'" in text and '二行目' in text, 'continuation line becomes \\n')
    check('\\w6' in text and '\\w3' in text, 'automatic waits inserted')
    check("return SatoriCall('誕生日')" in text, 'conditional jump')
    check("return SatoriCall('起動一般')" in text, 'unconditional jump')
    check("(Satori_Now(1))" in text and '&&' in text and '==' in text, 'condition operators converted')
    check("Satori_Var('ユーザ名')" in text, 'variable reference')
    check("'マスター'" in text, 'initial value from satori_conf.txt')
    check('Satori_TalkInterval\n{\n    45' in text.replace('\r\n', '\n'), 'talk interval')
    check('reference[0]' in text, 'event reference')
    check("'\\s[2]' + 'おっと！" in text, 'jump to a non-existent section becomes talk')
    check('コメントアウトされた見出し' not in text, 'muted section dropped')
    check('ヘッダーのコメント' not in text, 'text outside sections dropped')
    check('OnOtherGhostClosed\n{\n    SatoriCall' in text.replace('\r\n', '\n'), 'event function generated')
    check('username\n{' in text.replace('\r\n', '\n'), 'SHIORI resource from word group')
    check('存在しないジャンプ先' in rep, 'report mentions converted jump')

    for f in failures:
        print('FAILED:', f)
    print('{} checks, {} failures'.format(17, len(failures)))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
