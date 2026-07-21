#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
BMCU 固件批量编译脚本（高速版 · 适配本仓库 BMCU-C）

设计目标（与 build_all_firmwares_softload.sh 完全一致的输出，但快 1~2 个数量级）：
  - 输出目录结构 / 文件名 与 build_all_firmwares_softload.sh 完全相同
    （standard(A1) / high_force_load(P1S) / soft_load(A1)
     → AUTOLOAD|NO_AUTOLOAD → FILAMENT_RGB_ON|OFF → SOLO / AMS_A..D）
  - 精准计时（分阶段）与具体固件数量报告

加速原理（参考朋友的快速编译脚本）：
  1. pio run -e moj -v 提取工具链路径与编译/链接命令行（带缓存，二次运行跳过）
  2. 预编译所有不受变体宏影响的 .o（Framework SDK + 不变用户源）只编一次
  3. 按 "模式组合" (dm × rgb × p1s × soft_load × ams_num) 编译受宏影响的变体 .o 并缓存复用
  4. AMS_RETRACT_LEN 仅被 Motion_control 使用：用占位符 123.456f 编译基础固件，
     再在最终 .bin 中二进制修补该浮点为目标回抽长度（不重编 Motion_control.cpp）

源文件清单与链接顺序均从 pio verbose 动态推导，新增源文件（如 aht20/、hal/）自动纳入，无需手改。
"""
import os
import re
import sys
import time
import zlib
import hashlib
import struct
import shutil
import subprocess
import threading
from concurrent.futures import ThreadPoolExecutor

# ===== 切换到脚本所在目录 =====
os.chdir(os.path.dirname(os.path.abspath(__file__)))
GLOBAL_START = time.perf_counter()

# ===== 配置 =====
FAST_MODE = True          # True: 用二进制修补回抽长度（最快）；False: 每回抽长度重编 Motion_control
OUT_DIR = "firmwares"
PIO_ENV = "fw"
PARALLEL_DIR = ".pio_parallel"
CACHE_DIR = os.path.join(PARALLEL_DIR, "obj_cache")
VERBOSE_CACHE = os.path.join(PARALLEL_DIR, "verbose_cache.txt")

# 与 build_all_firmwares_softload.sh 完全一致
TXT_MODE = "which_to_choose_mode.txt"
TXT_AUTOLOAD = "which_to_choose_autoload.txt"
TXT_RGB = "which_to_choose_filament_rgb.txt"
TXT_SLOTS = "which_to_choose_slots.txt"
OUT_GUIDE = "README.md"

SOLO_RETRACT = "0.095f"
RETRACTS = [
    "0.10", "0.20", "0.25", "0.30", "0.35", "0.40",
    "0.45", "0.50", "0.55", "0.60", "0.65", "0.70",
    "0.75", "0.80", "0.85", "0.90",
]

# 变体宏（用于过滤 / 分类）
VARIANT_MACROS = [
    "BAMBU_BUS_AMS_NUM", "AMS_RETRACT_LEN",
    "BMCU_DM_TWO_MICROSWITCH", "BMCU_ONLINE_LED_FILAMENT_RGB",
    "DBMCU_P1S", "BMCU_SOFT_LOAD",
]
RETRACT_MACRO = "AMS_RETRACT_LEN"
PLACEHOLDER_FLOAT = 123.456          # 占位符，编译基础固件用
PLACEHOLDER_BYTES = struct.pack("<f", PLACEHOLDER_FLOAT)

MODE_A1_DIR = "standard(A1)"
MODE_P1S_DIR = "high_force_load(P1S)"
MODE_SOFT_DIR = "soft_load(A1)"

# 模式定义: 目录名 | DBMCU_P1S | BMCU_SOFT_LOAD
MODES = [
    (MODE_A1_DIR, 0, 0),
    (MODE_P1S_DIR, 1, 0),
    (MODE_SOFT_DIR, 0, 1),
]


def log(msg):
    print(msg, flush=True)


# ===== 工具链定位 =====
def find_pio_bin():
    pio = shutil.which("pio") or shutil.which("pio.exe")
    if pio:
        return pio
    home = os.path.expanduser("~")
    for cand in (os.path.join(home, ".platformio", "penv", "Scripts", "pio.exe"),
                 os.path.join(home, ".platformio", "penv", "bin", "pio")):
        if os.path.exists(cand):
            return cand
    log("ERROR: 找不到 pio，请先安装 PlatformIO 并加入 PATH")
    sys.exit(1)


PIO_BIN = find_pio_bin()

STARTUPINFO = None
if os.name == "nt":
    STARTUPINFO = subprocess.STARTUPINFO()
    STARTUPINFO.dwFlags |= subprocess.STARTF_USESHOWWINDOW


def run_cmd(args, check=True):
    r = subprocess.run(args, capture_output=True, startupinfo=STARTUPINFO)
    if check and r.returncode != 0:
        log(f"COMMAND FAILED: {' '.join(args[:5])} ...")
        log(r.stderr.decode(errors="replace")[:2000])
        return None
    return r


# ===== 校验必要文件 =====
for f in (TXT_MODE, TXT_AUTOLOAD, TXT_RGB, TXT_SLOTS):
    if not os.path.exists(f):
        log(f"ERROR: 缺少必要描述文件 {f}")
        sys.exit(1)


# ====================================================================
# 第一步：提取工具链编译 / 链接参数（带缓存）
# ====================================================================
log("=" * 60)
log("  第一步：提取工具链编译 / 链接参数")
log("=" * 60)
os.makedirs(PARALLEL_DIR, exist_ok=True)


def _pio_ini_mtime():
    try:
        return os.path.getmtime("platformio.ini")
    except OSError:
        return 0


cache_valid = False
if os.path.exists(VERBOSE_CACHE) and _pio_ini_mtime() < os.path.getmtime(VERBOSE_CACHE):
    cache_valid = True

t_extract = time.perf_counter()
if cache_valid:
    log("  使用缓存的工具链参数（跳过 pio build）")
    with open(VERBOSE_CACHE, "r", encoding="utf-8") as f:
        verbose_output = f.read()
else:
    log("  运行 pio verbose build 提取参数（仅一次）...")
    run_cmd([PIO_BIN, "run", "-e", "moj", "-t", "clean"], check=False)
    res = run_cmd([PIO_BIN, "run", "-e", "moj", "-v"])
    if res is None:
        log("ERROR: 无法执行 PlatformIO 编译")
        sys.exit(1)
    verbose_output = res.stdout.decode(errors="replace") + res.stderr.decode(errors="replace")
    with open(VERBOSE_CACHE, "w", encoding="utf-8") as f:
        f.write(verbose_output)
t_extract = time.perf_counter() - t_extract
log(f"  verbose 输出 {len(verbose_output)} 字符，提取耗时 {t_extract:.1f}s")

vo = verbose_output.replace("\\", "/")

# --- 扫描关键命令行 ---
cxx_line = cc_line = link_line = None
for line in vo.splitlines():
    line = line.strip()
    if not line:
        continue
    if cxx_line is None and "riscv" in line and "g++" in line and "-c" in line and ".o" in line and "ADC_DMA" in line:
        cxx_line = line
    if cc_line is None and "riscv" in line and "gcc" in line and "g++" not in line and "-c" in line and ".o" in line:
        cc_line = line
    if link_line is None and "firmware.elf" in line and "-o" in line and "-c" not in line:
        link_line = line

for name, val in (("C++ 编译", cxx_line), ("C 编译", cc_line), ("链接", link_line)):
    if val is None:
        log(f"ERROR: 无法从 pio verbose 输出中找到 {name} 命令")
        sys.exit(1)

# 工具链目录
home = os.path.expanduser("~")
toolchain_bin = os.path.join(home, ".platformio", "packages", "toolchain-riscv", "bin")
if not os.path.exists(toolchain_bin):
    m = re.search(r"(\S+)[/\\]riscv[^\s]*g\+\+", cxx_line)
    if m:
        toolchain_bin = m.group(1).replace("/", os.sep)
suffix = ".exe" if os.name == "nt" else ""
CXX = os.path.join(toolchain_bin, "riscv-wch-elf-g++" + suffix)
CC = os.path.join(toolchain_bin, "riscv-wch-elf-gcc" + suffix)
AR = os.path.join(toolchain_bin, "riscv-wch-elf-gcc-ar" + suffix)
OBJCOPY = os.path.join(toolchain_bin, "riscv-wch-elf-objcopy" + suffix)
for tool in (CXX, CC, AR, OBJCOPY):
    if not os.path.exists(tool):
        log(f"ERROR: 工具链文件不存在: {tool}")
        sys.exit(1)


def parse_compile_flags(line, src_marker):
    tokens = line.split()
    try:
        c_idx = tokens.index("-c")
    except ValueError:
        c_idx = 0
    src_idx = len(tokens) - 1
    for i in range(len(tokens) - 1, c_idx, -1):
        if src_marker in tokens[i]:
            src_idx = i
            break
    raw = tokens[c_idx:src_idx]
    # 剥离可能夹在 -c 与源文件之间的 "-o <路径>"（避免覆盖到原始输出路径）
    out = []
    skip = False
    for t in raw:
        if skip:
            skip = False
            continue
        if t == "-o":
            skip = True
            continue
        out.append(t)
    return out


def filter_variant_defines(flags):
    return [f for f in flags
            if not (f.startswith("-D") and any(m in f for m in VARIANT_MACROS))]


def fix_paths(flags):
    out = []
    for f in flags:
        if f.startswith("-I") and len(f) > 2:
            out.append("-I" + f[2:].replace("/", os.sep))
        elif os.sep == "\\" and "/" in f and (f.startswith("C:") or f.startswith("c:")):
            out.append(f.replace("/", os.sep))
        else:
            out.append(f)
    return out


raw_cxx = parse_compile_flags(cxx_line, "ADC_DMA")
raw_cc = parse_compile_flags(cc_line, "core_riscv")
CXX_BASE_FLAGS = fix_paths(filter_variant_defines(raw_cxx))
CC_BASE_FLAGS = fix_paths(filter_variant_defines(raw_cc))


# --- 提取链接参数 ---
link_tokens = link_line.split()
link_start = 0
for i, t in enumerate(link_tokens):
    if t.endswith("firmware.elf"):
        link_start = i + 1
        break

LINK_FLAGS = []
LINK_LD = None
obj_start = link_start
for i in range(link_start, len(link_tokens)):
    tok = link_tokens[i]
    if tok.endswith(".o"):
        obj_start = i
        break
    if tok == "-T":
        LINK_LD = link_tokens[i + 1].replace("/", os.sep) if i + 1 < len(link_tokens) else None
        continue
    if tok.endswith(".ld"):
        LINK_LD = tok.replace("/", os.sep)
        continue
    if tok.startswith("-"):
        if tok.startswith("-Wl,-Map"):
            continue
        LINK_FLAGS.append(tok)

if LINK_LD is None:
    LINK_LD = os.path.abspath(os.path.join(".pio", "build", "moj", "Link.ld"))
elif not os.path.isabs(LINK_LD):
    LINK_LD = os.path.abspath(LINK_LD)
# 链接脚本与环境无关；若提取到的路径不存在（如 moj 构建产物被清），回退到任意 .pio/build/*/Link.ld
if not os.path.exists(LINK_LD):
    cand = None
    for root, _, files in os.walk(os.path.join(".pio", "build")):
        if "Link.ld" in files:
            cand = os.path.join(root, "Link.ld")
            break
    LINK_LD = cand if cand else LINK_LD
if not os.path.exists(LINK_LD):
    log(f"ERROR: 链接脚本不存在: {LINK_LD}")
    sys.exit(1)

LINK_SUFFIX = []
in_suffix = False
for i in range(obj_start, len(link_tokens)):
    tok = link_tokens[i]
    if tok.endswith(".o"):
        continue
    if "-Wl,--start-group" in tok:
        in_suffix = True
    if in_suffix or tok.startswith("-L") or tok.startswith("-l") or tok.endswith(".a"):
        LINK_SUFFIX.append(tok.replace("/", os.sep) if (tok.endswith(".a") or tok.startswith("-L")) else tok)
    if "-Wl,--end-group" in tok:
        in_suffix = False

# --- 收集所有编译命令：obj_path -> (src, compiler, filtered_flags) ---
compile_map = {}          # link_obj_path -> (src, compiler, flags)
framework_objs = {}       # link_obj_path -> common precompiled .o (非用户源)
user_src_set = set()

for line in vo.splitlines():
    line = line.strip()
    if not line or "-c" not in line:
        continue
    if "riscv" not in line or ("g++" not in line and "gcc" not in line):
        continue
    tokens = line.split()
    obj_path = src_path = None
    for i, tok in enumerate(tokens):
        if tok == "-o" and i + 1 < len(tokens):
            obj_path = tokens[i + 1]
    # 源文件是最后一个带源码扩展名的 token（兼容 -o OUTPUT.o 在源文件之后的写法）
    for tok in reversed(tokens):
        if tok.endswith((".cpp", ".c", ".cc", ".cxx", ".s", ".S")):
            src_path = tok
            break
    if src_path is None:
        src_path = tokens[-1]
    if obj_path is None or src_path is None:
        continue
    obj_norm = obj_path.replace("\\", "/")
    src_norm = src_path.replace("\\", "/")
    # 稳健判断：源文件绝对路径位于仓库 src/ 目录下即为用户源（大小写不敏感）
    abs_src = os.path.abspath(src_norm)
    src_root_abs = os.path.abspath("src")
    is_user = abs_src.lower() == src_root_abs.lower() or abs_src.lower().startswith(src_root_abs.lower() + os.sep)
    compiler = CXX if "g++" in line else CC
    flags = fix_paths(filter_variant_defines(parse_compile_flags(line, os.path.basename(src_norm))))
    compile_map[obj_norm] = (src_norm, compiler, flags)
    if is_user:
        user_src_set.add(src_norm)

# 链接命令中的 .o 顺序（动态）
link_obj_order = [t.replace("\\", "/") for t in link_tokens[obj_start:]
                  if t.endswith(".o")]

# 框架 / SDK 源文件：全部预编译一次（含原被编进 .a 的源，统一走合并静态库）
for obj_path, (src, compiler, flags) in compile_map.items():
    if src not in user_src_set:
        framework_objs[obj_path] = None  # 占位，稍后填预编译路径


# ====================================================================
# 第二步：扫描源文件，按变体宏分类（含头文件递归）
# ====================================================================
log("=" * 60)
log("  第二步：扫描源文件，按变体宏分类")
log("=" * 60)

SRC_ROOT = os.path.abspath("src")


def scan_macros(src_file, seen=None):
    """返回该源文件（含其 #include 的头文件）引用到的变体宏集合"""
    if seen is None:
        seen = set()
    found = set()
    path = os.path.abspath(src_file)
    if path in seen:
        return found
    seen.add(path)
    try:
        with open(path, "r", encoding="utf-8", errors="ignore") as f:
            text = f.read()
    except OSError:
        return found
    for mac in VARIANT_MACROS:
        if re.search(r"(?<![\w])" + re.escape(mac) + r"(?![\w])", text):
            found.add(mac)
    # 递归本地 include
    for m in re.finditer(r'#\s*include\s*"([^"]+)"', text):
        inc = m.group(1)
        cand1 = os.path.join(os.path.dirname(path), inc)
        cand2 = os.path.join(SRC_ROOT, inc)
        for cand in (cand1, cand2):
            if os.path.exists(cand):
                found |= scan_macros(cand, seen)
                break
    return found


