# PR Policy — Pintos Project 1 Branch Merge Strategy

## 팀 구성 및 담당 영역

| 팀 | 멤버 | 담당 파트 |
|---|---|---|
| C+D | 박지용, 박승현 | alarm clock, priority-sema, priority-condvar |
| A+B | 박건우, 김규민 | alarm clock, priority-change, priority-fifo, priority-preempt |

---

## Milestone 구분

| Milestone | 내용 | 상태 |
|---|---|---|
| M1 | alarm clock + priority scheduler (sema, condvar, change, fifo, preempt) | **현재 진행** |
| M2 | priority donation | **jypark 완성본 기준으로 진행** |

> donation PR 현황:
> - **jypark (박지용)**: 완성본 — M2 기준 코드로 채택
> - 나머지 PR: 미완성본 — 코드 스타일 및 장점만 선별 반영
>
> M2 진행 방식: jypark donation 코드를 베이스로,
> 나머지 PR에서 스타일 또는 구현 장점이 있는 부분을 cherry-pick하여 통합한다.

---

## M1 Merge 전략

### 원칙
- 두 브랜치의 **contribute 이력을 보존**한다
- merge 방식은 **squash merge + mix merge 조합**을 사용한다
- 코드 충돌 시 **담당 영역 기준으로 해당 팀 코드를 우선** 채택한다
- donation 코드가 섞여 있는 커밋은 **cherry-pick으로 분리**하여 M2 브랜치로 이동한다

---

### Phase 1 — 사전 준비

```bash
# 각자 최신 상태로 동기화
git fetch origin
git checkout jypark
git pull origin jypark

# 두 브랜치 diff 확인 (충돌 가능 파일 사전 파악)
git diff [A+B브랜치] jypark -- threads/thread.c
git diff [A+B브랜치] jypark -- threads/synch.c
git diff [A+B브랜치] jypark -- threads/thread.h
git diff [A+B브랜치] jypark -- devices/timer.c
```

**확인 포인트:**
- alarm clock 구현 차이 (양쪽 모두 구현한 공통 영역)
- 공통 함수명 충돌 여부 (`cmp_priority` 등)
- donation 관련 코드 범위 파악

---

### Phase 2 — donation 코드 분리

```bash
# M2 브랜치 생성
git checkout -b milestone/m2-donation

# donation 관련 커밋만 cherry-pick
git log --oneline jypark          # 커밋 목록 확인
git cherry-pick [donation 커밋 해시]
```

> M1 merge 전에 반드시 donation 코드를 분리한다.
> donation이 섞인 채로 merge하면 M1 테스트에서 의도치 않은 PASS/FAIL이 발생할 수 있다.

---

### Phase 3 — alarm clock merge (공통 영역)

```bash
git checkout -b merge/m1-alarm-clock

# 양쪽 alarm clock 코드 비교 후 채택 결정
# 합의된 코드로 통일 후 squash merge → main
git checkout main
git merge --squash merge/m1-alarm-clock
git commit -m "feat: alarm clock (squash merge, C+D & A+B)"
```

---

### Phase 4 — 각 담당 영역 merge

```bash
# A+B 담당: change, fifo, preempt
git checkout main
git merge --squash [A+B브랜치의 해당 커밋 범위]
git commit -m "feat: priority-change, fifo, preempt (squash merge, A+B)"

# C+D 담당: sema, condvar
git checkout main
git merge jypark    # mix merge (이력 보존)
# 충돌 발생 시 → 담당 영역 기준으로 해당 팀 코드 채택
```

---

### Phase 5 — 검증

```bash
# M1 대상 테스트 전체 실행
make check

# 확인 대상 테스트
# alarm-*
# priority-change, priority-fifo, priority-preempt
# priority-sema, priority-condvar
# donation 테스트는 FAIL이어도 무방 (M2 대상)
```

---

## M2 브랜치 관리

### 기준 코드: jypark donation (완성본)

```bash
# M1 main 완료 후 M2 브랜치 시작
git checkout main
git checkout -b milestone/m2-donation

# jypark donation 코드를 베이스로 적용
git cherry-pick [jypark donation 커밋 해시들]
```

### 나머지 PR 코드 검토 및 반영

```bash
# 각 PR의 donation 코드 diff 확인
git diff jypark [타브랜치] -- threads/synch.c
git diff jypark [타브랜치] -- threads/thread.c

# 장점 있는 부분만 선별 cherry-pick 또는 수동 반영
```

**검토 기준:**
- 코드 스타일이 더 명확한 부분
- 예외 처리나 엣지 케이스를 더 잘 다룬 부분
- 변수명 또는 함수명이 더 직관적인 부분

> 기능 충돌 시 jypark 완성본 코드를 우선 채택한다.

---

## 충돌 해결 기준

| 파일 | 충돌 시 우선 채택 |
|---|---|
| `devices/timer.c` | 협의 후 결정 (공통 영역) |
| `threads/thread.c` | 해당 함수 담당 팀 코드 |
| `threads/synch.c` | C+D (지용) 코드 |
| `threads/thread.h` | 협의 후 결정 |
| `threads/synch.c` (donation 영역) | jypark 완성본 우선, 타 PR 스타일 선별 반영 |

---

## 커밋 메시지 컨벤션

```
feat: [파트명] 구현 내용 (담당: 팀명)
fix:  [파트명] 버그 수정
merge: [브랜치명] → main ([전략명])
```

예시:
```
feat: priority-sema list_min + cmp_priority 적용 (담당: C+D)
merge: merge/m1-alarm-clock → main (squash merge)
```

---

*작성일: 2026-04-29*
*적용 대상: Pintos Project 1 / jypark 브랜치 기준*