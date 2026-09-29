# PopPCE のライセンス

PopPCE は、Fredrik Ahlström 氏の [NitroGrafx](https://github.com/FluBBaOfWard/NitroGrafx) を移植した、**非公式・非商用**の改変版です。NitroGrafx の作者はこの移植に関わっていません。

ライセンスは2段になっています。

## 1. アプリ全体: NitroGrafx の作者の条件(非公式・非商用)

アプリ全体(`AppMain.exe`、およびこのリポジトリで公開しているソース全体)は、NitroGrafx の作者の条件で配布します。条件は次の2つです。

- **非公式であること**(公式版と偽らないこと)
- **非商用であること**

根拠は、[NitroGrafx issue #20](https://github.com/FluBBaOfWard/NitroGrafx/issues/20) での作者の返信(2026-08-12)です。

> My idea of a license is kind of "do what you want" but don't impersonate my releases, no commercial use.

NitroGrafx 本体と、それを元にした CE 用のファイル(`CE/core/GfxCE.s`、`CE/core/cefix.sed`、`CE/docs/upstream-fixes.diff`)は、この条件だけで使えます。これらには MIT の SPDX 行を付けていません。

### ARMH6280 と PCEPSG

ARMH6280(CPU)と PCEPSG(音源)には、それぞれの LICENSE ファイルがありません。どちらも作者が NitroGrafx のために作り、NitroGrafx の一部として配布しているものなので、NitroGrafx と同じ許諾に含まれるとみなして使っています。

根拠は、[issue #20](https://github.com/FluBBaOfWard/NitroGrafx/issues/20) の作者の返信(上の条件)と、[issue #22](https://github.com/FluBBaOfWard/NitroGrafx/issues/22) のやり取りです。#22 では、この移植と公開の予定を作者に伝えましたが、反対はありませんでした。

## 2. PopPCE の自作部分: MIT

PopPCE の自作部分は、MIT ライセンスです。本文は [`LICENSE`](LICENSE) にあります。対象は、先頭に `SPDX-License-Identifier: MIT` と書いてある次のファイルです。

- `CE/app/` のフロントエンド(`ce_*.c`、`ce_*.h`、`ce_res.rc`)。ただし、フォントのデータ `ce_shinonome16.h`、`ce_galmuri14.h`、`ce_galmuri11.h` は除きます(下の「第三者のもの」を参照)
- `CE/core/` の C とアセンブリ(`pce_core_ce.c`、`pce_render.c`、`pce_cd_ce.c`、`SgxCE.s`、`ce_asmfix.s`、`ce_stubs.c`、`mkvdc2.sh` など。`GfxCE.s` と `cefix.sed` は除く)
- `CE/compat/`、`CE/Makefile`、`CE/tools/galmuri2c.py`

自作部分だけを取り出して、ほかのプロジェクトで MIT として使うことができます。NitroGrafx と組み合わせて配布する場合は、1 の条件も守る必要があります。

## マスコットの絵とアイコン: CC0 1.0

メニューのマスコットの絵(`CE/icon/poppce_mascot.bmp`)、アプリのアイコン(`CE/icon/poppce.ico`、`AppMain.exe` に入っているもの)、README の先頭の絵(`.github/images/poppce_mascot_C_osanpo_4x.png`)は、[CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/)(パブリックドメイン)です。アイコンは、Pop シリーズのマスコットをもとに AI(Claude)で作りました。

## 上流のファイルについての注意

- リポジトリ直下の `logo.png`、`PCEngine.png` などの画像は、上流の NitroGrafx のファイルです。PopPCE では使っていません(`AppMain.exe` には入っていません)。
- `source/cueparser/timecode.c`、`timecode.h` は上流のリポジトリにありますが、PopPCE のビルドには使っていません。
- 上流のソースの著作権表示は、変えずに残しています。

## 第三者のもの

同梱している第三者のもの(Shared の MIT、CUEParser の MIT、libretro.h の MIT、Galmuri フォントの SIL OFL 1.1、東雲フォント)の著作権表示と許諾文は、[`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) にまとめています。

SuperGrafx のゲームを見分けるための CRC の値とゲーム名(6件)は、事実のデータとして、beetle-supergrafx-libretro の一覧を参考にしました。Mednafen のコードは使っていません。出どころは [`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) に書いています。