user_classification = {}   # src -> 'INVARIANT' | 'OTHER' | 'RETRACT'
for src in user_src_set:
    macros = scan_macros(src)
    if RETRACT_MACRO in macros:
        user_classification[src] = "RETRACT"
    elif any(m in macros for m in VARIANT_MACROS if m != RETRACT_MACRO):
        user_classification[src] = "OTHER"
    else:
        user_classification[src] = "INVARIANT"

n_inv = sum(1 for v in user_classification.values() if v == "INVARIANT")
n_oth = sum(1 for v in user_classification.values() if v == "OTHER")
n_ret = sum(1 for v in user_classification.values() if v == "RETRACT")
log(f"  用户源文件: 不变 {n_inv} | 模式相关 {n_oth} | 含回抽长度 {n_ret}")
log(f"  框架/SDK 源文件: {len(framework_objs)}")
log(f"  链接 .o 数量: {len(link_obj_order)}")


# ====================================================================
# 第三步：收集任务 & 创建输出目录（格式同 softload.sh）
# ====================================================================
log("=" * 60)
log("  第三步：收集任务 & 创建输出目录")
log("=" * 60)

tasks = []
for mode_dir, p1s, soft_load in MODES:
    for dm in (1, 0):
        dm_dir = "AUTOLOAD" if dm == 1 else "NO_AUTOLOAD"
        for rgb in (1, 0):
            rgb_dir = "FILAMENT_RGB_ON" if rgb == 1 else "FILAMENT_RGB_OFF"
            base = os.path.join(OUT_DIR, mode_dir, dm_dir, rgb_dir)
            tasks.append((os.path.join(base, "SOLO", f"solo_{SOLO_RETRACT}.bin"),
                          0, SOLO_RETRACT, dm, rgb, p1s, soft_load))
            for slot, ams_num in (("A", 0), ("B", 1), ("C", 2), ("D", 3)):
                for r in RETRACTS:
                    tasks.append((os.path.join(base, f"AMS_{slot}", f"ams_{slot.lower()}_{r}f.bin"),
                                  ams_num, f"{r}f", dm, rgb, p1s, soft_load))

