# 生成「带鲸鱼娘角色」的主界面/面板背景：Qwen-Image 2.1 参考图引导，
# 角色站在角落、大面深海留白给界面叠字。复用 gen_theme.py 的客户端与图构建。
# 用法：python tools/theme/character_backgrounds.py --reference tools/theme/reference/whale_maid.jpg
import argparse
import io
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from gen_theme import Comfy, generate_one  # noqa: E402

from PIL import Image, ImageEnhance  # noqa: E402

# (生成尺寸, 入库尺寸)：latent 步长要求 32 的倍数，入库尺寸差一点用缩放补。
SIZES = {
    'main': ((1728, 1056), (1728, 1080)),
    'panel': ((1600, 896), (1600, 900)),
}

PROMPTS = {
    'main': (
        '横版桌面应用背景插画，深蓝色深夜海洋幻想世界。画面右侧站着一位'
        '十几岁的鲸鱼少女：人类少女体型，及腰的深蓝色长发发梢渐变成浅蓝，'
        '头上有小小的鲸鱼耳鳍，身后一条大大的深蓝色鲸鱼尾巴，穿着缀白色'
        '蕾丝的深蓝色海洋风连衣裙，赤脚轻轻漂浮，长发扬起、裙摆在水中飘动，'
        '温柔地微笑，身边环绕发光气泡、小水母、光斑和远处悠游的鲸鱼剪影，'
        '人物完整清晰。画面左侧三分之二是干净的深海渐变、大量留白，供界面'
        '文字排版。整体深海军蓝色调、柔和低对比、梦幻治愈，画面中没有任何文字。'
    ),
    'panel': (
        '窄长竖向面板背景插画，深蓝色深夜海洋渐变。画面底部探出一位十几岁'
        '鲸鱼少女的上半身：深蓝色长发发梢渐变浅蓝、鲸鱼耳鳍、身后露出一段'
        '鲸鱼尾巴，穿着深蓝色海洋风连衣裙，温柔地微笑挥手，长发与气泡轻轻'
        '飘动。画面上部四分之三是干净的深海渐变留白，供界面内容使用。'
        '深海军蓝色调、柔和低对比、梦幻治愈，画面中没有任何文字。'
    ),
}

SEEDS = {'main': 20261101, 'panel': 20261102}


def mean_rgb(im):
    im2 = im.convert('RGB').resize((1, 1))
    return im2.getpixel((0, 0))


def ensure_visible(im, floor=32):
    """平均亮度低于 floor 就做伽马抬升，保证在深色 UI 里看得见。"""
    mean = sum(mean_rgb(im)) / 3
    if mean >= floor:
        return im, None
    gamma = 0.62
    lut = [round(255 * (i / 255) ** gamma) for i in range(256)]
    return im.point(lut * 3), mean


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--reference', default=None,
                    help='可选：参考图（保角色用）。少女形态不用参考，纯文字生成')
    ap.add_argument('--base', default='http://127.0.0.1:8188')
    ap.add_argument('--out', default=None, help='背景输出目录（默认 themes/xiaoai-whale/backgrounds）')
    args = ap.parse_args()

    out_dir = Path(args.out) if args.out else \
        Path(__file__).resolve().parents[2] / 'themes' / 'xiaoai-whale' / 'backgrounds'
    out_dir.mkdir(parents=True, exist_ok=True)

    comfy = Comfy(args.base)
    ref_name = comfy.upload_input(args.reference) if args.reference else None
    if ref_name:
        print(f'[ref] {args.reference} -> {ref_name}')

    for key in ('main', 'panel'):
        (gen_size, out_size) = SIZES[key]
        t0 = time.time()
        data, dt = generate_one(comfy, PROMPTS[key], gen_size[0], gen_size[1],
                                SEEDS[key], f'xiaoai_bg_char_{key}',
                                ref_name=ref_name)
        im = Image.open(io.BytesIO(data)).convert('RGB')
        if im.size != gen_size:
            im = im.crop((0, 0, min(im.width, gen_size[0]),
                          min(im.height, gen_size[1])))
        if im.size != out_size:
            im = im.resize(out_size, Image.LANCZOS)
        im = ImageEnhance.Contrast(im).enhance(0.92)
        lifted, before = ensure_visible(im)
        note = f' 亮度 {before} -> {mean_rgb(lifted)}' if before else \
            f' 亮度 {mean_rgb(lifted)}'
        lifted.save(out_dir / f'{key}.png')
        print(f'[bg:{key}] {dt:.0f}s{note} -> {out_dir / f"{key}.png"}')


if __name__ == '__main__':
    main()
