# PR Merge 중 발생한 CRLF/LF 노이즈 트러블슈팅

> 같은 코드인데 PR diff에 수백~수천 라인이 "전부 변경됨"으로 떠서 머지가 사실상 불가능했던 사건.
> 원인은 두 가지 — **줄 끝(CRLF↔LF)** + **파일 권한(실행비트)** — 이 동시에 작용하고 있었다.

---

## 1. 무엇이 보였나

머지 PR을 열면 변경하지도 않은 파일이 통째로 modified로 잡혔다. 대표 증상:

- `git diff <PR>` 가 **수만 라인**의 +/- 로 도배됨
- 정작 의미 있는 코드 변경은 그 안에 묻혀 보이지 않음
- 한 쪽 OS에서 보면 깨끗한데, 다른 OS(특히 Windows git bash)에서 보면 "all modified"
- 같은 리포를 두 사람이 각각 클론했을 때 `git status` 결과가 다름

타임라인 (커밋 기준):
- `127b8f6 (2026-04-24) Normalize line endings to LF` — IMGyuGo: 1차 시도
- `bc3d912 (2026-05-01) chore: 줄 끝 정규화용 .gitattributes 추가` — jypark: 본격 fix
- `.git/config` 에 `core.filemode=false` 추가 — 권한 차이도 별도로 봉인

---

## 2. 두 원인을 분리해서 진단

### 원인 A — CRLF/LF (줄 끝 문자)

- **Unix/Linux/macOS**: 줄 끝이 `\n` (LF, 1바이트)
- **Windows**: 줄 끝이 `\r\n` (CRLF, 2바이트)
- git 의 `core.autocrlf` 설정 + `.gitattributes` 부재 → OS 별로 작업 트리에 다른 줄 끝이 풀리거나, 누군가 CRLF 상태로 add → 인덱스에 CRLF 가 박혀 들어감
- 결과: 다른 OS 에서 풀면 "모든 라인이 다르다" 로 보임

### 원인 B — 파일 권한 (실행비트)

- Unix는 파일에 실행비트(`+x`)가 있음. git은 이걸 100644 / 100755 로 추적
- WSL2 가 Windows 마운트(`/mnt/c/...`)를 노출할 때 파일을 일괄 777 로 보여줌
- 누군가 그 마운트에서 add 하면 인덱스에 100755 로 박힘 (이 리포는 **630/635 파일이 100755 로 커밋**되어 있음 — 비정상)
- 다른 환경에서 100644 로 체크아웃되면 모드 차이로 modified 표시

---

## 3. 재현 (시연 캡쳐)

> 샌드박스: `/tmp/git-noise-demo/{stage1..stage4}` — 각각 fresh clone.
> 모든 스테이지는 먼저 `.gitattributes` 를 제거해 봉인을 해제한 상태에서 시작한다.

### Stage 1 — baseline (노이즈 트리거 없음)

`.gitattributes` 만 제거. 이 시점엔 작업 트리가 그대로라 깨끗:

```
$ git status
On branch dev
Your branch is ahead of 'origin/dev' by 1 commit.
nothing to commit, working tree clean
```

### Stage 2 — CRLF 노이즈만

작업 트리의 텍스트 파일들에 `sed -i 's/$/\r/'` 로 CRLF 주입 (Windows 측에서 편집·저장된 상황을 재현):

```
$ git status | head
On branch dev
Changes not staged for commit:
	modified:   .devcontainer/devcontainer.json
	modified:   README.md
	modified:   TIL/alarm_clock_study.md
	... (총 358개) ...

modified file count: 358
```

**diff 통계 — "삽입"과 "삭제"가 똑같다 = 라인 단위로 변경이 아니라 라인 종결자만 다르다는 결정적 증거:**

```
$ git diff --stat | tail -1
358 files changed, 23517 insertions(+), 23517 deletions(-)
```

**`cat -A` 로 줄 끝 문자를 가시화** (`$` = LF, `^M$` = CRLF):

```
$ git diff pintos/userprog/syscall.c | grep '^[-+][^-+]' | head -3 | cat -A
-#include "userprog/syscall.h"$
-#include <stdio.h>$
-#include <syscall-nr.h>$

$ git diff pintos/userprog/syscall.c | grep '^[-+][^-+]' | tail -3 | cat -A
+^I^I^Ithread_exit();^M$
+^I}^M$
+}^M$
```

**hex dump 로 바이트 레벨 비교:**

```
[작업 트리, 첫 줄 끝]
22 0d 0a 23   →  "  \r \n  #     ← CRLF

[HEAD blob, 첫 줄 끝]
22 0a 23 69   →  "  \n  #  i     ← LF
```

