# Phrase Gen v0.2.0 — Schwung MIDI FX

6本の元ネタMIDIを解析し、それらと「同じ人が作ったような」8ステップ（16分音符×8）の
C Phrygianフレーズを、PADを押すたびに生成してMoveのテンポに同期してループ再生する
Schwung用MIDI FXです。自前の音源は持たず、後段のSound Generatorへ Note On/Off を送ります。

## 1. 利用しているSchwung APIと既存実装

| 項目 | 内容 | 参照した既存実装 |
|---|---|---|
| プラグインAPI | `midi_fx_api_v1_t`（`create_instance` / `process_midi` / `tick` / `set_param` / `get_param`）、エクスポートは `move_midi_fx_init` | `src/host/midi_fx_api_v1.h`、内蔵 `arp` / `chord` |
| 読み込み | chainが `modules/midi_fx/<id>/dsp.so` を `dlopen` | `src/modules/chain/dsp/chain_midi.c` `v2_load_midi_fx_slot` |
| 出力 | `tick()` が毎オーディオブロック（128 frames ≒ 2.9 ms）呼ばれ、返したメッセージは後段のMIDI FX → Synth へ流れる | `chain_midi.c` `v2_tick_midi_fx` |
| テンポ同期 | `host->get_beat_position()`（Moveのcable-0トランスポートから24PPQNを補間した拍位置。停止中は負）。MoveのMIDI Clock Out設定に依存しない | `src/host/shadow_transport.c`、chain LFOのsync |
| PAD入力 | Moveが PAD押下を「そのトラックのチャンネルのノート」に変換し、それがslotのMIDI FXの `process_midi` に届く（arpが押鍵を受け取るのと同じ経路） | `arp.c` |
| パラメータUI | `module.json` の `ui_hierarchy`（ノブ割当）＋ `get_param("chain_params")`（メタデータ） | `arp` / `chord` の module.json |
| 状態保存 | `get_param("state")` / `set_param("state")` のJSON（slot autosave / プリセット） | `chain_patch.c` |
| 配布・導入 | 独立パッケージ `dist/phrase-gen-module.tar.gz` を Schwung Manager の Custom install（tarball）へ | 同リポジトリの `maniac` と同方式 |

**最重要の制約（`src/host/plugin_api_v1.h` 冒頭）**: MIDI FXの全エントリポイント
（`create_instance` を含む）はSPIオーディオコールバック上で実行され、ファイルI/O・malloc・
ログ出力が禁止です。そのため:

- MIDIファイルは **ビルド時にCの配列として `dsp.so` に埋め込み**、実行時はメモリ上のバイト列を
  パースするだけにしています（`scripts/embed_patterns.py`）。
- 解析・生成・再生はすべて固定長配列＋有界ループ。確保はインスタンス1個の `calloc` のみ（arpと同じ）。
- 「デバッグログ」はデバイス上では出せないので、`get_param("debug")` で現在のフレーズを
  同じ書式で返し、さらにPC用CLI（`phrase-gen-cli`）で同じエンジンの出力を確認できます。

## 2. 実装方針

### 生成アルゴリズム（「元パターン中心の変形」方式）

1. 6本から1本を選ぶ（直前と同じソースは1回だけ引き直し）。
2. そのソースをコピーし、**Mutation**（0〜100%）に比例して以下を適用:
   - **リズム（v0.1.1〜 既定: 休符なし）**: 全8ステップが必ず発音します。元ネタの休符
     （pattern02 の6ステップ目）は生成前に Markov で音を埋め、ノートの削除・ずらしは行いません。
     `pg_gen_params_t.allow_rests = 1` にすると v0.1.0 の動作（ノート数 = 元の数 ±(Mutation×2)、
     削除は鳴りにくいステップから、追加は鳴りやすいステップから、nudge 少量）に戻ります。
   - **ピッチ**（元ノートの約 20+60×Mutation %）: 次のどれか1つを重み付きで選択
     - スケール上で ±1〜3 ステップ移動（上行/下行の比率は解析結果から）
     - オクターブ ±（元ネタのオクターブ分布に寄せる。跳躍率が高いほど選ばれやすい）
     - 直前ノートの同音連打
     - Markov再サンプル（音名はスケール度数の遷移表、音域は音程差の分布から）
   - **追加ノート**のピッチはMarkov、velocity/gateはソースの分布から。
   - **Velocity** 微変化（±3〜17）、**Note Length** 変化（×0.5〜×2、確率 35%×Mutation）。