total_tasks = len(tasks)
log(f"  共 {total_tasks} 个固件编译任务")

# 创建目录结构 + 复制选型指南（与 softload.sh 一致）
if os.path.exists(OUT_DIR):
    shutil.rmtree(OUT_DIR, ignore_errors=True)
os.makedirs(OUT_DIR, exist_ok=True)
shutil.copy(TXT_MODE, os.path.join(OUT_DIR, OUT_GUIDE))
for mode_dir, p1s, soft_load in MODES:
    mode_base = os.path.join(OUT_DIR, mode_dir)
    os.makedirs(mode_base, exist_ok=True)
    shutil.copy(TXT_AUTOLOAD, os.path.join(mode_base, OUT_GUIDE))
    for dm in (1, 0):
        dm_dir = "AUTOLOAD" if dm == 1 else "NO_AUTOLOAD"
        dm_base = os.path.join(mode_base, dm_dir)
        os.makedirs(dm_base, exist_ok=True)
        shutil.copy(TXT_RGB, os.path.join(dm_base, OUT_GUIDE))
        for rgb in (1, 0):
            rgb_dir = "FILAMENT_RGB_ON" if rgb == 1 else "FILAMENT_RGB_OFF"
            base = os.path.join(dm_base, rgb_dir)
            os.makedirs(base, exist_ok=True)
            shutil.copy(TXT_SLOTS, os.path.join(base, OUT_GUIDE))
            for slot in ("SOLO", "AMS_A", "AMS_B", "AMS_C", "AMS_D"):
                os.makedirs(os.path.join(base, slot), exist_ok=True)


