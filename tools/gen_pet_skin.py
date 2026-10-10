# -*- coding: utf-8 -*-
"""
gen_pet_skin.py —— 生成内置桌宠皮肤（resources/pets/default/）

为什么要有这个脚本：
    SRS 4.1 / 4.2 要求桌宠有形象，且明确排除 Live2D / 3D（SRS 7.3）。
    最终形态是"序列帧目录 + skin.json"，所以资源本身就是几十张 PNG。
    但把 PNG 二进制直接扔进仓库不利于审查（看不出画的是什么、改不动），
    因此这里保留**生成脚本**作为可读、可复现的"源"：
      * 纯 Pillow 绘制，扁平矢量风格，无外部素材依赖；
      * 每帧都是程序算出来的，换配色/换尺寸改几个常量即可；
      * 生成的 PNG 与脚本一并保留（用户要求"源文件和软件成品都保存"）。

设计约束（都来自 SRS）：
    * 不用 3D / 不用 Live2D —— 所以是 2D 扁平几何图形。
    * 轻量 —— 单帧 256x256、每状态帧数控制在 6~8 张以内。
    * 低资源 —— 调色板只用几个纯色，无渐变噪点，PNG 调色板压缩后很小。
    * 状态可分辨 —— 6 个状态靠"配色 + 眼睛形状 + 附加符号"区分，
      而不是靠亮度微调，这样小尺寸下也能一眼看出宠物在干什么。

用法：
    python tools/gen_pet_skin.py

输出：
    resources/pets/default/skin.json
    resources/pets/default/<state>/000.png ...
"""

import json
import math
import os

from PIL import Image, ImageDraw

# ---------------------------------------------------------------------------
# 常量
# ---------------------------------------------------------------------------

CANVAS = 256          # 单帧画布边长（像素）。256 足够缩放，又不浪费内存。
SS = 4                # 超采样倍率。Pillow 没有抗锯齿的圆/多边形，
                      # 先按 4 倍画再缩回来，等于手动 MSAA。
                      # 这是最省事的"让边缘不锯齿"办法。

OUT_ROOT = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "resources", "pets", "default",
)

# 状态 -> (帧数, 帧间隔毫秒, 主色, 阴影色, 点缀色)
#
# 帧间隔的取值逻辑：越"忙"的状态动得越快。
#   idle 慢悠悠地呼吸；working 明显更快；success 是一串"跳一下"。
#   所有间隔都 >= 120ms，避免低端机上定时器挤在一起白白烧 CPU（SRS 6.1）。
STATES = {
    # name: (frames, interval, body, shade, accent)
    "idle":      (6, 220, (108, 160, 220), (84, 130, 190), (255, 255, 255)),
    "talking":   (6, 150, (108, 160, 220), (84, 130, 190), (255, 235, 140)),
    "working":   (6, 140, (120, 150, 175), (95, 122, 148), (140, 220, 200)),
    "success":   (8, 130, (120, 200, 150), (92, 168, 122), (255, 246, 170)),
    "failure":   (6, 200, (215, 130, 130), (185, 100, 100), (255, 255, 255)),
    # listening 的主色要跟 idle 拉开距离。第一版给的 (170,145,215) 饱和度偏低，
    # 缩到桌面上和 idle 的蓝几乎分不出来 —— 倾听是个需要用户立刻察觉的状态
    # （SRS 4.11 要求录音/倾听有明显提示），所以这里加深加饱和。
    "listening": (6, 160, (162, 124, 222), (128, 92, 188), (255, 255, 255)),
}

# 眼睛的五种画法。用形状而不是颜色区分状态，
# 是考虑到色盲用户 + 小尺寸下的可辨识度。
EYE_NORMAL = "normal"
EYE_HAPPY = "happy"       # ^ ^
EYE_FOCUS = "focus"       # 半闭，看着前方
EYE_SAD = "sad"           # 下垂的 > <
EYE_WIDE = "wide"         # 睁大

STATE_EYES = {
    "idle": EYE_NORMAL,
    "talking": EYE_NORMAL,
    "working": EYE_FOCUS,
    "success": EYE_HAPPY,
    "failure": EYE_SAD,
    "listening": EYE_WIDE,
}


