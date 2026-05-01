### 1. 아래 파일에서 제일 아랫 줄 source ./pintos/activate로 변경
``` bash
sudo vi ~/bashrc
```

### 2. pintos 테스트시 /usr/bin/env: ‘python3\r’: No such file or directory 에러 발생
- .gitattributes 파일 변경
```
# 기본
* text=auto eol=lf

# 절대 깨지면 안되는 것들
*.sh text eol=lf
Makefile text eol=lf
*.mk text eol=lf

# Pintos utils
utils/* text eol=lf

# Python
*.py text eol=lf

# C 파일도 통일 (선택)
*.c text eol=lf
*.h text eol=lf
```

- 변경 내역 반영 후 확인
``` bash
git add --renormalize .
git commit -m "Normalize line endings to LF"

git status

git rm --cached -r .
git reset --hard

git ls-files --eol | grep crlf
git ls-files --eol
```

### 3. pintos 명령어가 무엇?
`QEMU(에뮬레이터) 위에서 Pintos OS를 실행하고, 테스트 프로그램을 같이 돌려주는 명령어`
``` bash
pintos --fs-disk=10 -p tests/userprog/args-single:args-single -- -q -f run 'args-single onearg'
```
- 1. 디스크 이미지 생성 (--fs-disk=10)
- 2. 실행할 테스트 파일 복사 (-p)
- 3. QEMU 실행
- 4. Pintos 커널 부팅
- 5. user program 실행 (run 'args-single onearg')
> 즉, 부팅 + 파일 복사 + 실행 까지 한 번에 처리