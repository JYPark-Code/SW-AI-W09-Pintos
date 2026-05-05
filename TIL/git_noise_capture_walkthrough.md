# Git Bash 캡쳐 워크스루 — CRLF/filemode 노이즈 시연

> 이 문서대로 위에서 아래로 따라가면, 발표에 쓸 캡쳐 6장을 찍을 수 있습니다.
> **소요 시간**: setup 2~3분 + 캡쳐 5분
> **환경**: Windows 위의 Git Bash (어느 디렉토리에 있어도 OK — 스크립트가 알아서 `~/git-noise-demo` 에 만듭니다)

---

## STEP 0 — Git Bash 열기

1. 윈도우 시작 메뉴 → "Git Bash" 검색 → 실행
2. 새로 열린 검정 터미널 창의 프롬프트가 보이면 OK
3. **현재 위치는 신경 쓰지 않아도 됩니다** — 어디에 있든 다음 setup 명령이 알아서 `~/git-noise-demo` (= `C:\Users\<당신>\git-noise-demo`) 에 작업 폴더를 만듭니다.

---

## STEP 1 — Setup 스크립트 실행 (한 번만)

**아래 블록 전체를 복사해서 git bash 창에 우클릭으로 붙여넣고 Enter.**

> ⚠️ **Windows 사용자 주의**: 클론 시 `-c core.autocrlf=false` 가 반드시 들어가야 합니다.
> 안 그러면 Windows 기본값(autocrlf=true)이 클론 시 파일을 미리 CRLF 로 바꿔놓아서, 이후 sed 노이즈가 5~8개 파일에서만 발현됩니다 (대신 358개여야 정상).

```bash
REPO_URL="https://github.com/JYPark-Code/SW-AI-W0910-Pintos.git"
DEMO=~/git-noise-demo

rm -rf "$DEMO" && mkdir -p "$DEMO" && cd "$DEMO"

for s in stage1-baseline stage2-crlf stage3-filemode stage4-both; do
  git -c core.autocrlf=false clone -q "$REPO_URL" "$s"
  git -C "$s" config core.autocrlf false
  git -C "$s" checkout -q dev
  git -C "$s" config user.email "demo@local"
  git -C "$s" config user.name  "demo"
  git -C "$s" rm -q .gitattributes
  git -C "$s" commit -qm "demo: drop .gitattributes"
done

S=$DEMO/stage2-crlf
git -C "$S" ls-files | grep -E '\.(c|h|md|mk|txt|S|py|sh|json|yml|yaml|lds)$|Makefile|^README|activate$' | while read -r f; do
  sed -i 's/$/\r/' "$S/$f" 2>/dev/null || true
done

S=$DEMO/stage3-filemode
git -C "$S" config core.filemode true
git -C "$S" ls-files | while read -r f; do chmod -x "$S/$f"; done

S=$DEMO/stage4-both
git -C "$S" config core.filemode true
git -C "$S" ls-files | while read -r f; do
  chmod -x "$S/$f"
  case "$f" in
    *.c|*.h|*.md|*.mk|*.txt|*.S|*.py|*.sh|*.json|*.yml|*.yaml|*.lds|Makefile*|README*|*activate)
      sed -i 's/$/\r/' "$S/$f" 2>/dev/null || true ;;
  esac
done

echo ""
echo "=== Setup 완료 ==="
for s in stage1-baseline stage2-crlf stage3-filemode stage4-both; do
  echo "[$s] modified=$(git -C "$DEMO/$s" status --porcelain | wc -l)"
done
```

### 기대 결과

```
=== Setup 완료 ===
[stage1-baseline] modified=0
[stage2-crlf] modified=358          ← 빨갛게 358개 (메인 캡쳐)
[stage3-filemode] modified=0~수십    ← Windows native는 NTFS 한계로 약함 (정상)
[stage4-both] modified=358          ← Windows에선 CRLF 부분만 살아남음
```

> **stage 2 가 5~8개로 적게 나오면**: 위 setup 블록의 `git -c core.autocrlf=false clone` 부분이 빠진 채로 클론된 겁니다. 한 번 더 setup 블록을 통째로 다시 실행하세요 (기존 폴더 자동 삭제됨).
>
> **stage 3 가 0이면**: Windows native git의 정상 동작입니다. NTFS는 unix 실행비트를 추적 못 하기 때문. CRLF 데모(stage 2, 4)는 멀쩡하니 그것 위주로 캡쳐하시면 충분합니다.

---

## STEP 2 — Stage 1 캡쳐 (대조군: 깨끗한 상태)

**아래 블록을 복사해서 git bash에 붙여넣고 Enter:**

```bash
cd ~/git-noise-demo/stage1-baseline
clear
echo "### STAGE 1: .gitattributes 만 제거 — 깨끗 ###"
git status
```

### 📸 캡쳐 #1
- 출력 전체가 한 화면에 나옴
- 핵심 메시지: `nothing to commit, working tree clean`

---

## STEP 3 — Stage 2 캡쳐 (CRLF 본체) ⭐ 가장 중요

### 📸 캡쳐 #2 — git status + diff stat

```bash
cd ~/git-noise-demo/stage2-crlf
clear
echo "### STAGE 2: 작업 트리 CRLF → 358 파일 modified ###"
echo ""
echo "--- git status (위 20줄) ---"
git status | head -20
echo ""
echo "--- 핵심 증거: insertions == deletions ---"
git diff --stat | tail -3
```