# ====================================================================
# 第四步：并行预编译所有 .o
# ====================================================================
log("=" * 60)
log("  第四步：并行预编译所有 .o 文件")
log("=" * 60)

if os.path.exists(CACHE_DIR):
    shutil.rmtree(CACHE_DIR, ignore_errors=True)
os.makedirs(CACHE_DIR)
COMMON_OBJ_DIR = os.path.join(CACHE_DIR, "common")
os.makedirs(COMMON_OBJ_DIR, exist_ok=True)


def compile_one(compiler, flags, src, obj_path):
    obj_abs = os.path.abspath(obj_path)
    src_abs = os.path.abspath(src) if not os.path.isabs(src) else src
    os.makedirs(os.path.dirname(obj_abs), exist_ok=True)
    cmd = [compiler, "-o", obj_abs] + flags + [src_abs]
    r = subprocess.run(cmd, capture_output=True, startupinfo=STARTUPINFO)
    if r.returncode != 0:
        log(f"\n  COMPILE FAILED: {os.path.basename(src)}")
        log(r.stderr.decode(errors="replace")[:1000])
        return False
    return True


def variant_defines(dm, rgb, p1s, soft_load, ams_num, retract="0.095f"):
    return [
        f"-DBMCU_DM_TWO_MICROSWITCH={dm}",
        f"-DBMCU_ONLINE_LED_FILAMENT_RGB={rgb}",
        f"-DBMCU_P1S={p1s}",
        f"-DBMCU_SOFT_LOAD={soft_load}",
        f"-DBAMBU_BUS_AMS_NUM={ams_num}",
        f"-DAMS_RETRACT_LEN={retract}",
    ]