3. 必ず **C Phrygian に量子化**し、元ネタ全体の音域（今回 C1〜Eb7）にオクターブ折り返し。
4. 直前のフレーズ・ソースそのものと同一になったら作り直し（Mutation>0のとき）。

ソース単体の統計（重み0.6）と6本全体の統計（0.4）を混ぜるので、「そのソースの骨格＋他のソースの手癖」になります。

解析する特徴（`pg_analyzer`）: 各ステップの発音確率（=休符位置）、ノート密度（平均/最小/最大）、
ピッチクラス分布、オクターブ分布、音域、直前ノートからの音程差の分布、同音連打率、上行/下行率、
オクターブ以上の跳躍率、スケール度数のMarkov遷移表、velocity分布（平均・平均偏差・範囲）、
Note Length分布。

**今回の6本の解析結果**（`phrase-gen-cli` 出力より）: ほぼ全ステップが発音（7〜8音/8ステップ）、
休符は pattern02 の6ステップ目のみ、音程差の57%がオクターブ以上の跳躍、同音連打0%、
上行64%/下行36%、velocityは全て100、長さは全て16分の92%。

**Mutation ごとの実測**（v0.1.1、各4000回平均）: 常に8音/8ステップ（休符なし）。

| Mutation | 元ノート維持 |
|---|---|
| 0% | 100%（元MIDIの音そのまま。休符だった位置だけ埋める） |
| 50%（既定） | 約50% |
| 75% | 約34% |
| 100% | 約20% |

### 再生（`pg_sequencer`）

- 独自タイマーなし。毎ブロック `get_beat_position()` を読み、16分境界を跨いだらそのステップを発音。
  ステップ番号はトランスポートの拍0基準なので、8ステップの頭は常に小節頭・半小節頭。
- 新しいフレーズは **次の8ステップ境界で切り替え**（途中で押してもタイミングは崩れない）。
  境界前に再度押したら最新のものが勝ちます。停止中に押した場合は即座に差し替え。
- 停止中は無音（フリーランしない）。
- ホストが `get_beat_position` を持たない場合のみ、`process_midi` に届く 0xF8 クロックで代替。
- ジッタ: ノートオンは最大1ブロック（≈2.9 ms）遅れ。MIDI FXの出力にサンプルオフセットが無いための上限です。

### stuck note対策

- 発音ごとにボイス（音高・チャンネル・終了拍）を記録。停止・CC120/123・0xFC・トランスポートの巻き戻し
  （停止を挟まないリスタート）で全ボイスを解放。
- 同じ音高が鳴っている最中に再発音するときは必ず Off→On の順。
- 出力バッファが一杯で送れなかったNote Offは「借り」として次のブロックで送る。
- PADモード切替中に押しっぱなしのキーがあっても、そのキーのNote Offは Note On と同じ扱い
  （通したOnのOffは必ず通す）。

## 3. ファイル構成

