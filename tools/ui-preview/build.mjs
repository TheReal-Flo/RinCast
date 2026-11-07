// Builds and runs the headset UI preview: the app's real screens, widgets and renderer, compiled
// for Windows and drawn through ANGLE, so a change can be looked at in seconds.
//   node tools/ui-preview/build.mjs [out dir] [scene]
// Needs Visual Studio's C++ tools, the Android NDK (for the GLES headers) and ANGLE's libEGL.dll
// and libGLESv2.dll: every Electron app has them. Point ANGLE_DIR at that app's folder.
import { execFileSync, execSync } from "node:child_process";
import { copyFileSync, cpSync, existsSync, mkdirSync, readdirSync, readFileSync, writeFileSync } from "node:fs";
import { dirname, join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

const repo = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const b = join(repo, "build/ui-preview");
const cpp = join(repo, "headset/app/src/main/cpp");
const out = resolve(process.argv[2] || join(b, "shots"));
const scene = process.argv[3] || "";

const angle = process.env.ANGLE_DIR;
if (!angle || !existsSync(join(angle, "libGLESv2.dll"))) throw new Error("Set ANGLE_DIR to a folder with libEGL.dll and libGLESv2.dll");
const sdk = process.env.ANDROID_HOME || join(process.env.LOCALAPPDATA, "Android/Sdk");
const ndk = join(sdk, "ndk", readdirSync(join(sdk, "ndk")).sort().at(-1));
const sysroot = join(ndk, "toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/include");

const inc = join(b, "include");
for (const d of ["GLES2", "GLES3", "KHR", "EGL"]) cpSync(join(sysroot, d), join(inc, d), { recursive: true });
mkdirSync(join(inc, "android"), { recursive: true });
writeFileSync(join(inc, "android/asset_manager.h"), "#pragma once\nstruct AAssetManager;\n");

// screens.cpp includes "app.h" and friends next to itself, so it's compiled from a copy with the
// preview's versions beside it.
const gen = join(b, "gen");
for (const d of ["app", "gfx", "platform"]) mkdirSync(join(gen, d), { recursive: true });
copyFileSync(join(cpp, "app/screens.cpp"), join(gen, "app/screens.cpp"));
// ANGLE on Direct3D 11 stops at ES 3.0, which is all the UI shaders need.
const glSource = readFileSync(join(cpp, "gfx/gl.cpp"), "utf8").replace("#version 310 es", "#version 300 es").replace('#include "gl.h"', `#include "gfx/gl.h"`);
writeFileSync(join(gen, "gfx/gl.cpp"), glSource);
copyFileSync(join(repo, "tools/ui-preview/preview_app.h"), join(gen, "app/app.h"));
writeFileSync(join(gen, "gfx/sky.h"), `#include "${join(cpp, "gfx/sky.h").replaceAll(sep, "/")}"\n`);
writeFileSync(join(gen, "platform/jni_bridge.h"), `#pragma once
#include <string>
namespace rv { class Platform { public:
  static std::string DeviceModel() { return "Quest 3"; }
  static bool HasMicPermission() { return true; }
  struct UsbNetwork {
    enum State { Off, Waiting, Connected, NotAllowed } state = Off;
    unsigned long long handle = 0;
    std::string interface;
    int kbps = 0;
  };
}; }
`);

const vcvars = execSync(`"${process.env["ProgramFiles(x86)"]}\\Microsoft Visual Studio\\Installer\\vswhere.exe" -latest -products * -property installationPath`).toString().trim() + "\\VC\\Auxiliary\\Build\\vcvars64.bat";
mkdirSync(b, { recursive: true });
for (const f of ["libEGL.dll", "libGLESv2.dll", "d3dcompiler_47.dll"]) copyFileSync(join(angle, f), join(b, f));

const q = (s) => `"${s}"`;
const lines = ["@echo off", `call ${q(vcvars)} >nul`, `cd /d ${q(b)}`];
for (const dll of ["libEGL", "libGLESv2"]) {
  if (!existsSync(join(b, dll + ".lib"))) {
    lines.push(`dumpbin /nologo /exports ${dll}.dll > ${dll}.exports`);
    lines.push(`node -e "const s=require('fs').readFileSync('${dll}.exports','utf8');const n=[...s.matchAll(/^\\s+\\d+\\s+[0-9A-F]+\\s+[0-9A-F]+\\s+(\\S+)/gm)].map(m=>m[1]);require('fs').writeFileSync('${dll}.def','EXPORTS\\n'+n.join('\\n'))"`);
    lines.push(`lib /nologo /def:${dll}.def /machine:x64 /out:${dll}.lib >nul`);
  }
}
const includes = [join(gen, "app"), gen, cpp, join(repo, "shared/include"), join(repo, "third_party"), inc].map((i) => `/I${q(i)}`).join(" ");
const sources = [join(repo, "tools/ui-preview/main.cpp"), join(gen, "app/screens.cpp"), join(cpp, "ui/ui.cpp"), join(cpp, "ui/window.cpp"), join(cpp, "gfx/ui_renderer.cpp"), join(gen, "gfx/gl.cpp"), join(cpp, "stb_impl.cpp")].map(q).join(" ");
// The headset app gets its version from VERSION through CMake; the preview needs it too.
const version = readFileSync(join(repo, "VERSION"), "utf8").trim();
lines.push(`cl /nologo /std:c++17 /EHsc /O1 /utf-8 /W0 /DNOMINMAX /DGL_GLEXT_PROTOTYPES /DRINCAST_VERSION_STRING=\\"${version}\\" /Fo:obj\\ ${includes} ${sources} /Fe:ui-preview.exe /link libEGL.lib libGLESv2.lib`);
mkdirSync(join(b, "obj"), { recursive: true });
writeFileSync(join(b, "build.cmd"), lines.join("\r\n") + "\r\n");
try {
  console.log(execFileSync("cmd", ["/c", join(b, "build.cmd")], { encoding: "utf8", stdio: ["ignore", "pipe", "pipe"] }).split("\n").filter((l) => /error|unresolved/i.test(l)).join("\n"));
} catch (e) {
  console.log(String(e.stdout).split("\n").filter((l) => /error|unresolved|fatal/i.test(l)).slice(0, 40).join("\n"));
  process.exit(1);
}
console.log(execFileSync(join(b, "ui-preview.exe"), [join(repo, "headset/app/src/main/assets"), out, ...(scene ? [scene] : [])], { encoding: "utf8", cwd: b, stdio: ["ignore", "pipe", "inherit"] }));