def vkey_of(dm, rgb, p1s, soft_load, ams_num):
    return f"dm{dm}_rgb{rgb}_p1s{p1s}_sl{soft_load}_ams{ams_num}"


# 预编译任务收集
compile_tasks = []   # (compiler, flags, src, obj_path, name)

# 框架 / SDK
for obj_path in list(framework_objs.keys()):
    src, compiler, flags = compile_map[obj_path]
    obj = os.path.join(COMMON_OBJ_DIR, "fw", os.path.basename(obj_path))
    os.makedirs(os.path.dirname(os.path.abspath(obj)), exist_ok=True)
    compile_tasks.append((compiler, flags, src, obj, f"fw:{os.path.basename(obj_path)}"))
    framework_objs[obj_path] = obj

# 用户：不变
invariant_user_map = {}   # link_obj_path -> common .o
for obj_path, (src, compiler, flags) in compile_map.items():
    if src in user_src_set and user_classification.get(src) == "INVARIANT":
        obj = os.path.join(COMMON_OBJ_DIR, "user", os.path.basename(obj_path))
        os.makedirs(os.path.dirname(os.path.abspath(obj)), exist_ok=True)
        compile_tasks.append((compiler, flags, src, obj, f"inv:{os.path.basename(obj_path)}"))
        invariant_user_map[obj_path] = obj