```
schwung-phrase-gen/
  module.json            chain_params（DSPのノブ4つ）
  ui_chain.js / ui_core.mjs  専用画面（パッド＝GENERATE、ノブ1〜5）
  pad_anim.mjs           パッドのアニメーション（RIPPLE / SNAKE / INVADE）
  help.json              デバイス上のヘルプ
  patterns/
    patterns.txt         使用するMIDIファイルの一覧と順番（ここを書き換えるだけで差し替え可）
    pattern01..06.mid    元ネタ（Hiro Ikezawa 作の 6 つのフレーズ）
  dsp/
    pg_pattern.h         共通型（ステップ/パターン）
    pg_midi_loader.[ch]  MidiPatternLoader: SMFパース → 任意区間を8ステップへ抽出（2段階に分離）
    pg_analyzer.[ch]     PatternAnalyzer: 上記の特徴を統計化
    pg_generator.[ch]    PatternGenerator: ソース選択＋変形（rhythm / pitch / dynamics の各ステージ）
    pg_scale.[ch]        ScaleQuantizer: root＋12bitマスク（今は C Phrygian）
    pg_sequencer.[ch]    Sequencer: トランスポート同期再生、境界での差し替え、ボイス管理
    pg_pad.[ch]          PadController: 入力ノート → GENERATE / 素通し / 破棄
    pg_library.[ch]      Loader＋Analyzerの結線（プラグインとCLIで共通）
    pg_format.[ch]       ノート名、デバッグ表示、state用エンコード
    pg_rng.h             決定的乱数（xorshift32）と重み付き選択
    phrase_gen.c         Schwung API との接着のみ（パラメータ、state）
  scripts/
    build.sh             native | arm64（既定）
    embed_patterns.py    patterns/ → build/generated/pg_patterns_data.h（＋Source選択肢を件数に合わせる）
    check_abi.py         AArch64・エクスポート・GLIBC≦2.34・RT禁止関数の不在を検査
    test.sh              ネイティブビルド＋テスト一式
    install.py           Schwung Manager の Custom install へアップロード
  tests/
    test_phrase_gen.c    単体＋API経由の結合テスト（ASan/UBSan）
    test_dlopen.c        ビルド済み dsp.so を chain と同じ方法で dlopen して駆動
  tools/
    phrase_gen_cli.c     PC用: 解析結果と生成結果をデバッグ書式で表示
```

## 4. ビルド

このリポジトリ内で（Linux/WSL、`gcc`、`aarch64-linux-gnu-gcc`、Python 3）:

```bash
bash scripts/test.sh          # ネイティブビルド＋全テスト
bash scripts/build.sh arm64   # Move用パッケージ
```

成果物: `dist/phrase-gen-module.tar.gz`（このモジュール内）。本体の標準9モジュールや
`scripts/build.sh` には加えていません（maniac と同じく独立パッケージ）。

## 5. インストール

Schwung導入済みのMoveへ、Managerの既存Custom install（tarball）で導入します。

```bash
python3 scripts/install.py http://move.local:7700 --check
python3 scripts/install.py http://move.local:7700
```

ブラウザで `http://<MoveのIP>:7700/modules` → Custom install → tarball で
`phrase-gen-module.tar.gz` を選んでも同じです。`modules/midi_fx/phrase-gen/` に展開されます。

## 6. Move上での操作

1. 使うMoveトラック（例: Track 1）の MIDI Out を ch1 にし、Moveの内蔵音を鳴らしたくなければ
   空のDrum Rack / Samplerプリセットを読み込む（Schwungの「Connecting Tracks」と同じ）。
2. Shift＋Volumeノブに触れながら Track 1 を押して Schwung の Slot 1 を開く（Recv Ch = 1）。
3. チェーンの MIDI FX 位置の **+** で **Phrase Gen** を選択、その後段の Synth に好きな音源を入れる。
4. Moveの **Play** を押すと、初期フレーズ（6本のどれかのバリエーション）が8ステップでループ。
5. Track 1 の **PADを押すたびに新しいフレーズ**が生成され、次の8ステップの頭から切り替わる。

### Phrase Gen の画面（v0.2.0〜）

チェーンで Phrase Gen を選んで開くと、標準のノブ画面の代わりに専用画面（`ui_chain.js`）が開きます。