### Stage 3 — 파일 권한 노이즈만

`core.filemode=true` + 모든 트래킹 파일에 `chmod -x` (Windows native git 클론처럼 실행비트가 빠진 상황을 재현):

```
$ git ls-files -s | awk '{print $1}' | sort | uniq -c
      5 100644
    630 100755          ← 리포에 100755 로 커밋된 파일 630개

$ git status | head
Changes not staged for commit:
	modified:   .devcontainer/Dockerfile
	modified:   .gitignore
	modified:   pintos/Makefile
	... (총 630개) ...

modified file count: 630
```

**diff 출력은 내용 변경이 0 — 모드 비트만 다르다:**

```
$ git diff pintos/userprog/syscall.c
diff --git a/pintos/userprog/syscall.c b/pintos/userprog/syscall.c
old mode 100755
new mode 100644
```

### Stage 4 — 두 노이즈 동시

```
modified file count: 635
```

```
$ git diff pintos/userprog/syscall.c | head -7 | cat -A
diff --git a/pintos/userprog/syscall.c b/pintos/userprog/syscall.c$
old mode 100755$
new mode 100644$
index d9fd618..3448ca7$
--- a/pintos/userprog/syscall.c$
+++ b/pintos/userprog/syscall.c$
@@ -1,118 +1,118 @@$
```

→ **모드 변경 + 라인 종결자 변경** 두 노이즈가 한 diff 안에서 합쳐서 나타남.

### 각 봉인 한 개씩 풀었을 때 (효과 검증)

| 상태 | modified 개수 |
|---|---|
| 두 노이즈 모두 활성 | **635** |
| `core.filemode=false` 만 끔 | 351 (CRLF 만 남음) |
| `.gitattributes` 복원 + `git add --renormalize .` | ~0 (배경의 진짜 차이만 남음) |

→ **두 봉인 다 필요하다**. 어느 쪽도 없으면 절반의 노이즈가 살아남는다.

---

## 4. 적용된 fix

### A. `.gitattributes` 추가 (커밋 `bc3d912`)

```gitattributes
* text=auto eol=lf

*.c          text eol=lf
*.h          text eol=lf
... (확장자별 명시) ...

*.bin        binary
*.dsk        binary
... (바이너리 명시) ...
```

핵심:
- `text=auto eol=lf` — 텍스트로 감지된 모든 파일은 **저장소에는 LF**, OS 별 작업 트리에서만 필요 시 변환
- 확장자별 `text eol=lf` — auto 감지 실패 대비
- 바이너리 명시 — diff/merge/줄 끝 변환 모두 비활성

### B. `core.filemode=false` (각자 .git/config)

```bash
git config core.filemode false
```

- 작업 트리의 실행비트 차이를 git이 무시
- WSL2 마운트, macOS, Windows 사이를 오갈 때 mode-change 노이즈 차단
- **주의**: 이건 .git/config 에 들어가므로 *각자 한 번씩* 설정해야 함. 리포에 박을 수는 없음

### 적용 순서 (새로 합류한 팀원)

```bash
git pull                              # .gitattributes 받기
git config core.filemode false        # 권한 차이 무시
git rm --cached -r . && git reset --hard   # 작업 트리 재정렬
```

---

## 5. 발표용 요약 슬라이드

> "PR diff 가 수만 라인으로 폭발해서 코드 리뷰가 불가능했다."
>
> **원인**: CRLF/LF 차이 (358 파일, 47k 라인 노이즈) + 파일 권한 차이 (630 파일 mode-only 노이즈)
>
> **해결**:
> 1. `.gitattributes` 로 줄 끝을 LF로 강제 (저장소에 박힘)
> 2. `core.filemode=false` 로 실행비트 차이 무시 (각자 설정)
>
> **검증**: 어느 한 쪽만으로는 노이즈의 절반이 살아남음 → **둘 다 필요**

---

## 6. 부록 — 시연 직접 돌려보기

샌드박스 클론은 `/tmp/git-noise-demo/{stage1-baseline, stage2-crlf, stage3-filemode, stage4-both}` 에 살아 있다. `git status`, `git diff`, `cat -A` 등으로 직접 확인 가능. 정리는:

```bash
rm -rf /tmp/git-noise-demo
```

스테이지를 처음부터 다시 만들고 싶다면 이 문서의 stage 2~4 명령을 그대로 실행. 각 스테이지는 독립된 fresh clone 이라 서로 영향 없음.