# 模式组合
mode_combos = set()
for (_, ams_num, _, dm, rgb, p1s, soft_load) in tasks:
    mode_combos.add((dm, rgb, p1s, soft_load, ams_num))

other_user_map = {}    # (obj_path, vkey) -> .o
retract_user_map = {}  # (obj_path, vkey) -> .o (占位符)

for combo in sorted(mode_combos):
    dm, rgb, p1s, soft_load, ams_num = combo
    vkey = vkey_of(dm, rgb, p1s, soft_load, ams_num)
    vdir = os.path.join(CACHE_DIR, "variant", vkey)
    os.makedirs(vdir, exist_ok=True)
    defs = variant_defines(dm, rgb, p1s, soft_load, ams_num)
    defs_ph = variant_defines(dm, rgb, p1s, soft_load, ams_num, retract=f"{PLACEHOLDER_FLOAT}f")
    for obj_path, (src, compiler, flags) in compile_map.items():
        if src not in user_src_set:
            continue
        grp = user_classification.get(src)
        if grp == "OTHER":
            obj = os.path.join(vdir, "other_" + os.path.basename(obj_path))
            compile_tasks.append((compiler, flags + defs, src, obj, f"oth:{vkey}/{os.path.basename(obj_path)}"))
            other_user_map[(obj_path, vkey)] = obj
        elif grp == "RETRACT":
            obj = os.path.join(vdir, "ret_" + os.path.basename(obj_path))
            compile_tasks.append((compiler, flags + defs_ph, src, obj, f"ret:{vkey}/{os.path.basename(obj_path)}"))
            retract_user_map[(obj_path, vkey)] = obj

log(f"  共 {len(compile_tasks)} 个编译任务")

compile_jobs = os.cpu_count() or 4
pre_done = 0
pre_lock = threading.Lock()
total_pre = len(compile_tasks)
t_pre = time.perf_counter()


def _do_compile(task):
    global pre_done
    compiler, flags, src, obj, name = task
    ok = compile_one(compiler, flags, src, obj)
    with pre_lock:
        pre_done += 1
        cur = pre_done
    sys.stdout.write(f"\r  [预编译] {cur}/{total_pre} | {cur*100//total_pre}% | 用时 {time.perf_counter()-t_pre:.1f}s")
    sys.stdout.flush()
    return name if not ok else None


with ThreadPoolExecutor(max_workers=compile_jobs) as ex:
    results = list(ex.map(_do_compile, compile_tasks))
print("")
failed_pre = [r for r in results if r]
if failed_pre:
    log(f"ERROR: {len(failed_pre)} 个源文件预编译失败")
    sys.exit(1)
t_pre = time.perf_counter() - t_pre
log(f"  预编译完成 {total_pre} 个 .o，耗时 {t_pre:.1f}s")

# 校验框架 .o 是否都已生成
missing_fw = [o for o in framework_objs.values() if not os.path.exists(o)]
if missing_fw:
    log(f"ERROR: {len(missing_fw)} 个框架 .o 未生成，例如: {missing_fw[0]}")
    sys.exit(1)

# 完全复刻 pio 的链接结构：
#  - 链接命令里显式列出的框架 .o 直接入链（保持原顺序）
#  - 其余框架 .o（外设等）打进 libFrameworkNoneOSVariant.a，与原 pio 同名
link_obj_set = set(link_obj_order)
explicit_fw_objs = {k: v for k, v in framework_objs.items() if k in link_obj_set}
lib_fw_objs = sorted((os.path.abspath(v) for k, v in framework_objs.items() if k not in link_obj_set),
                      key=lambda p: os.path.basename(p).lower())
LIB_VARIANT = os.path.join(COMMON_OBJ_DIR, "libFrameworkNoneOSVariant.a")
ar_cmd = [AR, "rc", LIB_VARIANT] + lib_fw_objs
r_ar = subprocess.run(ar_cmd, capture_output=True, startupinfo=STARTUPINFO)
if r_ar.returncode != 0:
    log("ERROR: 打包框架静态库失败")
    log(r_ar.stderr.decode(errors="replace")[:1000])
    sys.exit(1)
