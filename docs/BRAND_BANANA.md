# Banana Player - Brand Identity

## Name

- **English:** Banana Player
- **Korean:** 바나나 플레이어 (banana peulleieo)
- **Short form:** Banana
- **Mascot cameo:** a smiling banana that plays videos (referenced in copy, not
  rendered as art). When we need a glyph in-app, use a curved banana silhouette
  whose inner curve doubles as a rightward play triangle.

## Tagline

Three candidates were considered:

1. "Peel and play." (English) / "까서 재생." (Korean)
2. "Any video. Instantly ripe." / "모든 영상, 방금 익음."
3. "Playful. Fast. Yellow." / "가볍고 빠른, 노란 플레이어."

**Chosen (English):** *Peel and play.*
**Chosen (Korean):** *까서 바로 재생.* (kkaseo baro jaesaeng - "peel and play immediately")

Rationale: it's the shortest of the three, it lands the banana metaphor with
a verb, and it echoes the "keyboard-first" instant-gratification promise of
the underlying app without repeating the old marketing language.

## Palette

Warm-forward dark theme, banana yellow as the single accent (replaces the
old sky-blue). Amber remains the "off-nominal" secondary. Neutrals are
slightly warmer than before (rock 8 -> 12 nudged +5 red).

| Role                          | Hex     | RGB              | Where it lives                              |
|-------------------------------|---------|------------------|---------------------------------------------|
| Accent (banana)               | #FFC93A | 255, 201,  58    | Buttons active, seek fill, focus rings      |
| Accent hover / thumb glow     | #FFDE7A | 255, 222, 122    | Hover/focus states, seek thumb outer glow   |
| Accent pressed (peel-toe)     | #E39A1C | 227, 154,  28    | Pressed states, gradient toe                |
| Warn / "B" marker             | #FF884A | 255, 136,  74    | AMP badge, warnings, B-loop marker          |
| OK / "A" marker               | #7ADC7A | 122, 220, 122    | A-loop marker                               |
| Text high                     | #F4F1E9 | 244, 241, 233    | Body copy                                   |
| Text low                      | #B5AE9B | 181, 174, 155    | Meta / secondary copy                       |
| Background base               | #12100C | 18,  16,  12     | Window base (warm near-black)               |
| Background elevated (popups)  | #1B1812 | 27,  24,  18    | Popup, panel                                |
| Background panel border       | #3A3428 | 58,  52,  40    | Hairline borders                            |

Rationale: the previous palette (#60B0F6 sky blue on cool #0E1116) reads
"utility software". A single warm banana-yellow accent on a slightly warmer
base makes the app feel friendly without going the full playschool route.
Text-on-yellow contrast is checked: near-black label on `#FFC93A` clears
WCAG AAA.

## Typography

- **UI:** Segoe UI (Windows default) at 16 px base. No change from before -
  it renders crisp at every size and doesn't pull us into font licensing.
- **Korean:** Malgun Gothic merged into the atlas (already present).
- **Wordmark:** Rendered from the loaded UI font at ~7-13% of window height.
  Slightly increased letter spacing (12% of glyph size, up from 10%) gives
  "Banana Player" enough breathing room to read as a mark rather than body
  copy.

## Voice & tone

- **Playful, not childish.** "Peel and play" is the tone ceiling. Do not add
  emojis, banana puns in dialogs, or exclamation-point-heavy microcopy.
- **Direct.** Error strings still say "Cannot open <file>" - the rebrand
  doesn't loosen the writing.
- **Bilingual by default.** New user-facing strings should be considered in
  both English and Korean at write time. Korean audience is the primary
  target market; do not ship English-only tooltips for shipping features.

## Korean market considerations

1. **Name recognition.** "Banana" is written in katakana / hangul as
   [바나나](banana) in Korean and is universally understood - no
   localization footnote needed. Unlike the previous "Vela" name (which
   lacked a hangul pronunciation cue), 바나나 플레이어 is trivially typable
   and searchable on Naver / Google KR.
2. **Color association.** Yellow reads "cheerful, casual" in the Korean
   consumer software market (Kakao's brand equity has done decades of that
   work for us). This is the opposite of the "corporate blue" many domestic
   players use (PotPlayer, KMPlayer both lean deep-blue/black) - so we get
   differentiation for free.
3. **Font ordering.** In dialogs that mix Latin + Hangul, keep the layout
   left-to-right and let Malgun handle the mixing - do NOT swap font atlases
   mid-line. The subtitle preset picker already handles this correctly.
4. **Sidecar files now use the `banana.` prefix** with a one-time
   migration from any pre-rename `vela.*` files present next to the exe
   on first launch, so Korean users who have already accumulated watch
   history experience zero data loss on upgrade.

## Rebrand scope

Full rename shipped. Internal identifiers were migrated across the board:
`BananaPlayer.exe`, `%APPDATA%\BananaPlayer\`, `banana.config`, registry
keys under `HKCR\Applications\BananaPlayer.exe`, CMake target
`BananaPlayer`. A first-launch sidecar migration copies any legacy
`vela.*` files (config / resume / bookmarks / recent / session.m3u /
stats / hdrlut / window) to the `banana.*` names so an upgrading user's
watch history, resume positions, and preferences carry over automatically.
The installer replaces the old file-association registrations rather than
duplicating them.

User-facing strings that changed:

- Splash wordmark (old "VELA" -> "Banana Player" / "바나나 플레이어")
- Splash tagline
- About panel wordmark, tagline, menu item ("About Vela" -> "About Banana Player")
- Window title ("<file> - Vela" -> "<file> - Banana Player")
- Welcome banner header ("Welcome to Vela" -> "Welcome to Banana Player")
- App icon (blue rounded square + play triangle -> banana silhouette whose
  inner curve reads as a play triangle at every size)
- Palette (sky blue accent -> banana yellow accent; base neutrals shifted
  slightly warmer)
