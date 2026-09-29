/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * 東雲16ドットビットマップフォント(sys/ce/ce_shinonome16.h、
 * sys/ce/tools/shinonome2c.py で shinonome-font から生成)による
 * 日本語/英語UIテキストの直接描画。ce_lang.c/.h がかつて担っていた
 * jptahoma.ttc の AddFontResource + TrueType 前提の描画を置き換える。
 *
 * ライセンス上の経緯: jptahoma.ttc は Microsoft 製プロプライエタリ
 * フォントを他機種ROMから抜き出して転載したものである可能性が高く、
 * 配布物への同梱がライセンス的にグレーと判断され撤去された。東雲
 * フォント自体のライセンス文・著作者表記はリポジトリ直下の
 * THIRDPARTY_LICENSES.txt に同梱している。
 */
#ifndef CE_BMPFONT_H
#define CE_BMPFONT_H

#include <windows.h>

#define CE_BMPFONT_HEIGHT 16

/* 字形が CE_BMPFONT_HEIGHT の枠より下に何行はみ出すかに応じて、行を
 * 積み重ねるリスト(ce_fileopen.c)の行の高さに足す値。東雲16は0。
 * Galmuri14(make CE_FONT=galmuri14、ce_bmpfont.c 参照)は g j p q y の
 * 下の出っ張りが枠の2行下まで出るので1。 */
#if defined(CE_FONT_GALMURI14)
#define CE_BMPFONT_ROW_EXTRA 1
#else
#define CE_BMPFONT_ROW_EXTRA 0
#endif

/* 半角(ASCII可視域・半角カナ)は8px、全角(漢字・かな・記号)は16px幅
 * で連続して左詰めに描画する。背景は透過(消灯ピクセルは何も描かない
 * - hdc の既存の背景色/パターンがそのまま透けて見える、ダイアログの
 * 既存STATICラベルと同じ見た目になる)。未収録のコードポイントは
 * 8px幅の空白として送る(文字化け/tofu化けの代わりに単に欠落させる)。
 * 戻り値は実際に描画した幅(px)。
 */
int CeBmpFontDrawTextW(HDC hdc, int x, int y, const wchar_t *text, COLORREF fg);

/* CeBmpFontDrawTextW() と同じだが、未収録コードポイントを空白で送る
 * 代わりに □(U+25A1 WHITE SQUARE)の全角グリフで描く。サロゲートペア
 * (BMP外の文字)は wchar_t 2個を1文字として □ 1個にまとめる。ファイル
 * 選択ダイアログ(ce_fileopen.c)のリスト項目とパス表示専用 - ユーザーが
 * 任意に付けたファイル名/フォルダ名で、字が消えると別ファイルと
 * 見分けが付かなくなる/名前が欠けていることが伝わらないため、そこ
 * だけ「表示できない文字がある」ことを明示する。他のUI(ボタン/ラベル)
 * は自前の固定文字列しか描かないので従来どおり空白送り。 */
int CeBmpFontDrawTextBoxedW(HDC hdc, int x, int y, const wchar_t *text, COLORREF fg);

/* 描画は行わず、CeBmpFontDrawTextW() が描くのと同じ幅(px)だけを計算
 * する。ボタン/ラベル内でのセンタリングに使う。高さは常に
 * CE_BMPFONT_HEIGHT。 */
int CeBmpFontGetTextWidth(const wchar_t *text);

/* BS_OWNERDRAW な PUSHBUTTON/DEFPUSHBUTTON の WM_DRAWITEM をまるごと
 * 処理する共通ヘルパー。枠と押下/フォーカス状態は
 * DrawFrameControl()/DrawFocusRect() で標準のボタンそのままの見た目に
 * し、ボタンの現在のテキスト(GetWindowTextW)だけを東雲フォントで中央
 * 揃え描画する。ダイアログの WM_DRAWITEM ハンドラから、対象コント
 * ロールIDのときにそのまま渡す。 */
void CeBmpFontDrawOwnerButton(const DRAWITEMSTRUCT *dis);

/* CeBmpFontDrawOwnerButtonTheme() が描ける簡易ベクターアイコン(1〜2px
 * の線・矢印付き四角形のみで構成、東雲フォントと同じ「濃い一色」の
 * 輪郭線スタイル)。ビットマップではなく実行時にGDI一次プリミティブ
 * (Rectangle/Ellipse/Polygon/Polyline)で描く - 解像度・配色替えの
 * たびに画像を作り直す必要がない。 */