log(f"  框架静态库已生成: {LIB_VARIANT}（{len(lib_fw_objs)} 个 .o；显式链 {len(explicit_fw_objs)} 个）")


# ====================================================================
# 第五步：链接基础固件 + 二进制修补回抽长度
# ====================================================================
log("=" * 60)
log("  第五步：链接基础固件 + 修补回抽长度")
log("=" * 60)

# 构建每个 vkey 的链接 .o 列表（显式框架 .o 按原顺序入链，用户源按分类入链）
def build_link_objs(vkey):
    objs = []
    for obj_path in link_obj_order:
        src, _, _ = compile_map[obj_path]
        if src in user_src_set:
            grp = user_classification.get(src)
            if grp == "INVARIANT":
                objs.append(invariant_user_map[obj_path])
            elif grp == "OTHER":
                objs.append(other_user_map[(obj_path, vkey)])
            elif grp == "RETRACT":
                objs.append(retract_user_map[(obj_path, vkey)])
            else:
                log(f"ERROR: 未分类的链接对象 {obj_path}")
                sys.exit(1)
        else:
            objs.append(framework_objs[obj_path])  # 显式框架 .o
    return objs


total_jobs = os.cpu_count() or 4
max_links = min(4, total_jobs)
total_jobs = int(os.environ.get("MAX_JOBS", total_jobs))
max_links = int(os.environ.get("LINK_JOBS", max_links))
log(f"  并行线程 {total_jobs}，最大并发链接 {max_links}")
link_sem = threading.Semaphore(max_links)

failed_builds = []
done_count = 0
base_done_local = [0]
prog_lock = threading.Lock()
t_link = time.perf_counter()


def run_base_link(combo):
    global done_count
    dm, rgb, p1s, soft_load, ams_num = combo
    vkey = vkey_of(dm, rgb, p1s, soft_load, ams_num)
    link_objs = build_link_objs(vkey)
    tmp_dir = os.path.join(CACHE_DIR, "tmp_base", vkey)
    os.makedirs(tmp_dir, exist_ok=True)
    elf_path = os.path.join(tmp_dir, "firmware.elf")
    link_cmd = [CXX, "-o", os.path.abspath(elf_path), "-T", LINK_LD] + LINK_FLAGS \
        + [os.path.abspath(o) for o in link_objs]
    link_cmd += [f"-L{COMMON_OBJ_DIR}"]
    for tok in LINK_SUFFIX:
        if tok.endswith(".a"):
            # 用复刻的 libFrameworkNoneOSVariant.a 替换原框架 .a
            link_cmd.append(os.path.abspath(LIB_VARIANT))
        else:
            link_cmd.append(tok)
    final = link_cmd
    link_env = os.environ.copy()
    link_env["TMP"] = os.path.abspath(tmp_dir)
    link_env["TEMP"] = os.path.abspath(tmp_dir)
    link_env["TMPDIR"] = os.path.abspath(tmp_dir)
    with link_sem:
        r = subprocess.run(final, capture_output=True, startupinfo=STARTUPINFO, env=link_env)
    if r.returncode != 0:
        log(f"\n  BASE LINK FAILED {vkey}:\n{r.stderr.decode(errors='replace')[:1500]}")
        raise subprocess.CalledProcessError(r.returncode, final)
    bin_path = os.path.join(tmp_dir, "firmware.bin")
    with link_sem:
        subprocess.run([OBJCOPY, "-O", "binary", elf_path, bin_path],
                       capture_output=True, startupinfo=STARTUPINFO, check=True)
    with open(bin_path, "rb") as f:
        data = f.read()
    with prog_lock:
        base_done_local[0] += 1
        cur = base_done_local[0]
    sys.stdout.write(f"\r  [基础链接] {cur}/{len(mode_combos)} | {cur*100//len(mode_combos)}% | 用时 {time.perf_counter()-t_link:.1f}s")
    sys.stdout.flush()
    return vkey, data


base_bins = {}
link_errors = []


def _base_worker(combo):
    try:
        return run_base_link(combo)
    except Exception as e:  # noqa
        link_errors.append(str(e))
        return None


with ThreadPoolExecutor(max_workers=total_jobs) as ex:
    for res in ex.map(_base_worker, sorted(mode_combos)):
        if res:
            base_bins[res[0]] = res[1]

