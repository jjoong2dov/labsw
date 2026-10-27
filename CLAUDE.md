# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 프로젝트

labsw: 라즈베리 파이 4 의 Linux bridge(br0)와 FRR 을 "작은 L2/L3 장비"로 보고 NETCONF/YANG 관리면을 붙인 예제 (책 《라즈베리 파이로 만드는 NETCONF/YANG 네트워크 장비》의 실습 저장소, MIT).

- `yang/` 자체 YANG 모듈. `labsw-switch`(포트·VLAN), `labsw-route`(IPv4 정적 경로 → FRR), `labsw-ops`(RPC)
- `agent/` C 에이전트 `labswd`: sysrepo 구독 → 장비 적용. `main.c`(labsw-switch), `routes.c`(labsw-route), `ops.c`(labsw-ops)
  - `core/` sysrepo 를 모르는 순수 로직: 상태(state)·계획(plan)·트랜잭션(txn: 적용·되돌리기·검증), 경로(route, rtxn)
  - `adapter/` 장비 쪽: `exec.c`(argv 실행기), `bridge.c`(iproute2 bridge/ip -j), `frr.c`(vtysh), `mock*.c`(JSON 파일로 흉내 낸 장비, 시험 빌드에만)
- `tests/unit/` cmocka 단위 시험, `tests/int/` pytest+ncclient 통합 시험(실제 sysrepo·netopeer2 + 가짜 장비 또는 실제 bridge)
- `tools/` build_stack.sh(스택 빌드, deps.lock), verify(장비별 시험 실행·요약), sysroot.sh·xbuild.sh·pkg.sh(aarch64 묶음), labsw-upgrade(-check)(스키마 업그레이드), devlab.py·nc.py(PC 에서 갖고 노는 장비와 NETCONF 클라이언트), lab_up/down.sh
- `deploy/` install.sh, systemd 유닛, NACM

## 빌드와 시험 (PC, root 없이)

스택(libyang 5.8.6 / libnetconf2 4.4.10 / sysrepo 5.1.0 / netopeer2 2.8.7)이 `$LABSW_PREFIX`(기본 `~/opt/labsw-host`)에 설치되어 있고, 그 bin·lib 이 PATH·LD_LIBRARY_PATH·PKG_CONFIG_PATH 에 들어 있어야 한다.

```
cmake -S . -B build -DLABSW_TESTING=ON && cmake --build build -j
(cd build && ctest --output-on-failure)            # 단위 시험
cd tests/int && python3 -m pytest -q               # 통합 시험 (mock 장비), 약 2분
cd tests/int && python3 -m pytest -q -k 이름        # 하나만
```

- 통합 시험은 시험마다 sysrepo 저장소·SHM 접두사·포트를 따로 만든다(`tests/int/conftest.py` 의 `Lab`). 실행 중인 다른 netopeer2/labswd 를 건드리지 않는다.
- 시험 빌드(`LABSW_TESTING=ON`)에만 mock 장비와 결함 주입 옵션이 있다: `labswd -a mock -m state.json [-r routes.json] [-t trace.txt] [-F n] [-G n] [-K bytes]`
- 실제 bridge 대상(VM·파이, root): `tools/verify --target remote ...` (`LABSW_TARGET=bridge`)
- 메모리 검사: `-DLABSW_ASAN=ON` 으로 따로 빌드하고 `LABSW_BUILD=<그 빌드>` 로 통합 시험

## 규칙

- C11, 경고 없이 빌드. 외부 명령은 `adapter/exec.c` 로 argv 실행(셸 금지), 시간 제한·출력 상한.
- sysrepo 변경 콜백 안에서 장비를 바꾼다(CHANGE). 실패하면 그 콜백 안에서 되돌리고 오류를 NETCONF error 로(`sr_session_set_netconf_error`), 다른 구독자의 거부는 ABORT 에서 되돌린다. "되돌렸다"는 다시 읽어 확인한 경우에만.
- 잠금 순서: sysrepo 를 부르는 동안 `txn_lock` 을 잡지 않는다(콜백이 sysrepo 잠금을 쥔 채 txn_lock 을 기다리므로).
- oper 트리와 알림은 `lyd_new_*` 로 키를 값으로 넘긴다(XPath 문자열 조립 금지).
- 새 동작에는 시험을 붙인다. 통합 시험은 mock·bridge 두 대상에서 같은 시나리오가 돌게 장치 메서드(`lab.ports()`, `lab.dev.set_ports()` 등)만 쓴다.
- 파일 머리에 한 줄 설명과 `SPDX-License-Identifier: MIT`. 코드 주석은 영어, 짧게, "왜"만.
- YANG 모듈을 바꾸면 revision 을 올리고 파일 이름(`모듈@날짜.yang`)도 바꾼다.