- 全32パッドが **GENERATE ボタン**になり、**パッド全体でアニメーション**します（v0.4.0〜、`pad_anim.mjs`）。
  押したパッドは押している間だけ白。アニメーションはノブ5で選びます。
  - **RIPPLE（放射状）**: 真ん中から色とりどりの輪が広がる。押したパッドからも輪が広がる。
  - **SNAKE（スネークゲーム風）**: 白い頭のヘビが点滅するエサを追いかけ、食べた色で体が伸びる。
    押したパッドにエサが置かれる。14マスまで伸びるか行き止まりになると赤く点滅して最初から。
  - **INVADE（インベーダー風）**: 上の2段から敵が左右に動きながら降りてきて、最下段の白い砲台が自動で撃つ。
    押したパッドの列に砲台が移って撃つ。全滅させると次のウェーブ、着地されると全体が赤く点滅して最初から。
  - **AUTO（既定）**: 上の3つを15秒ごとに切り替え。
  - 約12コマ/秒（80 ms）で動き、変化したパッドだけLEDを送ります。ノブ5の選択はUIだけの設定で、DSPには保存されません
    （画面を開き直すと AUTO に戻ります）。
- この画面が開いている間は `host_pad_block` でパッドを横取りするので、**Move本体（純正の音源・シーケンサ）にはパッドが届きません**。
- 画面には再生中の8ステップ（音名、再生位置を反転表示）、次のフレーズが予約中なら右上に `NEXT`。
- **制限（Schwungの仕様）**: パッドの横取りは Schwung の画面が表示されている間だけ有効です。
  Back・Menu・トラック長押しなどで Move の画面に戻ると、shim が自動で解除し、パッドの色も Move のものに戻ります
  （パッドが永久に効かなくなる事故を防ぐための設計で、モジュール側からは変えられません）。
  画面を閉じている間は従来どおり、パッドは Move に届き、その音が Phrase Gen に届いて Trigger 設定に従って GENERATE します。
- **Shift+パッドだけで操作する方式は不採用**（2026-10-01 に試作して取りやめ）: Shift は Move が自分で使うボタンで、
  MIDI FX に届くノートには Shift の情報が無く、この画面以外（音源の画面・Move の画面）では実現できないため。

ノブ（画面表示中）:

| ノブ | パラメータ | 内容 |
|---|---|---|
| 1 | Mutation | 0 = 元MIDIの音そのまま / 50 = 既定 / 100 = 大胆（1目盛り=1） |
| 2 | Trigger | 画面を閉じている時の動作: `any` / `note` / `learn`（4目盛りで1段） |
| 3 | Trig Note | 画面を閉じている時の GENERATE ノート（既定36 = Drum Rackの左下PAD） |
| 4 | Source | `rnd` または 1〜6（4目盛りで1段） |
| 5 | Animation | `AUTO` / `RIPL` / `SNAK` / `INVD`（4目盛りで1段、パッドのアニメーション） |

ノブに触れると、そのノブの名前と値が大きく表示されます。
専用画面にしたため、標準ノブ画面の末尾にあった「My Presets / Module」ページはありません（状態は slot の autosave で保存されます）。

## 7. テスト方法

**PC（自動）**: `scripts/test.sh` — 約145万チェック、ASan/UBSan有効。

- Loader: 6本の内容（音高・休符・velocity・長さ）、ランニングステータス/SysEx/Meta/vel0 Note Off、
  任意区間抽出、切り詰め・ビット反転・ランダムバイト（計数万ケース）でクラッシュしない。
- Scale: 全128音の量子化がスケール内かつ最近傍、度数移動がスケール内。
- Generator: 5段階のMutationで計2万回 — 全音がC Phrygian・元ネタ音域内、常に8音（休符なし）、
  直前/ソースと必ず異なる、Mutation 0で元の音を保持、全ソースが選ばれる。
  `allow_rests=1` では Mutation 0で完全一致、3音のソースが6音以上にならない。
- Plugin: PAD 500回押して、生成された全フレーズに REST が無い。
- Sequencer: ノートオンがグリッドから1ブロック以内、途中で予約したフレーズが次の8ステップ頭で切替、
  停止で全解放・停止中は無音。乱数ファズ（テンポ変更・停止/再開・巻き戻し・フラッシュ・最大4倍のgate・
  出力バッファ1〜16）で二重On/孤立Off/stuck noteがゼロ。
