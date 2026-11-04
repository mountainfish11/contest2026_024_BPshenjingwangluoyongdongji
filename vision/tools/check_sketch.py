"""检查 Arduino sketch 的括号配平与关键符号（无法真正编译时的最低保障）。"""
import io
import sys


def balance(text):
    dc = dp = 0
    i, n = 0, len(text)
    state = None
    while i < n:
        ch = text[i]
        two = text[i:i + 2]
        if state is None:
            if two == '//':
                state = 'line'
                i += 2
                continue
            if two == '/*':
                state = 'block'
                i += 2
                continue
            if ch == '"':
                state = 'str'
                i += 1
                continue
            if ch == "'":
                state = 'chr'
                i += 1
                continue
            if ch == '{':
                dc += 1
            elif ch == '}':
                dc -= 1
            elif ch == '(':
                dp += 1
            elif ch == ')':
                dp -= 1
        elif state == 'line':
            if ch == '\n':
                state = None
        elif state == 'block':
            if two == '*/':
                state = None
                i += 2
                continue
        elif state == 'str':
            if ch == '\\':
                i += 2
                continue
            if ch == '"':
                state = None
        elif state == 'chr':
            if ch == '\\':
                i += 2
                continue
            if ch == "'":
                state = None
        i += 1
    return dc, dp


def main(paths):
    ok = True
    for p in paths:
        s = io.open(p, encoding='utf-8', newline='').read()
        dc, dp = balance(s)
        good = (dc == 0 and dp == 0)
        ok = ok and good
        print(f"{'OK ' if good else 'BAD'}  {p}")
        print(f"      花括号 {dc}  圆括号 {dp}  行数 {s.count(chr(10))}")
        if 'void setup()' not in s or 'void loop()' not in s:
            print("      !! 缺少 setup() 或 loop()")
            ok = False
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