> 핵심 메시지: **`358 files changed, 23517 insertions(+), 23517 deletions(-)`** ← 삽입과 삭제가 정확히 같은 수 = 라인 단위 변경이 아니라 줄 끝 문자만 다름

### 📸 캡쳐 #3 — `cat -A` 로 줄 끝 가시화

```bash
clear
echo "### STAGE 2 증거 #1: cat -A 로 줄 끝 가시화 ###"
echo "(\$=LF 라인종결, ^M\$=CRLF 라인종결)"
echo ""
echo "[삭제(-): HEAD blob의 LF 라인]"
git diff pintos/userprog/syscall.c | grep '^-[^-]' | head -3 | cat -A
echo ""
echo "[삽입(+): 작업 트리의 CRLF 라인]"
git diff pintos/userprog/syscall.c | grep '^+[^+]' | head -3 | cat -A
```

> 핵심 메시지: `+` 라인 끝마다 **`^M$`** 가 보임 = CRLF

### 📸 캡쳐 #4 — hex dump 비교

```bash
clear
echo "### STAGE 2 증거 #2: 바이트 단위 비교 ###"
echo ""
echo "[작업 트리 첫 32바이트]"
head -c 32 pintos/userprog/syscall.c | xxd
echo ""
echo "[HEAD blob 첫 32바이트]"
git show HEAD:pintos/userprog/syscall.c | head -c 32 | xxd
echo ""
echo "→ 작업 트리: ...22 0d 0a... (CRLF)"
echo "→ HEAD blob: ...22 0a... (LF)"
```

> 만약 `xxd: command not found` 에러 나오면 아래 대체 명령으로 다시 실행:
> ```bash
> head -c 32 pintos/userprog/syscall.c | od -An -tx1
> git show HEAD:pintos/userprog/syscall.c | head -c 32 | od -An -tx1
> ```

> 핵심 메시지: `0d 0a` (CRLF) vs `0a` (LF) — 바이트 한 개 차이

---

## STEP 4 — Stage 3 캡쳐 (filemode 본체)

> Windows에선 약하게 나올 수 있으니, modified 가 0개면 이 캡쳐는 스킵하고 STEP 5 로.

### 📸 캡쳐 #5 — filemode 노이즈

```bash
cd ~/git-noise-demo/stage3-filemode
clear
echo "### STAGE 3: chmod -x → mode 변경 노이즈 ###"
echo ""
echo "--- 인덱스 mode 분포 ---"
git ls-files -s | awk '{print $1}' | sort | uniq -c
echo ""
echo "--- git status (위 15줄) ---"
git status | head -15
echo ""
echo "--- diff: 내용 변경 0, 모드만 변경 ---"
git diff pintos/userprog/syscall.c
```

> 핵심 메시지: diff에 `old mode 100755 / new mode 100644` 두 줄만 있고 코드 변경은 없음

---

## STEP 5 — Stage 4 캡쳐 (두 노이즈 결합)

### 📸 캡쳐 #6 — 합쳐진 최악 상태

```bash
cd ~/git-noise-demo/stage4-both
clear
echo "### STAGE 4: filemode + CRLF 동시 → 634 파일 ###"
echo ""
echo "--- git status ---"
git status | head -15
echo ""
echo "--- diff: 모드 변경 + CRLF 한 번에 ---"
git diff pintos/userprog/syscall.c | head -10 | cat -A
```

> 핵심 메시지:
> - 윗부분 `old mode .. new mode ..` (filemode 노이즈)
> - 아랫부분 라인 끝마다 `^M$` (CRLF 노이즈)
> - 한 PR diff에 둘 다 섞여 들어옴 = 발표 슬라이드용 "최악의 상황" 그림

---

## STEP 6 — 정리

캡쳐 다 끝나면:

```bash
rm -rf ~/git-noise-demo
```

---

## 캡쳐 체크리스트

- [ ] **#1** stage1: clean working tree
- [ ] **#2** stage2: 358 files / insertions==deletions
- [ ] **#3** stage2: `cat -A` 로 본 `^M$`
- [ ] **#4** stage2: hex dump `0d 0a` vs `0a`
- [ ] **#5** stage3: `old mode 100755 / new mode 100644` *(Windows에서 약하면 생략)*
- [ ] **#6** stage4: 한 diff 안에 두 노이즈

---

## Q&A

**Q. 어디 디렉토리에서 시작해야 하나요?**
어디든 OK. setup 스크립트가 `~/git-noise-demo` (사용자 홈 폴더 안) 에 알아서 만듭니다.

**Q. clone 이 인증 요청을 띄워요.**
사적인 리포면 GitHub 계정 인증 필요. PAT (personal access token) 또는 SSH 키로 인증 후 다시 setup.

**Q. setup 도중 에러나면?**
처음부터 다시 — 기존 `~/git-noise-demo` 가 자동으로 지워지고 새로 만들어지니 안전합니다.

**Q. stage3 modified 가 0이에요.**
Windows native git의 정상 동작입니다. `core.filemode` 가 Windows에선 잘 동작하지 않아요. CRLF 데모(stage 2, 4)는 멀쩡하니 그것만 캡쳐하시면 충분합니다.

**Q. 캡쳐 다시 찍고 싶은데 stage 가 망가졌어요.**
STEP 1 setup 블록을 한 번 더 실행하면 4개 stage 가 깨끗하게 다시 만들어집니다.
