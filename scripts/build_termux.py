#!/usr/bin/env python3
"""Build without deploying. Keeps original build/ artifacts; writes build/optimized/.

Requires Termux clang/clang++, Java 17, sibling vtouch-editor/sdk jars, ImGui sources,
and EGL/GLES headers (GL_HEADERS). Never installs dependencies or runs the core.
"""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent
OUT = Path(os.environ.get("VTOUCH_BUILD_OUT", str(ROOT / "build/optimized"))).resolve()
EDITOR = ROOT.parent / "vtouch-editor"
JAVA = Path("/data/data/com.termux/files/usr/lib/jvm/java-17-openjdk/bin")
GL = Path(os.environ.get("GL_HEADERS", "/data/data/com.termux/files/usr/include"))


def run(args, **kwargs):
    print(" ".join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), cwd=ROOT, check=True, **kwargs)


def link(args):
    # Termux's driver adds an app-private RUNPATH. It must not leak into a root/app_process payload.
    plan = run([args[0], "-###", *args[1:]], capture_output=True, text=True)
    for line in reversed(plan.stderr.splitlines()):
        words = shlex.split(line)
        if words and Path(words[0]).name == "ld.lld":
            words = [w for w in words if not w.startswith("-rpath=")]
            run(words)
            return
    raise RuntimeError("cannot identify linker invocation")


def main():
    for path in [GL / "EGL/egl.h", GL / "GLES2/gl2.h", ROOT / "thirdparty/imgui/imgui.h",
                 EDITOR / "sdk/android.jar", EDITOR / "sdk/d8.jar", JAVA / "javac"]:
        if not path.is_file():
            raise SystemExit("Missing build dependency: " + str(path))
    OUT.mkdir(parents=True, exist_ok=True)
    obj = OUT / "obj"
    obj.mkdir(exist_ok=True)
    classes = Path(tempfile.mkdtemp(prefix="classes-", dir=OUT))
    run([JAVA / "javac", "--release", "8", "-encoding", "UTF-8", "-cp", EDITOR / "sdk/android.jar",
         "-d", classes, ROOT / "src-ui/VTouchUI.java"])
    # Fresh classes directory prevents stale anonymous/inner classes from entering the dex.
    run([JAVA / "java", "-cp", EDITOR / "sdk/d8.jar", "com.android.tools.r8.D8", "--release",
         "--lib", EDITOR / "sdk/android.jar", "--min-api", "24", "--output", OUT,
         *sorted(classes.glob("*.class"))])
    run(["python3", "scripts/gen_ui_chars.py", "--out", OUT / "ui_chars.h"])
    cflags = ["-O2", "-Wall", "-Wextra", "-Werror", "-D_GNU_SOURCE"]
    core_objects, default_objects = [], []
    for src in sorted((ROOT / "src").glob("*.c")):
        dst = obj / ("core-" + src.stem + ".o")
        run(["clang", *cflags, "-DVT_UI", "-c", src, "-o", dst])
        core_objects.append(dst)
        dst = obj / ("default-" + src.stem + ".o")
        run(["clang", *cflags, "-c", src, "-o", dst])
        default_objects.append(dst)
    panel = []
    for name in ["vt_util", "vt_shm", "vt_expr"]:
        dst = obj / ("panel-" + name + ".o")
        run(["clang", "-O2", "-Wall", "-fPIC", "-D_GNU_SOURCE", "-DVT_UI", "-DVT_UI_PANEL",
             "-Isrc", "-c", "src/" + name + ".c", "-o", dst])
        panel.append(dst)
    dst = obj / "panel-ui_glue.o"
    run(["clang", "--target=aarch64-linux-android26", "-O2", "-Wall", "-fPIC", "-D_GNU_SOURCE",
         "-DVT_UI", "-DVT_UI_PANEL", "-Isrc", "-Isrc-ui", "-c", "src-ui/ui_glue.c", "-o", dst])
    panel.append(dst)
    for name in ["imgui", "imgui_draw", "imgui_tables", "imgui_widgets", "backends/imgui_impl_opengl3"]:
        dst = obj / ("panel-" + Path(name).name + ".o")
        run(["clang++", "-O2", "-Wall", "-fPIC", "-DIMGUI_IMPL_OPENGL_ES2", "-I" + str(GL),
             "-Ithirdparty/imgui", "-c", "thirdparty/imgui/" + name + ".cpp", "-o", dst])
        panel.append(dst)
    dst = obj / "panel-vtouch_ui.o"
    run(["clang++", "-O2", "-Wall", "-fPIC", "-DEGL_NO_PLATFORM_SPECIFIC_TYPES", "-I" + str(GL),
         "-Ithirdparty/imgui", "-Isrc", "-Isrc-ui", "-I" + str(OUT), "-c", "src-ui/vtouch_ui.cpp", "-o", dst])
    panel.append(dst)
    link(["clang++", "-shared", "-Wl,--no-undefined", "-o", OUT / "libtestimgui.so", *panel,
          "/system/lib64/libEGL.so", "/system/lib64/libGLESv2.so", "/system/lib64/libandroid.so",
          "/system/lib64/liblog.so", "-lm"])
    run(["llvm-strip", "--strip-unneeded", OUT / "libtestimgui.so"])
    shutil.copy2("/data/data/com.termux/files/usr/lib/libc++_shared.so", OUT / "libc++_shared.so")
    embed = OUT / "embed"
    embed.mkdir(exist_ok=True)
    embed_objects = []
    for src, name in [("classes.dex", "classes.dex"), ("libtestimgui.so", "libtestimgui.so"),
                      ("libc++_shared.so", "libcxx_shared.so")]:
        shutil.copy2(OUT / src, embed / name)
        subprocess.run(["llvm-objcopy", "-I", "binary", "-O", "elf64-littleaarch64", "-B", "aarch64",
                        "--rename-section", ".data=.rodata,alloc,load,readonly,data,contents", name, name + ".o"],
                       cwd=embed, check=True)
        embed_objects.append(embed / (name + ".o"))
    link(["clang", "-o", OUT / "vtouchd_ui", *core_objects, *embed_objects, "-lm"])
    link(["clang", "-o", OUT / "vtouchd", *default_objects, "-lm"])
    run(["llvm-strip", "--strip-unneeded", OUT / "vtouchd_ui"])
    run(["python3", "scripts/pack_su.py", OUT / "vtouchd_ui", OUT / "vtouch.sh"])
    lines = []
    for name in ["vtouchd_ui", "vtouchd", "vtouch.sh", "classes.dex", "libtestimgui.so", "libc++_shared.so"]:
        p = OUT / name
        line = hashlib.sha256(p.read_bytes()).hexdigest() + "  " + name
        lines.append(line)
        print(line)
    (OUT / "SHA256SUMS").write_text("\n".join(lines) + "\n")
    print("Build complete (not deployed):", OUT)


if __name__ == "__main__":
    main()