# ---------------------------------------------------------------------------
# 基础绘制工具（坐标一律用 0..256 的"逻辑坐标"，内部乘 SS 放大）
# ---------------------------------------------------------------------------

def _s(v):
    """逻辑坐标 -> 超采样后的实际像素坐标。"""
    return v * SS


def _box(x0, y0, x1, y1):
    return [_s(x0), _s(y0), _s(x1), _s(y1)]


def draw_ellipse(d, x0, y0, x1, y1, fill=None, outline=None, width=1):
    if outline is None:
        d.ellipse(_box(x0, y0, x1, y1), fill=fill)
    else:
        d.ellipse(_box(x0, y0, x1, y1), fill=fill,
                  outline=outline, width=max(1, int(width * SS)))


def draw_rounded(d, x0, y0, x1, y1, r, fill=None, outline=None, width=1):
    if outline is None:
        d.rounded_rectangle(_box(x0, y0, x1, y1), radius=_s(r), fill=fill)
    else:
        d.rounded_rectangle(_box(x0, y0, x1, y1), radius=_s(r), fill=fill,
                            outline=outline, width=max(1, int(width * SS)))


def draw_polygon(d, pts, fill=None, outline=None, width=1):
    p = [(_s(x), _s(y)) for (x, y) in pts]
    if outline is None:
        d.polygon(p, fill=fill)
    else:
        d.polygon(p, fill=fill, outline=outline,
                  width=max(1, int(width * SS)))


def draw_line(d, pts, fill, width=2):
    p = [(_s(x), _s(y)) for (x, y) in pts]
    d.line(p, fill=fill, width=max(1, int(width * SS)), joint="curve")


def new_canvas():
    """透明底 + 超采样画布。

    注意用 RGBA：桌宠是无边框窗口，背景必须透明，
    否则会出现一块灰色方块（SRS 4.1 PET-003）。
    """
    return Image.new("RGBA", (CANVAS * SS, CANVAS * SS), (0, 0, 0, 0))


def finish(img):
    """缩回目标尺寸，得到抗锯齿结果。"""
    return img.resize((CANVAS, CANVAS), Image.LANCZOS)


# ---------------------------------------------------------------------------
# 形象本体
# ---------------------------------------------------------------------------

def body_metrics(state, t):
    """按状态与进度 t(0..1) 返回身体几何。

    把"身体怎么变形"集中在一个函数里，画影子/画身体/画五官
    都读同一组数字，就不会出现"影子跟着动而身体不动"这种鬼影。
    """
    # 呼吸/弹跳幅度
    if state == "idle":
        # 幅度 2px 的呼吸。刻意做得很小 —— 桌宠常年挂在桌面上，
        # 大幅晃动会让人分心甚至晕（SRS 4.1 要求"不干扰工作"）。
        bob = math.sin(t * 2 * math.pi) * 2.0
        squash = 1.0 + math.sin(t * 2 * math.pi) * 0.012
    elif state == "talking":
        # 说话：整体轻微上下 + 身体拉长一点点（像在换气）
        bob = math.sin(t * 2 * math.pi) * 3.0
        squash = 1.0 + math.sin(t * 4 * math.pi) * 0.02
    elif state == "working":
        # 干活：小幅高频抖动，像在敲键盘
        bob = math.sin(t * 6 * math.pi) * 1.5
        squash = 1.0 + math.sin(t * 6 * math.pi) * 0.015
    elif state == "success":
        # 成功：跳两下。用 abs(sin) 做出"落地"的硬感，
        # 而不是平滑正弦 —— 平滑的看起来像飘，不像跳。
        hop = abs(math.sin(t * 2 * math.pi))
        bob = -hop * 14.0
        # 落地瞬间压扁，起跳拉长
        squash = 1.0 + (0.06 * (1.0 - hop) - 0.05 * hop)
    elif state == "failure":
        # 失败：低头、下垂，整体下沉 3px
        bob = 3.0 + math.sin(t * 2 * math.pi) * 1.0
        squash = 0.97
    elif state == "listening":
        # 倾听：整体前倾抬高一点点，头部偏向一侧
        bob = -2.0 + math.sin(t * 2 * math.pi) * 1.5
        squash = 1.0 + math.sin(t * 2 * math.pi) * 0.008
    else:
        bob, squash = 0.0, 1.0

    cx = 128.0
    cy = 132.0 + bob

    half_w = 62.0 / squash
    half_h = 54.0 * squash

    return cx, cy, half_w, half_h