- Plugin API: PAD連打4000回、`note`/`learn`、モード変更中のNote Off、CC123、state往復、
  `get_beat_position` 無しホストでのMIDIクロック代替。
- `test_dlopen`: ビルド済み `dsp.so` を chain と同じ手順で読み込み、4秒分再生（On数 = Off数）。

**生成結果を見る（デバッグログ相当）**:

```bash
build/native/phrase-gen-cli -n 8 -m 50 -s 1                  # 埋め込みの6本
build/native/phrase-gen-cli -n 8 -m 80 path/to/*.mid          # 任意のMIDIで試す
build/native/phrase-gen-cli -q -n 500 -m 50                   # 1行表示＋平均
```

出力例:

```
Source: pattern05.mid
Generated:
Step 1: C4 vel=98 len=92%
Step 2: Eb6 vel=100 len=92%
Step 3: C2 vel=100 len=92%
...
```

デバイス上の現在のフレーズは `get_param` の `debug` キーで同じ書式を返します
（chain経由のキー名は `midi_fx1:debug`。PC上では CLI で同じ内容を確認できます）。

**Move実機**: 上記6の手順で、(a) Play直後から鳴る、(b) PADごとに違うフレーズ、
(c) 切替が8ステップの頭、(d) テンポ変更・停止/再開で崩れない、(e) PAD連打・停止で音が残らない、を確認。

## 8. 元ネタMIDIの差し替え

`patterns/` にMIDIを置き、`patterns/patterns.txt` のファイル名を書き換えて再ビルドするだけです
（1〜16本。`patterns.txt` を消すと `*.mid` を名前順に全部使用）。
2列目に開始ステップを書くと長いMIDIの任意の8ステップを使えます（例: `riff.mid 16`）。
ノブ4の Source の選択肢も件数に合わせて自動で更新されます。

## 9. 拡張ポイント（今回は未実装）

| 機能 | 入れる場所 |
|---|---|
| Density | `stage_rhythm` の目標ノート数（`dmax` / `target`） |
| Mutation Amount | 実装済み（ノブ1）。全ステージがこの1値でスケール |
| Octave Range | `gen_ctx_t.lo/hi`（今は元ネタ全体の音域） |
| Random Seed | `pg_rng_seed()`（生成は決定的。CLIの `-s` と同じ） |
| Pattern Select | 実装済み（ノブ4 Source） |
| Scale / Root変更 | `pg_library_init(root, mask)` を作り直すだけ（`PG_ROOT` / `PG_SCALE_MASK`） |
| 8/16/32 step | `PG_LENGTH`。パターン長は最大32で全モジュールが `length` で動作 |
| 生成パターンの保存 | `pg_encode_pattern()`（stateに既に保存・復元している） |
| PADごとに異なるVariation | `pg_pad` が `GENERATE` と一緒にノート番号を返し、`pg_gen_params_t.source` / mutation を変える |
| Euclidean / Ratchet / Probability | `pg_step_t` にフィールドを追加し、`stage_rhythm` / `fire_step` で解釈 |
| MIDI CC生成 | `pg_seq_tick` の出力に CC を追加（出力枠は16/ブロック） |
| Phrase Generator | 生成した8ステップを `pg_pattern_t`（最大32）に連結 |

## 10. 既知の制限

- Phrase Gen を **バイパス（Mute＋Jog Click）または外した瞬間** に鳴っていたノートは、
  ホストがそれ以降 `tick()` を呼ばないため本モジュールからはNote Offを送れません
  （内蔵arpも同じ。Schwung本体側の仕様）。Moveを停止してから外すか、
  必要なら本体の `v2_unload_midi_fx_slot` / bypass時に synth panic を送る修正が必要です。
- Moveのトラックが同じPADで内蔵音を鳴らすのはMove側の動作です（手順6-1で消音）。
- ノートオンのタイミング精度はオーディオブロック単位（≈2.9 ms）。