print("")
if link_errors:
    log(f"ERROR: 基础链接失败 {len(link_errors)} 个")
    sys.exit(1)

# 校验占位符出现次数（必须 > 0，否则无法用修补法）
base_sentinel_counts = {vk: data.count(PLACEHOLDER_BYTES) for vk, data in base_bins.items()}
for vk, c in base_sentinel_counts.items():
    if c == 0:
        log(f"ERROR: 基础固件 {vk} 中未找到占位符浮点，无法用二进制修补（请设 FAST_MODE=False）")
        sys.exit(1)
log(f"  占位符浮点出现次数（每模式组合）: {base_sentinel_counts}")


def patch_and_write(task_item):
    global done_count
    out_path, ams_num, retract_len, dm, rgb, p1s, soft_load = task_item
    vkey = vkey_of(dm, rgb, p1s, soft_load, ams_num)
    base = bytearray(base_bins[vkey])
    target = struct.pack("<f", float(retract_len.rstrip("f")))
    expected = base_sentinel_counts[vkey]
    pos = 0
    replaced = 0
    while True:
        pos = base.find(PLACEHOLDER_BYTES, pos)
        if pos == -1:
            break
        base[pos:pos + 4] = target
        replaced += 1
        pos += 4
    if replaced != expected:
        with prog_lock:
            failed_builds.append(out_path)
            done_count += 1
        return
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "wb") as f:
        f.write(base)
    with prog_lock:
        done_count += 1
        cur = done_count
    elapsed = time.perf_counter() - t_link
    pct = cur * 100 // total_tasks
    em, es = divmod(int(elapsed), 60)
    sys.stdout.write(f"\r  [修补写盘] {cur}/{total_tasks} | {pct}% | 已用 {em}分{es}秒")
    sys.stdout.flush()


with ThreadPoolExecutor(max_workers=total_jobs) as ex:
    list(ex.map(patch_and_write, tasks))
print("")
t_link = time.perf_counter() - t_link
log(f"  链接/修补完成，耗时 {t_link:.1f}s")

if failed_builds:
    log(f"WARNING: {len(failed_builds)} 个固件生成失败：")
    for fb in failed_builds[:10]:
        log(f"  - {fb}")
else:
    log("所有固件生成成功！")


# ====================================================================
# 第六步：生成 manifest.txt
# ====================================================================
log("正在生成 manifest.txt ...")
manifest_path = os.path.join(OUT_DIR, "manifest.txt")
entries = []
for dirpath, _, filenames in os.walk(OUT_DIR):
    for fn in filenames:
        p = os.path.join(dirpath, fn)
        rel = os.path.relpath(p, OUT_DIR).replace(os.sep, "/")
        crc, size = 0, 0
        h = hashlib.sha256()
        with open(p, "rb") as f:
            while True:
                b = f.read(1024 * 1024)
                if not b:
                    break
                size += len(b)
                crc = zlib.crc32(b, crc)
                h.update(b)
        entries.append((rel, h.hexdigest(), f"{crc & 0xffffffff:08X}", size))
entries.sort(key=lambda x: x[0])
with open(manifest_path, "w", encoding="utf-8") as out:
    out.write("# format: SHA256_HEX CRC32_HEX SIZE_BYTES REL_PATH\n")
    for rel, sha, crc, size in entries:
        out.write(f"{sha} {crc} {size} {rel}\n")


# ====================================================================
# 清理 & 汇总
# ====================================================================
if os.path.exists(CACHE_DIR):
    shutil.rmtree(CACHE_DIR, ignore_errors=True)

total_time = time.perf_counter() - GLOBAL_START
tm, ts = divmod(int(total_time), 60)
te, tse = divmod(int(t_extract), 60)
tp, tsp = divmod(int(t_pre), 60)
tl, tls = divmod(int(t_link), 60)
log("=" * 60)
log("  编译完成 · 汇总")
log("=" * 60)
log(f"  固件总数 : {total_tasks}")
log(f"  成功     : {total_tasks - len(failed_builds)}")
log(f"  失败     : {len(failed_builds)}")
log(f"  输出目录 : {OUT_DIR}/")
log(f"  Manifest : {manifest_path}")
log("-" * 60)
log(f"  提取参数 : {te}分{tse}秒")
log(f"  预编译.o : {tp}分{tsp}秒  ({total_pre} 个)")
log(f"  链接/修补: {tl}分{tls}秒")
log(f"  总耗时   : {tm}分{ts}秒")
log("=" * 60)
