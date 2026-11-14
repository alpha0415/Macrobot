# Macro Bot Mobile (Geode, Android)

Geometry Dash 2.2091(2.209x) 모바일 + Geode v6.0.0-alpha.1(프리릴리즈)용 매크로 봇. 레벨에 들어가면 왼쪽에 BOT / REC / PLAY 버튼이 생깁니다.

## 빌드 (폰/PC에 SDK 없이)
1. 이 폴더를 GitHub 저장소에 올린다 (`.github/workflows/build.yml` 포함).
2. Actions 탭 → "Build Geode Mod" 실행 → 아티팩트 `Build Output`에서 `.geode` 파일 다운로드.
3. 폰의 Geode 모드 폴더(`Android/media/com.geode.launcher/game/geode/mods/`)에 넣고 게임 재시작.

## 사용
- BOT: 메뉴(Macro / Hacks / Tools 3페이지). 일시정지 화면 우상단에도 BOT 버튼이 있음.
- REC: 처음부터 다시 시작하며 녹화. 연습 모드 체크포인트 되돌리기 지원.
- PLAY: 불러온 매크로를 처음부터 재생.
- 저장/불러오기: BOT → Macro 페이지 → Save / Load. 파일은 `<mod save dir>/macros/*.gdr.json`. Load의 Import로 외부 파일 복사 가능.
- 클리어 방지(Safe mode): 기본 ON. 엔드월에 닿아도 클리어 처리 안 되고 기록/퍼센트도 저장 안 됨.

## 기능
Record/Play, Accuracy(Vanilla/Input Fixes/Frame Fixes), Frame Offset, Practice 체크포인트 정리, 매크로 저장·불러오기·가져오기·삭제, Auto-save,
Noclip, Safe mode, Instant respawn, No death effect, Speedhack, TPS bypass, Frame stepper(+1/+10), Autoclicker.

## 알려진 한계
- 미구현: Renderer(ffmpeg), Clickbot, Show Trajectory, Layout Mode, Seed modifier, Macro editor, Coin finder.
- 이 환경에서는 컴파일하지 못했습니다. 알파 API 변경으로 CI에서 오류가 날 수 있습니다.
- 2.2091 `processCommands`에 half-tick 인자가 생겨 타이밍이 반 틱 어긋날 수 있습니다(기기 테스트 필요).
