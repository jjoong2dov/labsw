# labsw — 라즈베리 파이로 만드는 작은 NETCONF/YANG 장비

전자책 《라즈베리 파이로 만드는 NETCONF/YANG 네트워크 장비》(《라즈베리 파이 × AI 실전 개발실》 3권)의 실습 저장소입니다.

라즈베리 파이 4 의 리눅스 브리지(br0)와 FRR 을 "작은 L2/L3 장비"로 보고, 그 위에 NETCONF/YANG 관리면을 붙입니다.

- YANG 모듈: `labsw-switch`(포트·VLAN, 운용 상태, 알림), `labsw-route`(IPv4 정적 경로), `labsw-ops`(백업·복원 RPC)
- C 에이전트 `labswd`: sysrepo 변경을 받아 브리지(`bridge`/`ip`)와 FRR(`vtysh`)에 적용하고, 실패하면 되돌린 뒤 NETCONF 오류로 알립니다. NETCONF 밖에서 바뀐 설정을 주기적으로 되돌리고(`-R`), 운용 상태와 알림을 냅니다.
- 시험: 단위(cmocka), 통합(pytest + ncclient, 실제 sysrepo·netopeer2 + 가짜 장비 또는 실제 브리지), 장시간 시험, 결함 주입
- 도구: 스택 빌드(`tools/build_stack.sh`), 크로스 빌드·배포 묶음(`tools/xbuild.sh`, `tools/pkg.sh`, `deploy/install.sh`), 장비별 시험(`tools/verify`), 스키마 업그레이드(`tools/labsw-upgrade`), PC 에서 갖고 노는 장비(`tools/devlab.py`, `tools/nc.py`)

NETCONF 스택은 `deps.lock` 의 버전으로 고정합니다: libyang 5.8.6, libnetconf2 4.4.10, sysrepo 5.1.0, netopeer2 2.8.7.

## PC 에서 시작하기 (root 없이)

Debian 13 / Ubuntu 24.04 기준입니다. 빌드에 필요한 패키지:

```sh
sudo apt-get install build-essential cmake pkg-config git libpcre2-dev libssh-dev libssl-dev \
    libcurl4-openssl-dev libjson-c-dev libcmocka-dev python3-pytest python3-ncclient
```

스택을 `~/opt/labsw-host` 에 빌드하고, labsw 를 시험 빌드로 만듭니다.

```sh
tools/build_stack.sh host                     # 네 프로젝트를 차례로, 몇 분 걸립니다
export PATH=~/opt/labsw-host/bin:~/opt/labsw-host/sbin:$PATH
export LD_LIBRARY_PATH=~/opt/labsw-host/lib PKG_CONFIG_PATH=~/opt/labsw-host/lib/pkgconfig
cmake -S . -B build -DLABSW_TESTING=ON && cmake --build build -j
(cd build && ctest)                           # 단위 시험
cd tests/int && python3 -m pytest -q          # 통합 시험 (약 2분), 끝나면 cd ../..
```

통합 시험 결과는 "45 passed, 2 skipped" 처럼 passed 가 대부분이어야 합니다(건너뛰는 둘은 실제 브리지 전용 시험과 장시간 시험). skipped 만 잔뜩 나오면 통과가 아니라 환경 문제이니 `python3 -m pytest -q -rs` 로 이유를 보세요.

갖고 놀 장비 하나 띄우기: NETCONF 서버(127.0.0.1:8301)와 에이전트, 포트 p1·p2·p3 를 흉내 내는 가짜 장비입니다.

```sh
tools/devlab.py start
tools/nc.py hello
echo '<switch xmlns="urn:example:labsw-switch"><vlan><vid>10</vid></vlan>
  <port><name>p1</name><vlan><vid>10</vid><pvid>true</pvid><egress>untagged</egress></vlan></port></switch>' | tools/nc.py edit -
cat ~/.labsw-devlab/trace.txt                # 에이전트가 장비에 내린 명령
tools/nc.py get '<switch-state xmlns="urn:example:labsw-switch"/>'
tools/devlab.py stop
```

## 라즈베리 파이(또는 arm64 VM)에 설치하기

PC 에서 크로스 빌드해 묶음 하나를 만들고, 장비에서 `install.sh` 로 설치합니다. 장비: Raspberry Pi OS(64-bit, Debian 13 기반) 또는 Debian 13 arm64.

```sh
# 1) 장비의 헤더·라이브러리를 PC 로 (장비에 먼저: sudo apt-get install rsync libpcre2-dev libssh-dev libssl-dev libcurl4-openssl-dev libjson-c-dev libsystemd-dev)
tools/sysroot.sh pi@<장비> ~/labsw-sysroot
# 2) 스택과 labsw 를 aarch64 로 (PC 에 gcc-aarch64-linux-gnu, qemu-user 필요)
XB_SYSROOT=~/labsw-sysroot tools/build_stack.sh aarch64 ~/labsw-stage
XB_SYSROOT=~/labsw-sysroot XB_STAGE=~/labsw-stage LABSW_TESTING=OFF tools/xbuild.sh build-aarch64-rel
tools/pkg.sh ~/labsw-stage build-aarch64-rel   # -> dist/labsw-bundle.tgz
# 3) 장비에서
scp dist/labsw-bundle.tgz pi@<장비>: && ssh pi@<장비> 'tar xzf labsw-bundle.tgz && sudo bash labsw/install.sh pi'
```

`install.sh` 는 스택을 `/opt/labsw` 에 풀고, YANG 모듈을 등록하거나(새 revision 이면 `labsw-upgrade` 로 미리 확인한 뒤) 올리고, 실습용 포트(br0, p1~p4, 호스트 h1~h4)와 서비스 세 개(`labsw-lab`, `netopeer2`, `labswd`)를 켭니다. NETCONF 는 830 포트, 인자로 준 계정의 `~/.ssh/authorized_keys` 키로 들어갑니다.

장비에서 시험(실제 브리지·FRR, root): PC 에서 `tools/verify --target remote --ssh ... --scp ... --dest ...` (`tools/verify -h`).

## 저장소 안내

| 경로 | 내용 |
|---|---|
| `yang/` | YANG 모듈 |
| `agent/main.c`, `routes.c`, `ops.c` | labswd: labsw-switch, labsw-route, labsw-ops |
| `agent/core/` | sysrepo 를 모르는 로직(상태, 계획, 적용·되돌리기·검증) |
| `agent/adapter/` | 명령 실행기, 브리지·FRR·가짜 장비 |
| `tests/unit`, `tests/int` | 단위·통합 시험 (`tests/int/conftest.py` 가 시험마다 저장소·포트를 따로 만듦) |
| `tests/fixtures/` | 실제 장비에서 받은 출력 견본, 업그레이드 예제 |
| `tools/`, `deploy/` | 빌드·배포·시험·업그레이드 도구, systemd 유닛 |

## 라이선스

labsw 는 MIT 라이선스입니다(`LICENSE`). 함께 쓰는 것:

- libyang, libnetconf2, sysrepo, netopeer2: BSD-3-Clause (CESNET 등). 배포 묶음에 빌드한 바이너리가 들어갑니다.
- FRRouting: GPL-2.0-or-later. 장비의 패키지를 그대로 쓰고, labswd 는 `vtysh` 를 별도 프로세스로 부르기만 합니다.
- iproute2(`bridge`, `ip`): 장비의 패키지를 별도 프로세스로 부릅니다.
- json-c(MIT), cmocka(Apache-2.0, 시험에만), ncclient(Apache-2.0, 시험·도구), pytest(MIT, 시험)
