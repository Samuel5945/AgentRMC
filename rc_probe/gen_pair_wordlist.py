"""生成配对码中文助记词表（2048 个常用双字词）。
数据源：jieba dict.txt（词频）。取「恰好两个汉字」的词按词频降序前 2048，
真实常用、好认好输；2048=2^11，12 词≈132-bit 熵（≥128-bit 配对强度）。
用法：python rc_probe/gen_pair_wordlist.py <jieba_dict.txt>
产出：native/resources/pair_words_zh.txt（每行一词，UTF-8，入库走 qrc）。
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "native" / "resources" / "pair_words_zh.txt"
dict_path = pathlib.Path(sys.argv[1]) if len(sys.argv) > 1 else pathlib.Path(r"D:\apps\pair-dl\jieba_dict.txt")

cjk2 = re.compile(r"^[\u4e00-\u9fff]{2}$")
# 纯虚词（代词/连词/副词）念起来像断句、不好记，剔掉让助记词偏实词。
STOP = set("""一个 我们 他们 她们 它们 自己 这个 那个 什么 怎么 怎样 这样 那样 这么 那么
已经 可以 没有 还是 就是 但是 因为 所以 如果 虽然 而且 或者 以及 关于 对于 由于 于是
不过 只是 正是 真是 总是 往往 常常 经常 曾经 正在 应该 应当 必须 需要 知道 觉得 认为
希望 所有 一切 大家 别人 人家 彼此 互相 相互 多么 非常 十分 特别 比较 稍微 几乎 简直
到底 究竟 难道 何必 何不 不是 不要 不用 别是 似的 一样 一般 一起 一直 一定 一度""".split())
rows = []
for line in dict_path.read_text(encoding="utf-8").splitlines():
    parts = line.split()
    if len(parts) < 2:
        continue
    word, freq = parts[0], parts[1]
    if not cjk2.match(word) or word in STOP:
        continue
    try:
        f = int(freq)
    except ValueError:
        continue
    rows.append((word, f))

# 词频降序；同频按词排序保证可复现。去重。
rows.sort(key=lambda x: (-x[1], x[0]))
seen = set()
words = []
for w, f in rows:
    if w in seen:
        continue
    seen.add(w)
    words.append(w)
    if len(words) == 2048:
        break

if len(words) != 2048:
    print("ERROR: only %d words" % len(words))
    raise SystemExit(1)

OUT.write_text("\n".join(words) + "\n", encoding="utf-8")
print("WROTE", OUT, "count=", len(words))
print("FIRST20:", " ".join(words[:20]))
print("LAST10:", " ".join(words[-10:]))