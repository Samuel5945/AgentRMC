#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""gen_theme.py — 桌宠主题资产生成管线（ComfyUI Qwen-Image 2.1 turbo，设计文档第 6 节）。

用法（候选帧，主命令）:
    python gen_theme.py --reference <img> --out <候选目录> \
        --states idle,listening,recording,transcribing,chat \
        --candidates 4 --size 1024 --seed 20261101

附加模式（按设计文档第 6 节的流水线拆分）:
    python gen_theme.py ... candidates    # 默认：生成候选帧 + theme.json 脚手架 + 候选对比 preview.png
    python gen_theme.py ... pick          # 启发式代选每状态 2 帧，规整 512x512，拷入 themes/<id>/pet/
    python gen_theme.py ... backgrounds   # 文生图两张背景（压对比 + 暗角）拷入 themes/<id>/backgrounds/
    python gen_theme.py ... preview       # 最终 pet 帧 + 背景拼 preview.png 存 themes/<id>/
    python gen_theme.py ... all           # 顺序执行以上全部

依赖：Pillow、requests（D:/AI_work/python_embeded/python.exe 自带）。
零下载、零新增依赖；所有中文提示词为 UTF-8 源码字符串，文件读写显式 encoding='utf-8'。
"""
import argparse
import json
import os
import sys
import time
from pathlib import Path

import requests
from PIL import Image, ImageDraw, ImageEnhance, ImageFilter, ImageFont

sys.stdout.reconfigure(encoding='utf-8')
sys.stderr.reconfigure(encoding='utf-8')

# ---------------------------------------------------------------------------
# 模型与图结构常量（与已验证的 D:/AI_work/poster_test.py 完全一致）
# ---------------------------------------------------------------------------
UNET = 'qwen_image_2.1_turbo_int8_convrot.safetensors'
CLIP = 'qwen3vl_8b_int8_convrot.safetensors'
VAE = 'qwen_image_2.1_vae_bf16.safetensors'
STEPS = 8
POLL_INTERVAL = 4          # 秒
PER_IMAGE_TIMEOUT = 900    # 单张上限 15 分钟

# ---------------------------------------------------------------------------
# 提示词：角色一致性骨架（从参考图特征提炼）+ 状态姿势词 + 透明背景要求
# ---------------------------------------------------------------------------
CHARACTER = (
    'Q版chibi比例的可爱鲸鱼娘少女，深蓝色长直发，头戴白色女仆头饰，'
    '身穿白色女仆连衣裙、蓝色缎带镶边和白色围裙，身后一条巨大的深蓝色鲸鱼尾巴，'
    '圆润简洁的卡通游戏立绘风格，全身像，单个角色完整居中，'
)

POSES = {
    'idle':         '标准直立站姿，双手自然下垂放在身体两侧，面带微微微笑，平静地看着前方，',
    'listening':    '身体微微前倾，好奇地睁大眼睛注视着前方，双手自然垂在身前，',
    'recording':    '张着嘴巴正在说话，一只手举起靠近嘴边做出说话的手势，表情认真，另一只手自然下垂，',
    'transcribing': '歪着头思考，一只手的食指轻轻点在脸颊旁边，眼睛看向斜上方，若有所思，',
    'chat':         '开心地拍手，双手举在胸前拍掌，眼睛弯成月牙形，露出灿烂的笑容，',
}

ALPHA_WORDS = (
    '纯透明背景，背景完全透明，无背景，无地面，无阴影，无阴影底座，'
    'RGBA透明PNG素材，边缘干净利落'
)

# 备用姿势词（--states 传了未知名时报错前给出提示）
KNOWN_STATES = list(POSES.keys())

BG_PROMPTS = {
    'main': (
        '深夜海洋主题的电脑桌面壁纸背景，深海般极暗的深蓝色纯色渐变底，'
        '低对比度、低亮度、低饱和，画面绝大部分是干净的留白，'
        '只有零星几颗微弱的半透明小气泡和非常柔和的圆形光斑散落在角落边缘，'
        '中央区域和上方区域完全干净，安静沉稳的深海氛围，'
        '简洁现代的UI背景设计，无文字，无人物，无水印，无边框'
    ),
    'panel': (
        '深夜海洋主题的软件面板背景，非常暗的深蓝色纯色底，极低对比度，几乎纯色，'
        '仅在右下角有两三个极微弱的半透明小气泡和一点柔和光晕，'
        '整体干净安静，给前景文字留出大量空间，'
        '简洁现代的UI背景设计，无文字，无人物，无水印'
    ),
}

# 背景尺寸：1080 可被 VAE 步长 8 整除，直接生成；
# 900 不能被 8 整除（900/8=112.5），按 1600x896 生成后 Pillow 精确缩放到 1600x900。
BG_SIZES = {
    'main':  ((1728, 1080), None),
    'panel': ((1600, 896), (1600, 900)),
}

PERSONA = (
    '你是「小爱」，AgentRMC 的语音桌宠，一只化身女仆的温柔鲸鱼娘。'
    '你性格开朗体贴，说话简洁口语化，回答通常不超过两三句话，适合语音朗读，'
    '喜欢用海洋、鲸鱼和深海打比方。用户让你执行本机操作时，用平实的语言汇报结果'
    '并主动给出下一步建议；遇到不确定的事情先向用户确认再动手。'
    '全程使用中文交流，除非用户使用其他语言。'
)


# ---------------------------------------------------------------------------
# ComfyUI HTTP 客户端（提交/轮询写法照抄 poster_test.py，换 requests 实现）
# ---------------------------------------------------------------------------
class Comfy:
    def __init__(self, base):
        self.base = base.rstrip('/')
        self.sess = requests.Session()
        self.sess.trust_env = False  # 本机直连，忽略系统代理（等价 ProxyHandler({})）

    def upload_input(self, path):
        """上传参考图到 ComfyUI input 目录，返回 LoadImage 需要的文件名。"""
        name = os.path.basename(path)
        with open(path, 'rb') as f:
            r = self.sess.post(
                self.base + '/upload/image',
                files={'image': (name, f, 'application/octet-stream')},
                data={'overwrite': 'true'},
                timeout=60,
            )
        r.raise_for_status()
        d = r.json()
        sub = d.get('subfolder') or ''
        return (sub + '/' if sub else '') + d['name']

    def submit(self, graph):
        r = self.sess.post(self.base + '/prompt', json={'prompt': graph}, timeout=60)
        if r.status_code != 200:
            raise RuntimeError('VALIDATION: ' + r.text[:400])
        return r.json()['prompt_id']

    def wait(self, pid, timeout=PER_IMAGE_TIMEOUT):
        """轮询 /history/{pid} 直到 completed 或 execution_error（poster_test.py 模式）。"""
        t0 = time.time()
        while time.time() - t0 < timeout:
            time.sleep(POLL_INTERVAL)
            h = self.sess.get(f'{self.base}/history/{pid}', timeout=15).json()
            if h:
                st = h[pid]['status']
                for m in st.get('messages', []):
                    if m[0] == 'execution_error':
                        raise RuntimeError(
                            f"execution_error: {m[1].get('node_type')} "
                            f"{str(m[1].get('exception_message', ''))[:300]}")
                if st.get('completed'):
                    return h[pid]['outputs'], time.time() - t0
        raise TimeoutError(f'prompt {pid} 超过 {timeout}s 未完成')

    def download(self, item):
        r = self.sess.get(
            self.base + '/view',
            params={'filename': item['filename'],
                    'subfolder': item.get('subfolder', ''),
                    'type': item.get('type', 'output')},
            timeout=120,
        )
        r.raise_for_status()
        return r.content


def build_graph(prompt, width, height, seed, prefix, ref_name=None):
    """UNETLoader + CLIPLoader + VAELoader + TextEncodeQwenImage21 +
    EmptySD3LatentImage + KSampler(turbo 8 步 cfg1.0 euler/simple) + VAEDecode + SaveImage。"""
    g = {
        '1': {'class_type': 'UNETLoader',
              'inputs': {'unet_name': UNET, 'weight_dtype': 'default'}},
        '2': {'class_type': 'CLIPLoader',
              'inputs': {'clip_name': CLIP, 'type': 'qwen_image', 'device': 'default'}},
        '3': {'class_type': 'VAELoader', 'inputs': {'vae_name': VAE}},
        '4': {'class_type': 'TextEncodeQwenImage21',
              'inputs': {'clip': ['2', 0], 'prompt': prompt,
                         'negative_prompt': '', 'resolution': 1024}},
        '5': {'class_type': 'EmptySD3LatentImage',
              'inputs': {'width': width, 'height': height, 'batch_size': 1}},
        '6': {'class_type': 'KSampler',
              'inputs': {'model': ['1', 0], 'positive': ['4', 0], 'negative': ['4', 1],
                         'latent_image': ['5', 0], 'seed': seed, 'steps': STEPS,
                         'cfg': 1.0, 'sampler_name': 'euler', 'scheduler': 'simple',
                         'denoise': 1.0}},
        '7': {'class_type': 'VAEDecode', 'inputs': {'samples': ['6', 0], 'vae': ['3', 0]}},
        '8': {'class_type': 'SaveImage',
              'inputs': {'images': ['7', 0], 'filename_prefix': prefix}},
    }
    if ref_name:
        # 参考图引导（保角色）：autogrow 参考图输入的 API 键是 "images.image_1"
        # （comfy_api/_io.py: finalize_prefix(["images"], "image_1")），
        # 同时接 VAE 使参考图以 reference latents 拼进文本序列
        # （nodes_qwen.py: vae 不接则只走视觉塔）。
        g['9'] = {'class_type': 'LoadImage', 'inputs': {'image': ref_name}}
        g['4']['inputs']['images.image_1'] = ['9', 0]
        g['4']['inputs']['vae'] = ['3', 0]
    return g


def generate_one(comfy, prompt, width, height, seed, prefix, ref_name=None):
    """提交一张图并下载，返回 (png_bytes, 耗时秒)。"""
    pid = comfy.submit(build_graph(prompt, width, height, seed, prefix, ref_name))
    outputs, dt = comfy.wait(pid)
    for node_out in outputs.values():
        for item in node_out.get('images', []):
            if item.get('type') == 'output':
                return comfy.download(item), dt
    raise RuntimeError('history 中没有 output 图片')


# ---------------------------------------------------------------------------
# alpha 校验 / 候选质检
# ---------------------------------------------------------------------------
def alpha_stats(im):
    """返回 (有alpha通道, 覆盖率, 边缘干净(四周8px全透明), 备注列表)。"""
    notes = []
    has_alpha = im.mode in ('RGBA', 'LA') or (im.mode == 'P' and 'transparency' in im.info)
    if not has_alpha:
        return False, 0.0, False, ['无alpha通道(mode=%s)' % im.mode]
    rgba = im.convert('RGBA')
    a = rgba.getchannel('A')
    hist = a.histogram()
    total = rgba.width * rgba.height
    solid = sum(hist[9:])  # alpha>8 视为实体
    if solid == 0:
        return True, 0.0, False, ['alpha全空']
    coverage = solid / total
    w, h = rgba.size
    strips = [a.crop((0, 0, w, 8)), a.crop((0, h - 8, w, h)),
              a.crop((0, 0, 8, h)), a.crop((w - 8, 0, w, h))]
    edge_max = max(s.getextrema()[1] for s in strips)
    edge_ok = edge_max <= 8
    if not edge_ok:
        notes.append(f'边缘有内容(edge_max={edge_max})，疑似截断')
    if not (0.15 <= coverage <= 0.70):
        notes.append(f'覆盖率{coverage:.0%}超出15%~70%')
    return True, coverage, edge_ok, notes


def frame_diff(p1, p2):
    """两帧姿态差异：alpha>8 区域内的平均 RGB 绝对差（0~255）。"""
    a = Image.open(p1).convert('RGBA').resize((256, 256))
    b = Image.open(p2).convert('RGBA').resize((256, 256))
    pa, pb = list(a.getdata()), list(b.getdata())
    tot = n = 0
    for (r1, g1, b1, a1), (r2, g2, b2, a2) in zip(pa, pb):
        if a1 > 8 or a2 > 8:
            tot += abs(r1 - r2) + abs(g1 - g2) + abs(b1 - b2)
            n += 3
    return tot / max(n, 1)


# ---------------------------------------------------------------------------
# 模式一：候选帧生成 + theme.json 脚手架 + 候选对比 preview.png
# ---------------------------------------------------------------------------
def mode_candidates(args, comfy):
    out = Path(args.out)
    states = [s.strip() for s in args.states.split(',') if s.strip()]
    for s in states:
        if s not in POSES:
            sys.exit(f'未知状态 {s}，可用：{",".join(KNOWN_STATES)}')
    ref_name = None
    if args.reference:
        ref_name = comfy.upload_input(args.reference)
        print(f'参考图已上传: {ref_name}')

    manifest = {}
    timings = []
    for si, state in enumerate(states):
        sdir = out / state
        sdir.mkdir(parents=True, exist_ok=True)
        prompt = CHARACTER + POSES[state] + ALPHA_WORDS
        entries = []
        fail_streak = 0
        for j in range(args.candidates):
            seed = args.seed + si * 1000 + j  # 每状态固定 seed 顺序出候选
            prefix = f'{args.theme_id}_{state}_c{j:02d}'
            try:
                data, dt = generate_one(comfy, prompt, args.size, args.size,
                                        seed, prefix, ref_name)
            except Exception as e:  # noqa: BLE001 — 失败要继续下一张
                fail_streak += 1
                print(f'  [{state} cand{j}] 失败({fail_streak}连): {e}')
                if fail_streak >= 3:
                    print(f'  [{state}] 连续失败 3 次，跳过该状态剩余候选')
                    break
                continue
            fail_streak = 0
            timings.append(dt)
            path = sdir / f'cand_{j}.png'
            path.write_bytes(data)
            im = Image.open(path)
            ok, cov, edge_ok, notes = alpha_stats(im)
            entries.append({'file': str(path), 'valid': ok and edge_ok,
                            'coverage': round(cov, 4), 'edge_ok': edge_ok,
                            'notes': notes, 'seconds': round(dt, 1)})
            print(f'  [{state} cand{j}] {im.size} {im.mode} 覆盖{cov:.0%} '
                  f'边缘{"OK" if edge_ok else "脏"} {dt:.0f}s '
                  f'{"; ".join(notes) if notes else "合格"}')
        manifest[state] = entries

    # 落盘记录（供 pick 与人工复核）
    with open(out / 'candidates_manifest.json', 'w', encoding='utf-8') as f:
        json.dump({'theme_id': args.theme_id, 'seed': args.seed,
                   'size': args.size, 'states': manifest}, f,
                  ensure_ascii=False, indent=2)

    good_states = [s for s, es in manifest.items()
                   if any(e['valid'] for e in es)]
    write_theme_json(out / 'theme.json', args, good_states)
    make_candidates_preview(out, manifest)
    total = sum(timings)
    n = len(timings)
    print(f'[candidates] 完成：成功 {n} 张，平均 {total / max(n, 1):.0f}s/张，'
          f'总计 {total / 60:.1f} 分钟；候选目录 {out}')


def write_theme_json(path, args, good_states):
    states = {}
    for s in good_states:
        states[s] = {'frames': [f'pet/{s}_0.png', f'pet/{s}_1.png'],
                     'fps': 2, 'bob': 3, 'tilt': 2}
    data = {
        'id': args.theme_id,
        'displayName': args.display_name,
        'author': '',
        'character': {'name': args.character_name, 'persona': PERSONA},
        'palette': {},  # 留空 = 继承 classic 配色
        'pet': {'canvas': 512, 'display_size': 84, 'ring': True, 'states': states},
        'backgrounds': {'main': 'backgrounds/main.png',
                        'panel': 'backgrounds/panel.png', 'dim': 0.55},
    }
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)
    print(f'theme.json 脚手架已写入: {path}（含状态: {", ".join(good_states) or "无"}）')


def _font(size=16):
    for cand in (r'C:\Windows\Fonts\msyh.ttc', r'C:\Windows\Fonts\simhei.ttf',
                 r'C:\Windows\Fonts\arial.ttf'):
        try:
            return ImageFont.truetype(cand, size)
        except Exception:  # noqa: BLE001
            continue
    return ImageFont.load_default()


def make_candidates_preview(out, manifest, cell=240):
    states = [s for s, es in manifest.items() if es]
    if not states:
        return
    maxc = max(len(es) for es in manifest.values())
    label_h = 26
    gap = 6
    W = maxc * (cell + gap) + gap
    H = len(states) * (cell + label_h + gap) + gap + 30
    canvas = Image.new('RGB', (W, H), (24, 26, 32))
    d = ImageDraw.Draw(canvas)
    font = _font(15)
    d.text((gap, 4), f'{out.name} 候选帧对比（绿框=合格，红框=不合格）',
           fill=(220, 224, 232), font=font)
    for ri, state in enumerate(states):
        y = 30 + ri * (cell + label_h + gap)
        for ci, e in enumerate(manifest[state]):
            x = gap + ci * (cell + gap)
            try:
                im = Image.open(e['file']).convert('RGBA')
                im.thumbnail((cell, cell))
                bg = Image.new('RGBA', (cell, cell), (40, 44, 54, 255))
                bg.paste(im, ((cell - im.width) // 2, (cell - im.height) // 2), im)
                canvas.paste(bg, (x, y))
                color = (90, 200, 110) if e['valid'] else (220, 90, 90)
                d.rectangle((x, y, x + cell, y + cell), outline=color, width=2)
            except Exception as ex:  # noqa: BLE001
                d.rectangle((x, y, x + cell, y + cell), outline=(220, 90, 90), width=2)
                d.text((x + 6, y + cell // 2), f'读取失败 {ex}', fill=(255, 120, 120), font=font)
            tag = f'{state} #{ci} {e["coverage"]:.0%}'
            d.text((x + 2, y + cell + 3), tag, fill=(200, 205, 215), font=font)
    canvas.save(out / 'preview.png')
    print(f'候选对比图已写入: {out / "preview.png"}')


# ---------------------------------------------------------------------------
# 模式二：启发式代选帧 → 512x512 规整 → themes/<id>/pet/
# ---------------------------------------------------------------------------
def pick_for_state(entries, cand_dir):
    """返回 (选中的候选文件列表[≤2], 说明列表)。启发式：
    硬条件=alpha 有效且边缘干净；覆盖率 15%~70% 优先；两帧间像素差异尽量大。"""
    cands = []
    for e in entries:
        p = Path(e['file'])
        if not p.exists():
            continue
        im = Image.open(p)
        ok, cov, edge_ok, notes = alpha_stats(im)
        strict = ok and edge_ok and 0.15 <= cov <= 0.70
        score = -abs(cov - 0.40)          # 覆盖率越接近 40% 越好
        if not edge_ok:
            score -= 1.0
        if not ok:
            score -= 5.0
        cands.append({'file': p, 'strict': strict, 'cov': cov, 'edge': edge_ok,
                      'score': score})
    strict = [c for c in cands if c['strict']]
    pool = strict if len(strict) >= 2 else (cands if cands else [])
    reasons = []
    if not pool:
        return [], ['无合格候选']
    pool.sort(key=lambda c: -c['score'])
    first = pool[0]
    if first not in strict and strict:
        first = strict[0]
    pick = [first]
    rest = [c for c in pool if c is not first]
    if rest:
        diffs = sorted(((frame_diff(first['file'], c['file']), c) for c in rest),
                       key=lambda t: -t[0])
        second = diffs[0][1]
        if diffs[0][0] < 2.0:
            reasons.append(f'两帧像素差异偏小({diffs[0][0]:.1f})，程序动画下仍可感知')
        pick.append(second)
    else:
        pick.append(first)  # 只有 1 张合格：两帧同图兜底
    for i, c in enumerate(pick):
        tag = '严格合格' if c['strict'] else '放宽（候选不足）'
        reasons.append(f'帧{i}: {c["file"].name} 覆盖率{c["cov"]:.0%} '
                       f'边缘{"干净" if c["edge"] else "脏"} [{tag}]')
    return [c['file'] for c in pick], reasons


def normalize_512(src, dst, canvas=512, max_ratio=0.95):
    """裁 alpha 内容包围盒 → 等比缩放 → 512x512 居中；alpha<=8 清零去杂边。"""
    im = Image.open(src).convert('RGBA')
    a = im.getchannel('A').point(lambda v: 255 if v > 8 else 0)
    bbox = a.getbbox()
    if bbox:
        im = im.crop(bbox)
    max_side = int(canvas * max_ratio)
    s = min(max_side / im.width, max_side / im.height)
    im = im.resize((max(1, round(im.width * s)), max(1, round(im.height * s))),
                   Image.LANCZOS)
    im.putdata([(r, g, b, 0 if al <= 8 else al)
                for r, g, b, al in im.getdata()])
    bg = Image.new('RGBA', (canvas, canvas), (0, 0, 0, 0))
    bg.paste(im, ((canvas - im.width) // 2, (canvas - im.height) // 2), im)
    bg.save(dst)


def mode_pick(args):
    out = Path(args.out)
    themes_dir = Path(args.themes_dir) if args.themes_dir else out.parent.parent / 'themes'
    pet_dir = themes_dir / args.theme_id / 'pet'
    pet_dir.mkdir(parents=True, exist_ok=True)
    manifest_path = out / 'candidates_manifest.json'
    with open(manifest_path, encoding='utf-8') as f:
        manifest = json.load(f)['states']

    picked_states = {}
    for state, entries in manifest.items():
        if not entries:
            continue
        files, reasons = pick_for_state(entries, out)
        if not files:
            print(f'[{state}] 无可用候选，跳过（运行时回落 idle）')
            continue
        for i, src in enumerate(files[:2]):
            normalize_512(src, pet_dir / f'{state}_{i}.png')
        picked_states[state] = True
        print(f'[{state}] ' + '；'.join(reasons))

    # 最终 theme.json（只写挑选成功的状态；chat 缺省时运行时回落 idle）
    class A:  # noqa: N801 — 复用 write_theme_json 的最小视图
        pass
    a = A()
    a.theme_id, a.display_name = args.theme_id, args.display_name
    a.character_name = args.character_name
    write_theme_json(themes_dir / args.theme_id / 'theme.json', a,
                     list(picked_states))
    print(f'[pick] 完成：{len(picked_states)} 个状态入库 → {pet_dir}')


# ---------------------------------------------------------------------------
# 模式三：背景两张（文生图 + 压对比 + 暗角）
# ---------------------------------------------------------------------------
def add_vignette(im, max_darken=0.25):
    """椭圆遮罩暗角：边缘最多压暗 25%，中心不压。"""
    w, h = im.size
    mask = Image.new('L', (w, h), int(255 * max_darken))
    d = ImageDraw.Draw(mask)
    d.ellipse((-w * 0.30, -h * 0.30, w * 1.30, h * 1.30), fill=0)
    mask = mask.filter(ImageFilter.GaussianBlur(max(w, h) * 0.08))
    black = Image.new('RGB', (w, h), (0, 0, 0))
    return Image.composite(black, im.convert('RGB'), mask)


def mode_backgrounds(args, comfy):
    themes_dir = Path(args.themes_dir) if args.themes_dir else \
        Path(args.out).parent.parent / 'themes'
    bg_dir = themes_dir / args.theme_id / 'backgrounds'
    bg_dir.mkdir(parents=True, exist_ok=True)
    for key, prompt in BG_PROMPTS.items():
        (size, resize_to) = BG_SIZES[key]
        t0 = time.time()
        data, dt = generate_one(comfy, prompt, size[0], size[1],
                                args.seed + 70000 + (0 if key == 'main' else 1),
                                f'{args.theme_id}_bg_{key}')
        import io as _io
        im = Image.open(_io.BytesIO(data)).convert('RGB')
        if im.size != size:  # ComfyUI 按 latent 步长取整，裁/缩到目标
            im = im.crop((0, 0, min(im.width, size[0]), min(im.height, size[1])))
        if resize_to:
            im = im.resize(resize_to, Image.LANCZOS)
        im = ImageEnhance.Contrast(im).enhance(0.85)   # 压对比约 0.85 倍
        im = add_vignette(im, 0.25)                    # 四角暗角压 25%
        im.save(bg_dir / f'{key}.png')
        print(f'[bg:{key}] {im.size} 生成{dt:.0f}s 总{time.time() - t0:.0f}s '
              f'→ {bg_dir / f"{key}.png"}')
    print(f'[backgrounds] 完成 → {bg_dir}')


# ---------------------------------------------------------------------------
# 模式四：最终 preview.png（pet 全部状态帧 + 两背景拼图）
# ---------------------------------------------------------------------------
def mode_preview(args):
    themes_dir = Path(args.themes_dir) if args.themes_dir else \
        Path(args.out).parent.parent / 'themes'
    tdir = themes_dir / args.theme_id
    pet_dir, bg_dir = tdir / 'pet', tdir / 'backgrounds'
    cell = 190
    label_h = 24
    gap = 6
    frames = []
    states = ['idle', 'listening', 'recording', 'transcribing', 'chat']
    for s in states:
        for i in (0, 1):
            p = pet_dir / f'{s}_{i}.png'
            if p.exists():
                frames.append((f'{s}_{i}', p))
    bg_w = 420
    W = max(10 * (cell + gap) + gap, 2 * (bg_w + gap) + gap)
    H = 30 + (cell + label_h + gap) * 2 + (bg_w * 900 // 1600 + label_h) + gap
    canvas = Image.new('RGB', (W, H), (24, 26, 32))
    d = ImageDraw.Draw(canvas)
    font = _font(14)
    d.text((gap, 6), f'{args.theme_id} 最终资产预览：上=桌宠帧(512入库存) 下=背景',
           fill=(220, 224, 232), font=font)
    for idx, (tag, p) in enumerate(frames):
        x = gap + idx * (cell + gap)
        y = 30 + (idx // 5) * (cell + label_h + gap)
        im = Image.open(p).convert('RGBA')
        im.thumbnail((cell, cell))
        bg = Image.new('RGBA', (cell, cell), (40, 44, 54, 255))
        bg.paste(im, ((cell - im.width) // 2, (cell - im.height) // 2), im)
        canvas.paste(bg, (x, y))
        d.text((x + 2, y + cell + 2), tag, fill=(200, 205, 215), font=font)
    bg_y = 30 + (cell + label_h + gap) * 2
    for idx, key in enumerate(('main', 'panel')):
        p = bg_dir / f'{key}.png'
        if not p.exists():
            continue
        im = Image.open(p).convert('RGB')
        im.thumbnail((bg_w, bg_w * im.height // im.width))
        x = gap + idx * (bg_w + gap)
        canvas.paste(im, (x, bg_y))
        d.text((x + 2, bg_y + im.height + 2), f'backgrounds/{key}.png {im.size}',
               fill=(200, 205, 215), font=font)
    canvas.save(tdir / 'preview.png')
    print(f'[preview] 最终预览图已写入: {tdir / "preview.png"}')


# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        description='桌宠主题资产生成管线（ComfyUI Qwen-Image 2.1 turbo）')
    ap.add_argument('mode', nargs='?', default='candidates',
                    choices=['candidates', 'pick', 'backgrounds', 'preview', 'all'])
    ap.add_argument('--reference', help='角色参考图（挂 image_1 做参考引导）')
    ap.add_argument('--out', required=True, help='候选输出目录')
    ap.add_argument('--states', default='idle,listening,recording,transcribing,chat')
    ap.add_argument('--candidates', type=int, default=4)
    ap.add_argument('--size', type=int, default=1024, help='候选帧边长（32 的倍数）')
    ap.add_argument('--seed', type=int, default=20261101)
    ap.add_argument('--base-url', default='http://127.0.0.1:8188')
    ap.add_argument('--theme-id', default='xiaoai-whale')
    ap.add_argument('--themes-dir', default=None,
                    help='themes 根目录（默认 <out>/../themes）')
    ap.add_argument('--display-name', default='鲸鱼娘')
    ap.add_argument('--character-name', default='鲸鱼娘')
    args = ap.parse_args()
    if args.size % 32:
        sys.exit('--size 必须是 32 的倍数')

    if args.mode in ('candidates', 'backgrounds', 'all'):
        comfy = Comfy(args.base_url)
    else:
        comfy = None

    if args.mode in ('candidates', 'all'):
        mode_candidates(args, comfy)
    if args.mode in ('pick', 'all'):
        mode_pick(args)
    if args.mode in ('backgrounds', 'all'):
        mode_backgrounds(args, comfy)
    if args.mode in ('preview', 'all'):
        mode_preview(args)


if __name__ == '__main__':
    main()
