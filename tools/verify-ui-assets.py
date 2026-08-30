#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Verify every committed and generated Sapote Redwood asset."""

from hashlib import sha256
from pathlib import Path


PINNED = {
    "assets/sapote-logo.png": "f7d932cfb5b2fcc7ec9a33291326217cc17e2e36c604a880083ba7bb459fa912",
    "assets/sapote-logo-source.png": "90f1c5613af4eaa817bbf69b151fc2e387ba45873643cc7c220bb471423c6663",
    "build/logo.srl": "cae1b3979144f528e5245697474397b881c9ed234838821d5ea4625065562b5e",
    "build/wallpaper.spw": "1154a4b9a8feccdd4a422e07e9794c4b610a2bf467ef9a6fa9985d1d4707ec1d",
    "assets/sapstudio-icon.png": "466ab943da6ca035c88988d8bf819625e46ca85a5fe11a2a39b7f34f2e3e1df7",
    "build/sapstudio-icon.srl": "0e30ba0bfd43ee19ecbef903ae2bf05cab4c0f1e9e28e0c9233907ff6f3bcddd",
    "assets/settings-icon-dock.png": "29bbd3bf688a4eb7ae7c67ef8b9bdbe7c8d101f6694485c585d74e6cd36bbc7f",
    "build/settings-icon.srl": "646fcc71ffb1e621d6a2e828405a1dfcea9930aff50062755fb34698d2c813ba",
    "assets/settings-icon.png": "858e9fc54d2760e7fd486c992f9660831ceb8258b814e5d600e7145f679d9f6b",
    "assets/files-icon-dock.png": "445211ce12458a02dcd3c6f9be113a07dcb8a91fabc9a07ec3f41fd0e4bf7cc6",
    "build/files-icon.srl": "878dc489855709c5874d431d37ab18f01a45d765de4da94ad5cd19343e00003f",
    "assets/files-icon.png": "d07b23abebd9cb95b72a81b11af658ef746ee17105865aa41654a62f1615b044",
    "assets/terminal-icon-dock.png": "380e645c469b454f5c792f75c5d462c6404e333042c19f8edc0088d1cbc2345e",
    "build/terminal-icon.srl": "68b93675d08a36fae73349fba3a172b2d54cd5ea6a4d9437d14d5aa0fa92388b",
    "assets/terminal-icon.png": "bc333d40ad7a59b620ffb37c136a07ea543ad80ab43eefd3562fa236b0310a34",
    "assets/camera-icon-dock.png": "318c029c4a2c57058a1c4a8a33614420a938a1b59ba7b0c77fc30628b76e00a2",
    "build/camera-icon.srl": "8eb00e5a86e92fb47211123d019df6b03fbc2126065451402abc77308f27c125",
    "assets/camera-icon.png": "378b38b0a96760d71af37069d89c5aaaa045f85e30be9311f64640a17742daea",
    "assets/canvas-icon.png": "b750399f8472ad8e1a2fa04c93732bd773efb63cff6c26c660e9872caf077972",
    "assets/canvas-icon-dock.png": "7456bf5390acb86f2f702e2922a01f2206be3e67ad86805566e2128d0d2020a2",
    "build/canvas-icon.srl": "1fa12b05141928d772e544a5455a83915d9588b7f5646d14760cf9babc428f93",
    "assets/settings-category-icons.png": "48aa43fa5ce7051d3f138d293feeff326d3544e4b56fba2bd63454cfada8d441",
    "build/settings-category-icons.srl": "1a47dca6e6a1ea81ce9f838391f6877bbec7107ee2027a1fffde3e427837f08b",
    "assets/icons/lucide/LICENSE": "b495047bd93a9b06913511076f504daba17d5bbeb3e0650f3bb53a4220329c57",
    "assets/icons/lucide/circle.svg": "3a991bd47beaf9874fba6fdf87bbba442970a89b5e6aa391558bc8b0a00a0513",
    "assets/icons/lucide/clock-3.svg": "0e097d0b66278965fb53bc14900575d95f5c21b9cca932028bbb33294b56c9e2",
    "assets/icons/lucide/eraser.svg": "838d4355dd49340523c38a22a2d23f1aca72ef42f8eba79f6a0c84bf6e27b929",
    "assets/icons/lucide/hard-drive.svg": "324363c40f54e4ca843de0ad1d023b063b70a26a83bb2e0a9e9f4aad9af37378",
    "assets/icons/lucide/image.svg": "619c9db58c879dce6250376ba67d9c9102ca6dbb3b1efb9294c9f180e22273a5",
    "assets/icons/lucide/info.svg": "bc977a64eb96f3e9c1041ffd09a3fceb70e3e65c02b571f76064062ed31f3cb9",
    "assets/icons/lucide/keyboard.svg": "f509925ca81964ed06251ad4702e989bb81a566ffc74d15c2226b4f07ded67be",
    "assets/icons/lucide/monitor.svg": "d1a443233345724859de8e4fb48968ba91e885e12ddbd710701548bd91b39428",
    "assets/icons/lucide/mouse-pointer-2.svg": "a87128b1d33024c22ab598d1ce313081ce8942efd1a70274215bcc53421b7c7e",
    "assets/icons/lucide/network.svg": "626be730feea1e5ea7539fbf78b4a3fcb927ce5d7dc43364f2216908672ac792",
    "assets/icons/lucide/paintbrush.svg": "b0a958ba9889f89c40df6e1c168b8a7aa20f6309780e795c5bce00a4bc11bff6",
    "assets/icons/lucide/palette.svg": "8e7d3fde990cffd10469ba351f27c50202b057a9bb9af5626322e53fd3abc78c",
    "assets/icons/lucide/panel-bottom.svg": "7792f8b39919a347ba7be86fda059c41a46e38dbe23758f7be6aa4514579703e",
    "assets/icons/lucide/rocket.svg": "2b7887d9a5623f2e5e4a5d32716f93829d61b458e03b6eae33a6df648b9eb855",
    "assets/icons/lucide/search.svg": "283d371c2e433817bb9c0c8310caa6c77fa4177c0f4f1168d9c83b97af7389dc",
    "assets/icons/lucide/slash.svg": "2e8b636164f057c3113b04d36a5bb958f78aff454c4a644642da790ae2addbc8",
    "assets/icons/lucide/square.svg": "bd979354f0ab184b95cecf03eedefe40c2dc65830ac6d7e60017b2b25a354acb",
    "assets/icons/lucide/volume-2.svg": "fb404f9c128a0579de67b399177631e5edb3502fdc247ceb30e8b15754b46071",
    "assets/canvas-tools.a8": "b19a01ed2147bb782c684691607e174643e720fa0b0711545e0fdfe52cbaafd0",
    "assets/fonts/inter-ui-atlas.png": "3f620de4ba4e340f0dbf673074acc0178f62cb3afed84d996de578f916c2e119",
    "assets/fonts/inter-ui-metrics.txt": "31c2ac1bfe015e48965ad4a0ca101ae22ea9c0db1629297ccd606c44bc20e261",
    "assets/fonts/Inter-LICENSE.txt": "262481e844521b326f5ecd053e59b98c8b2da78c8ee1bdbb6e8174305e54935a",
    "assets/fonts/InterVariable.ttf": "4989b125924991b90d05b2d16e0e388c48f7d5bb8b30539bbf9c755278d0ccaf",
    "build/ui-font.suf": "b39b35d4749a5eeaa5d900bd4d6579c6df014d70b47756fce1506634e28e7964",
}

