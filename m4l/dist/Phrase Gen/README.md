# Phrase Gen — Max for Live版

8ステップ（16分音符×8）の C Phrygian フレーズを、**Ableton Live のクリップに直接書き込む**
Max for Live MIDIエフェクトです。再生も録音もしません。GENERATE を押すとクリップの
ノートが置き換わり、Live の通常の編集と同じく Ctrl+Z / Cmd+Z で元に戻せます。

生成エンジンは Ableton Move 用 MIDI FX「Phrase Gen」と同じもので、C のコードを
JavaScript に1行ずつ移植しています。元ネタの6本のMIDIは組み込み済みです。
同じ乱数シードなら Move 版とまったく同じフレーズを生成します
（`tests/test_engine.js` で検証）。

## 動作環境

- Ableton Live 11 / 12 と Max for Live（Suite、または Standard + Max for Live）
- Live 10 は非対応です（Live 11 で追加されたノート編集APIを使っているため）。

## インストール

次の2ファイルを **同じフォルダに入れたまま** 置いてください。

```
Phrase Gen/
  Phrase Gen.amxd
  phrase_gen_m4l.js
```

フォルダごと User Library にコピーします。例:
`Documents/Ableton/User Library/Presets/MIDI Effects/Max MIDI Effect/`
その後、`Phrase Gen.amxd` を MIDI トラックにドラッグします。

（任意）1ファイルにまとめたい場合: デバイスの Edit ボタンで Max を開き、
*Freeze Device* してから保存します。フリーズした .amxd には .js が内蔵されます。

## 使い方

1. デバイスを MIDI トラックに挿します。入ってきた MIDI はそのまま通過します。
2. **Session View**: 任意の MIDI トラックのクリップスロットをクリックします。
   - 空のスロット → 新しいクリップを作ります。
   - クリップがあるスロット → そのクリップのノートをすべて置き換え、ループ長を
     Length に合わせ、名前を `PG <ソース番号>` にします。
3. **Arrangement View**: MIDI クリップを選択します。長さと位置はそのままで、
   ループ範囲全体にフレーズを繰り返して書き込みます。
4. **GENERATE** を押します。もう一度押すと別のフレーズになります。

| 操作 | 内容 |
|------|------|
| Mutation | 0% = 元フレーズそのまま、50% = 約半分のノートを変更、100% = 大胆に変更 |
| Source | `random`（同じソースは2回続けて選ばない）、または 1〜6 を固定 |
| Length | Session のクリップのみ: 8 steps（2拍）/ 1 bar / 2 bars / 4 bars。8ステップのフレーズを繰り返して埋めます |
| GENERATE | 対象のクリップに新しいフレーズを書き込みます |

全ステップが発音します（休符なし）。ノートは16分音符で、C Phrygian
（C Db Eb F G Ab Bb）に量子化されます。ボタンの下のステータス行に、何を書き込んだか、
または書き込めなかった理由が表示されます（デバイス表示は英語です）。

## ビルド（開発者向け）

```
python3 build_m4l.py        # ../patterns の MIDI を埋め込み、dist/Phrase Gen/ を作る
node tests/test_engine.js <参照ファイル>
node tests/test_live_glue.js
```

`build_m4l.py` は `tools/pg_ref_dump.c` を `../dsp` の C エンジンと一緒にコンパイルして
実行するので、デバイスに組み込まれるフレーズは Move 版が同じ MIDI から作るものと
完全に同じになります。

Copyright (c) 2026 Hiro Ikezawa. MIT License.