typedef enum
{
    CE_MENU_ICON_NONE = 0,
    CE_MENU_ICON_OPEN,   /* ROMを開く: フォルダ+「+」 */
    CE_MENU_ICON_SAVE,   /* ステートセーブ: フロッピーディスク */
    CE_MENU_ICON_LOAD,   /* ステートロード: フォルダ */
    CE_MENU_ICON_VIDEO,  /* 画面設定: モニター */
    CE_MENU_ICON_SOUND,  /* サウンド設定: スピーカー+音波 */
    CE_MENU_ICON_INPUT,  /* ボタン設定: ゲームパッド */
    CE_MENU_ICON_SCREENSHOT, /* スクリーンショット: カメラ */
    CE_MENU_ICON_EXIT    /* 終了: 電源マーク */
} CeMenuIcon;

/* CeBmpFontDrawOwnerButton() の色付き・角丸版。標準の3D風フレーム
 * (DrawFrameControl)の代わりに RoundRect() で塗り+枠を描く。呼び出し
 * 側が背景色・枠線色・文字色・アイコン種別・ダイアログ自体の背景色
 * (`windowBg` - ボタンの角丸の外側、rcItem四隅の余白をこの色で塗り、
 * ボタン専用ウィンドウの素の背景が白く覗くのを防ぐ)を指定する
 * (ODS_DISABLED時は指定色を無視してグレー表示に、ODS_FOCUS時は白黒
 * 反転にフォールバックし、押下時の見た目調整もその上に重ねて内部で
 * 自動的に行う)。`stacked` が真なら「アイコンが上段・文字が下段」の
 * 2行レイアウト(画面/サウンド/ボタン設定の3並びボタン用)、偽なら
 * 「アイコンが左・文字が右」の1行レイアウト(それ以外のボタン用)。
 * メインメニューのパステル配色ボタン専用 - 他のダイアログ(Sound/
 * Video/Input Config等)のボタンは従来どおり CeBmpFontDrawOwnerButton()
 * を使い続ける。 */
void CeBmpFontDrawOwnerButtonTheme(const DRAWITEMSTRUCT *dis, COLORREF bg, COLORREF border, COLORREF text,
                                    CeMenuIcon icon, int stacked, COLORREF windowBg);

/* BS_OWNERDRAW な CHECKBOX 用の同等ヘルパー。四角い枠+チェックマーク
 * を左側に描いてから、テキストをその右に描く。
 *
 * `checked` は呼び出し側が明示的に渡す - BM_GETCHECK/BM_SETCHECK/
 * CheckDlgButton() には *あえて* 頼らない設計。RCの CHECKBOX は
 * ベースで BS_CHECKBOX(値2)を暗黙設定するが、そこに BS_OWNERDRAW
 * (値0x0B)を追加するRC記法は単純なビットOR(2 | 0x0B = 0x0B)にしか
 * ならず、下位4ビット全体を占めるボタンタイプ値としては
 * BS_CHECKBOX情報がBS_OWNERDRAWに完全に上書きされてしまう。実機で
 * このため BM_GETCHECK/BM_SETCHECK がチェックボックスとして機能せず
 * (タッチでも決定キーでもチェック済み表示にならない)、フロント側に
 * 既に存在する実データ(colorfilterのint*、CeFileOpenGetRememberLast()
 * 等)をチェック状態の一次情報として直接参照する方式に変更した。 */
void CeBmpFontDrawOwnerCheckbox(const DRAWITEMSTRUCT *dis, int checked);

/* 非表示にした STATIC(LTEXT) コントロールを、その本来の位置に東雲
 * フォントで描く。ダイアログの WM_PAINT から、日本語/英語切り替えの
 * 対象ラベルIDそれぞれについて呼ぶ。STATIC自体は
 * ApplyXxxLanguage() 側で ShowWindow(..., SW_HIDE) 済みであることが
 * 前提(隠しても GetWindowTextW/GetWindowRect は引き続き有効)。 */
void CeBmpFontPaintLabel(HDC hdc, HWND hDlg, int ctrlId);

/* CeBmpFontPaintLabel() と同じだが未収録文字を □ で描く。
 * ce_fileopen.c の IDC_FO_PATH(カレントディレクトリのパス表示)専用。 */
void CeBmpFontPaintLabelBoxed(HDC hdc, HWND hDlg, int ctrlId);

#endif