WALLPAPERS = [
    "assets/sapote-redwood-wallpaper.png",
    *[f"assets/wallpapers/{number:02d}-{name}.png" for number, name in enumerate((
        "galaxy-stars", "milky-way-lake", "milky-way-reflection",
        "forest-waterfall", "waterfall-valley", "desert-dunes", "aurora",
        "aurora-fjord", "golden-mist-forest", "yosemite-mist",
        "alpine-lake", "tropical-sunset", "ocean-cliffs",
    ), 1)],
]
WALLPAPER_MANIFEST = "3e0c3c3f115b0a44563a5745da37a384df4e20f48a5c821a6efe74edf4d8cf2b"


def digest(path: str) -> str:
    source = Path(path)
    if not source.is_file():
        raise SystemExit(f"missing UI asset: {path}")
    return sha256(source.read_bytes()).hexdigest()


def main() -> None:
    for path, expected in PINNED.items():
        actual = digest(path)
        if actual != expected:
            raise SystemExit(f"UI asset digest mismatch: {path}: {actual}")

    manifest = "".join(f"{digest(path)}  {path}\n" for path in WALLPAPERS)
    actual_manifest = sha256(manifest.encode("utf-8")).hexdigest()
    if actual_manifest != WALLPAPER_MANIFEST:
        raise SystemExit(
            f"wallpaper source manifest digest mismatch: {actual_manifest}"
        )
    print(f"UI asset integrity: {len(PINNED)} pins and "
          f"{len(WALLPAPERS)} wallpaper sources verified")


if __name__ == "__main__":
    main()