def draw_shadow(d, cx, cy, half_w, half_h, state):
    """地面投影。

    有了影子，宠物才像"站在桌面上"而不是"贴在屏幕上"。
    成功跳跃时影子要变小变淡 —— 这是跳跃感的来源之一。
    """
    if state == "success":
        # 借用身体的 bob 反推高度（这里再算一次，避免把状态传进来太深）
        lift = max(0.0, (132.0 - (cy - 0)) / 14.0)
        lift = min(1.0, abs(lift))
    else:
        lift = 0.0

    sw = half_w * (0.92 - 0.22 * lift)
    sh = 9.0 * (1.0 - 0.35 * lift)
    base_y = 196.0

    # 影子用黑色 + 低透明度。Pillow 的 ellipse 不支持 alpha 渐变，
    # 所以叠三层不同透明度的椭圆，手工做软边。
    for i, (grow, alpha) in enumerate(((1.0, 40), (0.75, 55), (0.5, 70))):
        layer = Image.new("RGBA", d._image.size, (0, 0, 0, 0))
        ld = ImageDraw.Draw(layer)
        ld.ellipse(
            _box(cx - sw * grow, base_y - sh * grow,
                 cx + sw * grow, base_y + sh * grow),
            fill=(0, 0, 0, alpha),
        )
        d._image.alpha_composite(layer)


