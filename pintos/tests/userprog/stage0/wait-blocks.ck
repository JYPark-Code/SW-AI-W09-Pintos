# -*- perl -*-
# stage 0: process_wait 영구 블록 검증.
#
# 통과 조건:
#   1. 자식 마커가 출력에 등장 (자식이 정상 실행됐다)
#   2. "Powering off" 가 출력에 등장하지 않음 (kernel이 power_off로
#      가지 못했다 = 부모가 process_wait에서 블록 중)
#
# 실패 시나리오:
#   - 마커 부재: 자식이 실행 자체를 못 했거나 write가 실패
#   - "Powering off" 등장: process_wait stub이 즉시 반환하여 init이
#     main 끝까지 도달, kernel을 종료시킴 → 블록 동작 안 함

use strict;
use warnings;
use tests::tests;

our ($test);
my (@output) = read_text_file ("$test.output");

fail "child marker missing — write failed or kernel terminated too early\n"
  if !grep (/STAGE0_WAIT_CHILD_DONE/, @output);

fail "kernel powered off — process_wait did NOT block (stub broken)\n"
  if grep (/Powering off/, @output);

pass;
