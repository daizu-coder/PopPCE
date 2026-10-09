# PopPCE

> **Unofficial, non-commercial port.** PopPCE is an unofficial port of
> NitroGrafx by Fredrik Ahlström to the SHARP Brain PW-G5300 (Windows CE).
> It is not an official NitroGrafx release. The original author is not
> involved in it and does not provide support for it, so please report
> PopPCE issues here, not to the NitroGrafx project.
> Used with permission, based on the author's reply in
> [issue #20](https://github.com/FluBBaOfWard/NitroGrafx/issues/20)
> (no commercial use, and it must not be presented as an official release).

## 概要

- SHARP の電子辞書 **Brain PW-G5300**(Windows CE / ARM)向けに、PC エンジンのエミュレータ **NitroGrafx**(Fredrik Ahlström 氏作、ニンテンドーDS 用)を移植したものです。エミュレーションの中心(ARM アセンブリ)はそのまま使い、DS の画面処理の部分をソフトウェアの描画に置き換えて、Win32 のフロントエンドで動かしています
- HuCard、SuperGrafx、CD-ROM² のゲームに対応しています
- 無料・非商用のホームブリュー(自作ソフト)で、ソースコードを公開しています
- ライセンスは、全体が **NitroGrafx の作者の条件(非公式・非商用)**、自作部分が **MIT**、マスコットの絵とアイコンが **CC0 1.0** です。詳しくは [`LICENSING.md`](LICENSING.md)・[`LICENSE`](LICENSE)・[`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) を見てください
- ゲームの ROM、CD のイメージ、CD-ROM² のシステムカード(BIOS)は同梱していません。ご自身で用意してください
- NitroGrafx の公式版ではありません。NitroGrafx の作者はこの移植に関わっていないので、不具合はこちらに報告してください

## ビルド方法

- ツール
  * cegcc(`arm-mingw32ce-*`、`/opt/cegcc`)— Windows CE / ARM 向けのクロスコンパイラ。製作者は WSL(Windows 上の Linux)でビルドしています
  * GNU sed、cpp(上流のアセンブリをビルドのときに変換します)
- サブモジュール `source/ARMH6280`(CPU)と `source/Shared`(NDS_Shared)
- 手順
  * `CE/` で `make && make strip` を実行します
  * `CE/AppMain.exe` ができます(依存する DLL は `COREDLL.dll` だけ)
  * 上流のファイルは書き換えません。Windows CE 向けの変更は、ビルドのときに `core/cefix.sed` で当てます

**クローンするときは `--recursive` を付けてください。** 付けないと、サブモジュールのフォルダが空のままになり、ビルドできません(`make` は最初に、どのフォルダが空かを表示して止まります)。GitHub の「Download ZIP」にもサブモジュールの中身は入らないので、ZIP では取得しないでください。

```sh
git clone --recursive https://github.com/daizu-coder/PopPCE.git
cd PopPCE/CE
make
make strip
```

`--recursive` を付けずにクローンしてしまった場合は、リポジトリの中で次を実行してください。

```sh
git submodule update --init
```

## 使用方法

- `AppMain.exe` を、実機(PW-G5300)の好きなフォルダに置いてください
- 開けるファイル
  * HuCard のゲーム: `.pce`
  * SuperGrafx のゲーム: `.sgx`(`.pce` でも、SuperGrafx 専用のゲームとして知られているものは SuperGrafx として動きます)
  * CD-ROM² のゲーム: `.cue`(`.cue` と、それに対応する1つの `.bin` の組)
- CD-ROM² のシステムカード
  * システムカード(NEC・ハドソンの著作物)は同梱していません。CD-ROM² のゲームを遊ぶには、ご自身で用意したシステムカードのイメージが必要です
  * システムカードのファイル名は自由です。拡張子は `.pce` にしてください(256KB。先頭に 512 バイトのヘッダが付いたものも使えます)
  * CD のイメージ(`.cue`)と同じフォルダか、`AppMain.exe` と同じフォルダに置いてください。そのフォルダの中だけを探します(サブフォルダは探しません)
  * どちらにも見つからないときは、メッセージのあとにファイルを選ぶ画面が出るので、システムカードを選んでください。選んだフォルダは設定ファイルに覚えていて、次からは最初にそこを探します
  * システムカードかどうかは中身で見分けるので、HuCard のゲームと同じフォルダに置いても大丈夫です。何枚かあるときは、SUPER CD-ROM² のシステムカード 3.0 を選びます
- セーブ
  * CD-ROM² のゲームのセーブ(バックアップメモリ)は、`.cue` と同じフォルダに、`.cue` のファイル名のうしろに `.srm` を付けた名前(例: `game.cue.srm`)で保存します。バックアップメモリはゲームごとに別です
  * ステートセーブは、ゲームのファイルと同じフォルダに、ファイル名のうしろに `.state` を付けた名前(例: `game.pce.state`)で保存します(ゲームごとに1つ)
  * スクリーンショットは、`AppMain.exe` のフォルダの `Screenshots` に保存します

## 動作確認環境

- SHARP Brain PW-G5300

PopGBA を CeOpener や CERestorer から起動すると、PW-G5300 で、終了後に画面が真っ暗になる、または操作を受け付けなくなることがありました。USB ケーブルと電池を抜いてから入れ直すと戻りました。PopPCE での動作は確かめていません。

## クレジット

- **NitroGrafx、ARMH6280(CPU)、PCEPSG(音源)**— Fredrik Ahlström 氏。PopPCE の中心となるエミュレータです。移植を快く認めてくださり、不具合の報告にも丁寧に対応していただきました。
  <https://github.com/FluBBaOfWard/NitroGrafx>
  ARMH6280 の多くの部分は、Loopy 氏が始めた PocketNES をもとにしています
- **Shared(NDS_Shared)**— Fredrik Ahlström 氏、MIT
- **CUE ファイルの読み込み(CUEParser)**— Rabbit Hole Computing、Fredrik Ahlström 氏、MIT
- **libretro API のヘッダ**— The RetroArch team、MIT
- **SuperGrafx のゲームの CRC の一覧**(`.pce` の SuperGrafx のゲームを見分けるもの)— CRC の値とゲーム名(6件)は、事実のデータとして beetle-supergrafx-libretro の一覧を参考にしました(Mednafen のコードは使っていません)。一覧をまとめてくださった Mednafen project と libretro の移植チームに感謝します
- **Galmuri** ビットマップフォント(メニューの既定の文字)— Lee Minseo(quiple)氏、SIL Open Font License 1.1
  <https://github.com/quiple/galmuri>
- **東雲(しののめ)16 ドットビットマップフォント** — メインデザイン 古川 泰之 氏ほか、The Electronic Font Open Laboratory(/efont/)。実質パブリックドメイン
  <https://github.com/code4fukui/shinonome-font>
- **CeGCC** — Windows CE / ARM 向けのクロスコンパイラ
- **SHARP Brain homebrew コミュニティ** — 端末の情報を残してくださった皆さん
- Windows CE 向けの部分(`CE/`)は、このプロジェクトで作りました

コンポーネントごとの出所とライセンスの詳細は、[`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) と [`LICENSING.md`](LICENSING.md) を見てください。

## 制作について

コードとマスコットの絵はAI(Claude)で作りました。製作者はプログラムを読めません。

製作者がしたのは、AI への指示と、実機(PW-G5300)での確認です。不具合があったときは、AI にデバッグ用の記録(ログ)を残す仕組みを作ってもらいました。そのうえで同じ操作をもう一度試し、記録を AI に読んでもらって原因を調べ、直してもらうことを繰り返して作りました。作る中で実機でわかったことは、[NOTES.md](NOTES.md) にまとめています。

マスコットの絵とアイコン(`CE/icon/`)は CC0 1.0(パブリックドメイン)です。アイコンは、Pop シリーズのマスコットをもとに AI(Claude)で作りました。

## ライセンス

PopPCE 全体は、NitroGrafx の作者の条件で配布します([issue #20](https://github.com/FluBBaOfWard/NitroGrafx/issues/20) での作者の返信)。

- 公式版と偽らないこと(非公式であること)
- 商用利用しないこと

PopPCE の自作部分(`SPDX-License-Identifier: MIT` と書いてあるファイル)は MIT です([`LICENSE`](LICENSE))。自作部分だけを取り出して、ほかのプロジェクトで MIT として使うことができます。バイナリを配るときは、[`THIRDPARTY_LICENSES.txt`](THIRDPARTY_LICENSES.txt) も一緒に配ってください。

## 商標・免責

PopPCE は非公式のファンプロジェクトです。NEC、ハドソン、コナミ、シャープ株式会社などの権利者とは関係がなく、許諾・後援・承認も受けていません。

- 「PCエンジン」「PC Engine」「CD-ROM²」「SUPER CD-ROM²」「スーパーグラフィックス」「SuperGrafx」「TurboGrafx」などは、それぞれの権利者の商標です。
- 「SHARP」「Brain」はシャープ株式会社の商標です。

ゲームの ROM、CD のイメージ、システムカード(BIOS)は含みません。ご自身で合法的に用意したものを使ってください。