def draw_body(d, cx, cy, half_w, half_h, colors):
    body, shade, accent = colors

    # 主体
    draw_ellipse(d, cx - half_w, cy - half_h,
                 cx + half_w, cy + half_h, fill=body)

    # 顶部高光：一个偏左上、透明度低一点的椭圆，
    # 让平面色块有"圆润"的感觉，但不用真正的渐变（省 CPU + 风格统一）。
    layer = Image.new("RGBA", d._image.size, (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    ld.ellipse(_box(cx - half_w * 0.72, cy - half_h * 0.82,
                    cx + half_w * 0.18, cy - half_h * 0.05),
               fill=(255, 255, 255, 46))
    d._image.alpha_composite(layer)

    # 底部暗面：同理，压一点暗色让体积感立起来
    layer = Image.new("RGBA", d._image.size, (0, 0, 0, 0))
    ld = ImageDraw.Draw(layer)
    ld.ellipse(_box(cx - half_w * 0.85, cy + half_h * 0.35,
                    cx + half_w * 0.85, cy + half_h * 1.02),
               fill=(shade[0], shade[1], shade[2], 60))
    d._image.alpha_composite(layer)


def draw_ears(d, cx, cy, half_w, half_h, colors, state, t):
    body, shade, accent = colors

    # 耳朵随状态摆动：working 时竖起，failure 时耷拉。
    if state == "failure":
        left = 0.62      # 耷拉角度
        right = 0.62
    elif state == "listening":
        left = 1.18
        right = 1.18     # 竖起来听
    elif state == "working":
        left = 1.06
        right = 1.06
    else:
        sway = math.sin(t * 2 * math.pi) * 0.07
        left = 1.0 + sway
        right = 1.0 - sway

    ear_dx = half_w * 0.60
    ear_top = cy - half_h * 0.94
    # 耳朵做成"宽底圆头"的三角形，而不是细长尖角。
    # 第一版用了 20 宽 / 30 高的尖三角，结果看起来像天线不像耳朵 ——
    # 这里把底边加宽到 34、高度降到 24，接近猫耳的宽高比。
    ear_w = 34.0
    ear_h = 24.0

    def ear(sign, height_scale):
        """sign=-1 左耳，+1 右耳。"""
        bx = cx + sign * ear_dx
        h = ear_h * height_scale
        return [
            (bx - ear_w * 0.5, ear_top + 8),
            (bx + ear_w * 0.5, ear_top + 8),
            (bx + sign * ear_w * 0.16, ear_top - h),
        ]

    draw_polygon(d, ear(-1, left), fill=shade)
    draw_polygon(d, ear(1, right), fill=shade)

    # 耳内：同形状缩小 45%，往"耳根"方向收缩一点，做出层次
    def ear_inner(sign, height_scale):
        bx = cx + sign * ear_dx
        h = ear_h * height_scale
        return [
            (bx - ear_w * 0.24, ear_top + 6),
            (bx + ear_w * 0.24, ear_top + 6),
            (bx + sign * ear_w * 0.08, ear_top - h * 0.58),
        ]

    draw_polygon(d, ear_inner(-1, left), fill=accent)
    draw_polygon(d, ear_inner(1, right), fill=accent)


def draw_face(d, cx, cy, half_w, half_h, colors, state, t):
    body, shade, accent = colors
    eye = STATE_EYES.get(state, EYE_NORMAL)

    eye_dx = half_w * 0.36
    eye_y = cy - half_h * 0.10
    ink = (38, 44, 58)

    # 眨眼：让 idle / listening 偶尔眨一下。
    # 周期取 1（每轮动画眨一次），在 6 帧下约 1.3 秒眨一次，不频繁。
    blink_window = (state in ("idle", "listening", "talking"))
    blinking = blink_window and t > 0.83

    if blinking and eye == EYE_NORMAL:
        # 闭眼 = 两条短横线
        for sx in (-1, 1):
            draw_line(d, [(cx + sx * eye_dx - 7, eye_y),
                          (cx + sx * eye_dx + 7, eye_y)], ink, 3)

    elif eye == EYE_HAPPY:
        # ^ ^ 用两段折线
        for sx in (-1, 1):
            px = cx + sx * eye_dx
            draw_line(d, [(px - 8, eye_y + 3), (px, eye_y - 4), (px + 8, eye_y + 3)],
                      ink, 3)

    elif eye == EYE_SAD:
        # > < 或者说下垂的八字
        for sx in (-1, 1):
            px = cx + sx * eye_dx
            draw_line(d, [(px - 7, eye_y - 5), (px + 7, eye_y + 4)], ink, 3)
            draw_line(d, [(px - 7, eye_y + 4), (px + 4, eye_y + 4)], ink, 3)

    elif eye == EYE_FOCUS:
        # 半闭：椭圆被压扁成条
        for sx in (-1, 1):
            px = cx + sx * eye_dx
            draw_ellipse(d, px - 7, eye_y - 5, px + 7, eye_y + 2,
                         fill=ink)
        # 专注时加一条眉毛，显得"在认真干活"
        for sx in (-1, 1):
            px = cx + sx * eye_dx
            draw_line(d, [(px - 8, eye_y - 12), (px + 8, eye_y - 9)], ink, 2)

    elif eye == EYE_WIDE:
        # 睁大 + 高光点
        for sx in (-1, 1):
            px = cx + sx * eye_dx
            draw_ellipse(d, px - 8, eye_y - 10, px + 8, eye_y + 8, fill=ink)
            draw_ellipse(d, px - 4, eye_y - 6, px + 1, eye_y - 1,
                         fill=(255, 255, 255))

    else:  # EYE_NORMAL
        for sx in (-1, 1):
            px = cx + sx * eye_dx
            draw_ellipse(d, px - 7, eye_y - 8, px + 7, eye_y + 8, fill=ink)
            # 高光：让眼睛"活"起来的关键，没有高光会显得像死物
            draw_ellipse(d, px - 4, eye_y - 5, px + 0, eye_y - 1,
                         fill=(255, 255, 255))

    # ---- 嘴 ----
    mouth_y = cy + half_h * 0.30

    if state == "talking":
        # 说话：嘴一张一合。用 t 的两个相位，闭合时画细横线，
        # 张开时画实心椭圆 —— 比单纯改高度更清楚。
        phase = math.sin(t * 4 * math.pi)
        if phase > 0.2:
            draw_ellipse(d, cx - 9, mouth_y - 3, cx + 9, mouth_y + 9,
                         fill=(70, 48, 55))
            draw_ellipse(d, cx - 6, mouth_y + 3, cx + 6, mouth_y + 8,
                         fill=(200, 110, 120))
        else:
            draw_line(d, [(cx - 7, mouth_y + 2), (cx + 7, mouth_y + 2)],
                      ink, 2)

    elif state == "success":
        # 大笑：一张大弧
        draw_ellipse(d, cx - 12, mouth_y - 6, cx + 12, mouth_y + 10,
                     fill=(70, 48, 55))
        draw_ellipse(d, cx - 8, mouth_y, cx + 8, mouth_y + 9,
                     fill=(200, 110, 120))

    elif state == "failure":
        # 下弯的嘴
        draw_line(d, [(cx - 9, mouth_y + 5), (cx, mouth_y + 1),
                      (cx + 9, mouth_y + 5)], ink, 3)

    elif state == "listening":
        # 小圆嘴，像"哦"的一声
        draw_ellipse(d, cx - 5, mouth_y - 4, cx + 5, mouth_y + 6,
                     fill=(70, 48, 55))

    else:
        # 默认微笑
        draw_line(d, [(cx - 8, mouth_y), (cx, mouth_y + 5),
                      (cx + 8, mouth_y)], ink, 3)

    # ---- 腮红 ----
    if state in ("success", "talking", "idle"):
        layer = Image.new("RGBA", d._image.size, (0, 0, 0, 0))
        ld = ImageDraw.Draw(layer)
        for sx in (-1, 1):
            px = cx + sx * half_w * 0.60
            ld.ellipse(_box(px - 10, mouth_y - 12, px + 10, mouth_y - 1),
                       fill=(240, 140, 150, 70))
        d._image.alpha_composite(layer)


def draw_accessory(d, cx, cy, half_w, half_h, colors, state, t):
    """状态点缀符号。

    这些符号是"状态可解释性"的一部分：用户不应该需要盯着宠物猜它在干嘛。
    SRS 4.3 / 4.6 要求反馈明确，所以 working 有齿轮转圈、
    listening 有声波、failure 有小叉。
    """
    body, shade, accent = colors

    if state == "working":
        # 右上角转圈的小齿轮（用扇形拼）
        gx, gy = cx + half_w * 0.95, cy - half_h * 0.88
        r = 15.0
        angle0 = t * 2 * math.pi
        for i in range(6):
            a = angle0 + i * math.pi / 3
            x0 = gx + math.cos(a) * r * 0.62
            y0 = gy + math.sin(a) * r * 0.62
            x1 = gx + math.cos(a) * r
            y1 = gy + math.sin(a) * r
            draw_line(d, [(x0, y0), (x1, y1)], accent, 5)
        draw_ellipse(d, gx - r * 0.55, gy - r * 0.55,
                     gx + r * 0.55, gy + r * 0.55, fill=accent)
        draw_ellipse(d, gx - r * 0.22, gy - r * 0.22,
                     gx + r * 0.22, gy + r * 0.22, fill=body)

    elif state == "success":
        # 三颗小星星，大小按 t 依次点亮
        for i, (ox, oy, r) in enumerate(((-58, -46, 9), (56, -52, 7), (44, 34, 6))):
            phase = (t + i * 0.33) % 1.0
            scale = 0.55 + 0.45 * math.sin(phase * math.pi)
            sx, sy = cx + ox, cy + oy
            rr = r * scale
            draw_polygon(d, [
                (sx, sy - rr), (sx + rr * 0.30, sy - rr * 0.30),
                (sx + rr, sy), (sx + rr * 0.30, sy + rr * 0.30),
                (sx, sy + rr), (sx - rr * 0.30, sy + rr * 0.30),
                (sx - rr, sy), (sx - rr * 0.30, sy - rr * 0.30),
            ], fill=accent)

    elif state == "failure":
        # 头上一团小乌云 + 一滴汗
        cxx, cyy = cx + half_w * 0.80, cy - half_h * 0.95
        draw_ellipse(d, cxx - 18, cyy - 8, cxx + 4, cyy + 8, fill=(150, 158, 172))
        draw_ellipse(d, cxx - 6, cyy - 13, cxx + 16, cyy + 5, fill=(150, 158, 172))
        draw_ellipse(d, cxx + 2, cyy - 6, cxx + 20, cyy + 8, fill=(150, 158, 172))
        # 汗滴，随 t 往下滑
        dy = 10 + 10 * t
        draw_ellipse(d, cxx - 26, cyy - 6 + dy, cxx - 20, cyy + 4 + dy,
                     fill=(130, 190, 235))

    elif state == "listening":
        # 两侧声波弧线，向外扩散。用三段弧而不是同心圆，
        # 是因为 OpenCV/Pillow 里画弧的 API 更啰嗦，折线更直接。
        for sx in (-1, 1):
            base_x = cx + sx * (half_w + 8)
            for i in range(3):
                phase = (t + i * 0.28) % 1.0
                alpha = int(200 * (1.0 - phase))
                if alpha <= 8:
                    continue
                r = 10 + phase * 22
                layer = Image.new("RGBA", d._image.size, (0, 0, 0, 0))
                ld = ImageDraw.Draw(layer)
                # 用短折线近似弧
                pts = []
                for k in range(7):
                    a = (-0.55 + 0.22 * k)
                    pts.append((_s(base_x + sx * r * math.cos(a)),
                                _s(cy + r * math.sin(a))))
                ld.line(pts, fill=(255, 255, 255, alpha),
                        width=max(1, int(2.5 * SS)), joint="curve")
                d._image.alpha_composite(layer)

    elif state == "talking":
        # 说话时旁边飘一个小音符，位置随 t 上浮
        mx = cx - half_w * 0.95
        my = cy - half_h * 0.85 - t * 6
        layer = Image.new("RGBA", d._image.size, (0, 0, 0, 0))
        ld = ImageDraw.Draw(layer)
        ld.ellipse(_box(mx - 7, my + 4, mx + 3, my + 13),
                   fill=(accent[0], accent[1], accent[2], 210))
        ld.line([(_s(mx + 3), _s(my + 11)), (_s(mx + 3), _s(my - 8)),
                 (_s(mx + 13), _s(my - 11))],
                fill=(accent[0], accent[1], accent[2], 210),
                width=max(1, int(2.5 * SS)), joint="curve")
        d._image.alpha_composite(layer)


def render_frame(state, index, count):
    """渲染一帧。t 归一化到 [0,1)。"""
    t = index / float(count)

    img = new_canvas()
    d = ImageDraw.Draw(img)
    # Pillow 的 ImageDraw 拿不到底图，这里挂上去供 alpha_composite 使用
    d._image = img

    colors = STATES[state][2:5]
    cx, cy, half_w, half_h = body_metrics(state, t)

    draw_shadow(d, cx, cy, half_w, half_h, state)
    draw_ears(d, cx, cy, half_w, half_h, colors, state, t)
    draw_body(d, cx, cy, half_w, half_h, colors)
    draw_face(d, cx, cy, half_w, half_h, colors, state, t)
    draw_accessory(d, cx, cy, half_w, half_h, colors, state, t)

    return finish(img)


def main():
    manifest = {
        "schema_version": 1,
        "id": "default",
        "displayName": {
            "zh-CN": "默认小伙伴",
            "en-US": "Default Buddy",
            "ja-JP": "デフォルトの相棒",
        },
        "description": {
            "zh-CN": "程序化绘制的扁平风格内置形象，6 个状态各一组序列帧。",
            "en-US": "Procedurally drawn flat-style built-in character, one frame sequence per state.",
            "ja-JP": "プログラムで描画したフラット調の内蔵キャラクター。状態ごとに連番フレーム。",
        },
        "canvas": CANVAS,
        "anchor": "bottom-center",
        "animations": {},
        "_comment": (
            "本目录下的 PNG 由 tools/gen_pet_skin.py 生成，"
            "skin.json 只声明帧间隔，实际帧数以目录内文件数为准（见 PetRenderer::loadSkinManifest）。"
        ),
    }

    total = 0
    for state, (count, interval, *_rest) in STATES.items():
        out_dir = os.path.join(OUT_ROOT, state)
        os.makedirs(out_dir, exist_ok=True)

        # 先清掉旧帧，避免改了帧数之后残留高编号文件，
        # 导致"少 1 帧"这种很难查的问题。
        for name in os.listdir(out_dir):
            if name.lower().endswith(".png"):
                os.remove(os.path.join(out_dir, name))

        for i in range(count):
            frame = render_frame(state, i, count)
            frame.save(os.path.join(out_dir, "%03d.png" % i), "PNG", optimize=True)
            total += 1

        manifest["animations"][state] = {
            "frameCount": count,
            "frameIntervalMs": interval,
            "loop": True,
        }
        print("  %-10s %2d frames @ %dms" % (state, count, interval))

    with open(os.path.join(OUT_ROOT, "skin.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)

    print("\n生成完成：%d 帧 -> %s" % (total, OUT_ROOT))


if __name__ == "__main__":
    main()
