# Third-party notices

ukagaka-3d-shell itself is released under the MIT License (see `LICENSE`). It contains or links the
third-party components below; their licenses apply to those parts and are not replaced by the MIT
License of this project. The source is fetched at configure time (CMake FetchContent, pinned versions);
no third-party source code is vendored in the repository.

| Component | Version | License | Where it ends up |
|---|---|---|---|
| libmmd (incl. portions derived from MikuMikuDayo) | v0.3.2, f478f985ca506fa9d265b6ba8f2359009b087548 | MIT | `ukagaka3d.exe` (statically linked) |
| Bullet Physics (BulletDynamics, BulletCollision, LinearMath only) | 3.25 | zlib | `ukagaka3d.exe` (statically linked) |
| nlohmann/json | 3.11.3 | MIT | `ukagaka3d.exe`, `ukagaka3d_saori.dll` (header-only) |
| stb_image | stb 4e9b368bfc071e97206ea5c6adba68dd0cbf8897 | MIT or Public Domain (MIT chosen here) | `ukagaka3d.exe` |
| stb_image_resize2, stb_image_write | same stb commit | MIT or Public Domain | build tools only (not distributed) |
| Microsoft C/C++ runtime | MSVC 14.51 (static, `/MT`) | Visual Studio license terms (Distributable Code) | `ukagaka3d.exe`, `ukagaka3d_saori.dll` |
| YAYA (SHIORI) | 6.x `yaya.dll` | BSD 3-Clause (+ AYA original notice) | sample ghost only; license files are placed next to `yaya.dll` |
| CG-CA Uka (MMD model) | 659f0d1a740fec68bb2817dc9c694a35e7a15f12 | CC BY 4.0 + usage guidelines | sample ghost only (see below) |

Direct3D 11, DXGI, DirectComposition, DWM, WIC and DirectXMath come from the Windows SDK / the operating
system and are not redistributed.

## libmmd

https://github.com/haturatu/libmmd — portions of the PMX, VMD, animation and physics implementations are
derived from MikuMikuDayo (Copyright (c) 2024, 2025 pennennennennennenem, MIT). `src/mmd/Skinning.cpp`
reimplements libmmd's SDEF / QDEF skinning formulas and quaternion helpers so that both skinning paths
produce the same result; that file is covered by this notice as well.

```
MIT License

Copyright (c) 2024, 2025 pennennennennennenem
Copyright (c) 2026 haturatu

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## Bullet Physics

https://github.com/bulletphysics/bullet3 — release tarball 3.25 (SHA-256
c45afb6399e3f68036ddb641c6bf6f552bf332d5ab6be62f7e6c54eda05ceb77). Files under `Extras` and
`examples/ThirdPartyLibs` have other licenses; ukagaka3d builds none of them.

```
The files in this repository are licensed under the zlib license, except for the files under 'Extras' and examples/ThirdPartyLibs.

Bullet Continuous Collision Detection and Physics Library
http://bulletphysics.org

This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from the use of this software.
Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it freely,
subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not claim that you wrote the original software. If you use this software in a product, an acknowledgment in the product documentation would be appreciated but is not required.
2. Altered source versions must be plainly marked as such, and must not be misrepresented as being the original software.
3. This notice may not be removed or altered from any source distribution.
```

## nlohmann/json

https://github.com/nlohmann/json — release json.tar.xz 3.11.3 (SHA-256
d6c65aca6b1ed68e7a182f4757257b107ae403032760ed6ef121c9d55e81757d).

```
MIT License 

Copyright (c) 2013-2022 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## stb

https://github.com/nothings/stb — available under the MIT License or the Unlicense; ukagaka3d uses it under
the MIT License.

```
ALTERNATIVE A - MIT License
Copyright (c) 2017 Sean Barrett
Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.
THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## YAYA (sample ghost only)

https://github.com/YAYA-shiori/yaya-shiori — `yaya.dll` is not part of this repository. The packaging
script copies it together with its `LICENSE` (below) and `readme-original.txt` (the original AYA notice by
umeici) into the assembled ghost.

```
BSD 3-Clause License

Copyright (c) 2007-, 整備班(SEIBIHAN)
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice, this
  list of conditions and the following disclaimer.

* Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

* Neither the name of the copyright holder nor the names of its
  contributors may be used to endorse or promote products derived from
  this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## CG-CA Uka (sample ghost only)

CG-CA Uka (c) 2023-2024 by Nagoya Institute of Technology, Moonshot R&D Goal 1 Avatar Symbiotic Society

- Source: https://github.com/mmdagent-ex/uka (commit 659f0d1a740fec68bb2817dc9c694a35e7a15f12),
  downloaded and checked against a SHA-256 by `scripts/package-ghost.ps1`; never committed to this repository.
- License: Creative Commons Attribution 4.0 International, https://creativecommons.org/licenses/by/4.0/
- Usage guidelines: the "利用ガイドライン" section of the official README (shipped with the model as
  `README.ja.md` / `README.md`). The guidelines may be updated by the authors.
- Changes made for the sample ghost: the eight 4096×4096 textures are downscaled to 2048×2048 by default
  (`uka3d_texresize`, sRGB-aware, alpha-weighted filtering); `iraira.png`, `MS_Uka.pmx` and the READMEs are
  unchanged; the unused `tex/hair.png`, the PMD files and the sample motions are not included. The
  assembled model folder records this in `CREDITS.ja.md`.
- `docs/images/screenshot-uka.png` is a screenshot of the sample ghost and shows a rendering of Uka (CC BY 4.0, credited
  under the image in the README). It is the only Uka-derived file in the repository.
- ukagaka-3d-shell is an unofficial project and is not endorsed by the authors of Uka.

## Motions in the sample ghost

All VMD files in the sample ghost are produced by this project's own code; no third-party motion data is
used. `idle`, `walk`, `sit` and `grab` are generated for the packaged model by `uka3d_motiongen`
(`src/motion/MotionGenerator.cpp`) from measurements of the model (leg lengths, feet, arm angle);
`greet` and `nod` are written from hand-coded keyframes in `tools/make_test_assets.cpp`.

## Reference only

Uka3D (by yasi) is a prior SAORI implementation for SSP. It has no stated license, so ukagaka3d uses none
of its code or binaries; only its publicly documented behaviour was consulted for orientation.
